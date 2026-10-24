#!/usr/bin/env python3
import csv
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "sky_inventory.py"
SPEC = importlib.util.spec_from_file_location("sky_inventory", SCRIPT)
sky = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sky
SPEC.loader.exec_module(sky)


def literal_lz4(data):
    length = len(data)
    result = bytearray([min(length, 15) << 4])
    if length >= 15:
        remaining = length - 15
        while remaining >= 255:
            result.append(255)
            remaining -= 255
        result.append(remaining)
    result.extend(data)
    return bytes(result)


def encode_adrp(register, pc, target):
    pages = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    pages &= (1 << 21) - 1
    return (
        0x90000000
        | ((pages & 3) << 29)
        | (((pages >> 2) & 0x7FFFF) << 5)
        | register
    )


def encode_add_immediate(destination, source, immediate):
    return 0x91000000 | (immediate << 10) | (source << 5) | destination


def make_nso(exports, imports, relocations=(), strings=(), compressed=False, build_id=None):
    symbols = [(None, False, 0, 0, 0, 0)]
    symbols.extend((name, True, value, 4, 1, 2) for name, value in exports)
    symbols.extend((name, False, 0, 0, 1, 2) for name in imports)
    symbol_indexes = {name: index for index, (name, *_) in enumerate(symbols) if name}

    string_table = bytearray(b"\0")
    name_offsets = {None: 0}
    for name, *_ in symbols[1:]:
        name_offsets[name] = len(string_table)
        string_table.extend(name.encode("ascii") + b"\0")
    symbol_table = bytearray()
    for name, defined, value, size, binding, symbol_type in symbols:
        if name is None:
            symbol_table.extend(bytes(24))
            continue
        symbol_table.extend(struct.pack(
            "<IBBHQQ",
            name_offsets[name],
            (binding << 4) | symbol_type,
            0,
            1 if defined else 0,
            value,
            size,
        ))

    mod0_offset = 0x100
    dynamic_offset = 0x140
    symtab_offset = 0x300
    strtab_offset = symtab_offset + len(symbol_table)
    cursor = (strtab_offset + len(string_table) + 7) & ~7
    encoded_relocations = {}
    for table in ("RELA", "JMPREL"):
        table_data = bytearray()
        for relocation_table, type_id, symbol_name, addend in relocations:
            if relocation_table != table:
                continue
            symbol_index = symbol_indexes[symbol_name] if symbol_name else 0
            table_data.extend(struct.pack(
                "<QQq",
                0x2000 + len(table_data),
                (symbol_index << 32) | type_id,
                addend,
            ))
        encoded_relocations[table] = (cursor, bytes(table_data))
        cursor += len(table_data)
        cursor = (cursor + 7) & ~7
    extra_offset = cursor
    extra = b"\0".join(strings) + (b"\0" if strings else b"")
    image = bytearray(extra_offset + len(extra))
    image[symtab_offset:strtab_offset] = symbol_table
    image[strtab_offset:strtab_offset + len(string_table)] = string_table
    for _, (offset, data) in encoded_relocations.items():
        image[offset:offset + len(data)] = data
    image[extra_offset:extra_offset + len(extra)] = extra

    struct.pack_into("<I", image, 4, mod0_offset)
    image[mod0_offset:mod0_offset + 4] = b"MOD0"
    struct.pack_into("<i", image, mod0_offset + 4, dynamic_offset - mod0_offset)
    dynamic = [
        (sky.DT_STRTAB, strtab_offset),
        (sky.DT_STRSZ, len(string_table)),
        (sky.DT_SYMTAB, symtab_offset),
        (sky.DT_SYMENT, 24),
        (sky.DT_RELAENT, 24),
    ]
    rela_offset, rela_data = encoded_relocations["RELA"]
    if rela_data:
        dynamic.extend(((sky.DT_RELA, rela_offset), (sky.DT_RELASZ, len(rela_data))))
    jmprel_offset, jmprel_data = encoded_relocations["JMPREL"]
    if jmprel_data:
        dynamic.extend((
            (sky.DT_JMPREL, jmprel_offset),
            (sky.DT_PLTRELSZ, len(jmprel_data)),
            (sky.DT_PLTREL, sky.DT_RELA),
        ))
    dynamic.append((0, 0))
    for index, (tag, value) in enumerate(dynamic):
        struct.pack_into("<qQ", image, dynamic_offset + index * 16, tag, value)

    stored = literal_lz4(image) if compressed else bytes(image)
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 0xC, 1 if compressed else 0)
    segment_values = (
        (0x100, 0, len(image)),
        (0x100 + len(stored), len(image), 0),
        (0x100 + len(stored), len(image), 0),
    )
    for index, values in enumerate(segment_values):
        struct.pack_into("<III", header, 0x10 + index * 0x10, *values)
    if build_id is None:
        build_id = bytes(range(32))
    header[0x40:0x60] = build_id
    struct.pack_into("<III", header, 0x60, len(stored), 0, 0)
    return bytes(header) + stored


class SkyInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.exefs = self.root / "ExeFS"
        self.output = self.root / "meta"
        self.exefs.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def rows(self, name):
        with (self.output / name).open(newline="", encoding="utf-8") as stream:
            return list(csv.DictReader(stream, delimiter="\t"))

    def test_lz4_match_and_truncation(self):
        block = b"\x44abcd\x04\x00" + b"\x30XYZ"
        self.assertEqual(
            sky.decompress_lz4_block(block, 15),
            b"abcdabcdabcdXYZ",
        )
        with self.assertRaises(sky.InventoryError):
            sky.decompress_lz4_block(block[:-1], 15)

    def test_adjacent_adrp_add_string_xref(self):
        image = bytearray(0x5000)
        pc = 0x100
        target = 0x4123
        struct.pack_into("<I", image, pc, encode_adrp(8, pc, target))
        struct.pack_into("<I", image, pc + 4, encode_add_immediate(8, 8, 0x123))
        struct.pack_into("<I", image, pc + 8, encode_add_immediate(8, 8, 0x123))
        xrefs = sky.find_code_xrefs(image, 0, 0x1000, {target})
        self.assertEqual(dict(xrefs), {target: [pc]})

    def test_inventory_with_synthetic_modules(self):
        orion_build_id = bytes(range(32))
        support_build_id = bytes(reversed(range(32)))
        orion = make_nso(
            exports=(("nnMain", 0x1000), ("_init_libc2", 0x1100)),
            imports=("nvnBootstrapLoader", "supportFunction", "orphan"),
            relocations=(
                ("RELA", sky.R_AARCH64_RELATIVE, None, 16),
                ("RELA", sky.R_AARCH64_ABS64, "nvnBootstrapLoader", 0),
                ("RELA", sky.R_AARCH64_GLOB_DAT, "supportFunction", 0),
                ("RELA", 261, "orphan", 0),
                ("JMPREL", sky.R_AARCH64_JUMP_SLOT, "nvnBootstrapLoader", 0),
            ),
            strings=(b"nvnQueuePresent", b"intro.bik", b"_init_libc0"),
            compressed=True,
            build_id=orion_build_id,
        )
        support = make_nso(
            exports=(
                ("nvnBootstrapLoader", 0x1200),
                ("nninitStartup", 0x1300),
                ("supportFunction", 0x1400),
            ),
            imports=(),
            compressed=False,
            build_id=support_build_id,
        )
        (self.exefs / "orion").write_bytes(orion)
        (self.exefs / "support").write_bytes(support)
        (self.exefs / "main.npdm").write_bytes(b"META synthetic fixture")
        (self.exefs / "README").write_text("not a module", encoding="utf-8")

        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(self.exefs), str(self.output)],
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("UNSUPPORTED orion RELA PREL32 count=1", result.stderr)

        modules = self.rows("modules.tsv")
        self.assertEqual([row["filename"] for row in modules], ["orion", "support"])
        orion_row = modules[0]
        self.assertEqual(orion_row["sha256"], hashlib.sha256(orion).hexdigest())
        self.assertEqual(orion_row["build_id"], orion_build_id.hex())
        self.assertEqual(orion_row["compressed_segments"], "text")
        self.assertEqual(orion_row["dependencies"], "support")
        self.assertEqual(orion_row["unsupported_relocation_count"], "1")
        self.assertEqual(modules[1]["build_id"], support_build_id.hex())

        imports = {(row["module"], row["name"]): row for row in self.rows("imports.tsv")}
        self.assertEqual(imports[("orion", "nvnBootstrapLoader")]["provider_modules"], "support")
        self.assertEqual(imports[("orion", "nvnBootstrapLoader")]["relocation_count"], "2")
        self.assertEqual(imports[("orion", "supportFunction")]["resolution"], "resolved")
        self.assertEqual(imports[("orion", "orphan")]["resolution"], "unresolved")

        dependencies = self.rows("dependencies.tsv")
        self.assertEqual(dependencies, [{
            "module": "orion",
            "provider_module": "support",
            "resolved_import_count": "2",
        }])

        relocations = {(row["table"], row["type_name"]): row for row in self.rows("relocations.tsv")}
        self.assertEqual(relocations[("RELA", "PREL32")]["loader_supported"], "NO")
        self.assertEqual(relocations[("RELA", "ABS64")]["loader_action"], "resolved symbol + addend (ABS64)")
        self.assertEqual(relocations[("RELA", "GLOB_DAT")]["loader_action"], "resolved symbol + addend (GLOB_DAT)")
        self.assertNotEqual(relocations[("RELA", "ABS64")]["type_id"], relocations[("RELA", "GLOB_DAT")]["type_id"])

        startup = {row["symbol"]: row for row in self.rows("startup.tsv")}
        self.assertEqual(startup["nnMain"]["export_modules"], "orion")
        self.assertEqual(startup["nnMain"]["loader_available"], "yes")
        self.assertEqual(startup["nninitStartup"]["export_modules"], "support")
        self.assertEqual(startup["_init_libc0"]["status"], "string-only")
        self.assertEqual(startup["nnosInitialize"]["status"], "absent-from-inventory")

        evidence = self.rows("evidence.tsv")
        self.assertTrue(any(
            row["category"] == "nvn" and row["evidence"] == "string" and row["value"] == "nvnQueuePresent"
            for row in evidence
        ))
        self.assertTrue(any(row["category"] == "movie" and "intro.bik" in row["value"] for row in evidence))
        inputs = {row["filename"]: row for row in self.rows("inputs.tsv")}
        self.assertEqual(inputs["main.npdm"]["kind"], "NPDM")
        self.assertEqual(inputs["README"]["kind"], "other")

        startup_report = (self.output / "startup.txt").read_text(encoding="utf-8")
        renderer_report = (self.output / "renderer.txt").read_text(encoding="utf-8")
        self.assertIn("Relocation blockers:", startup_report)
        self.assertIn("PREL32", startup_report)
        self.assertIn("nvnBootstrapLoader linkage is concrete evidence", renderer_report)
        self.assertIn("RomFS was not inspected", renderer_report)
        self.assertIn("not definitively identified", renderer_report)

    def test_patched_memory_image_inventory(self):
        build_id = bytes(range(32))
        reference_data = make_nso(
            exports=(("otherFunction", 0x1000),),
            imports=("supportFunction", "orphan"),
            relocations=(
                ("RELA", sky.R_AARCH64_RELATIVE, None, 16),
                ("RELA", sky.R_AARCH64_GLOB_DAT, "supportFunction", 0),
                ("RELA", sky.R_AARCH64_ABS64, "orphan", 0),
                ("JMPREL", sky.R_AARCH64_JUMP_SLOT, "supportFunction", 0),
            ),
            compressed=False,
            build_id=build_id,
        )
        reference_path = self.root / "reference"
        reference_path.write_bytes(reference_data)
        reference = sky.parse_module(reference_path)
        raw = reference_data[0x100:]
        patched_path = self.exefs / "main.patched.bin"
        patched_path.write_bytes(raw)
        (self.exefs / "main.fixups.tsv").write_text(
            f"# NSO_BUILD_ID\t{build_id.hex()}\n"
            "# TEXT_END\t0x100\n"
            "# RO_OFF\t0x100\n"
            "# DATA_OFF\t0x200\n"
            f"# PATCH_OFF\t{len(raw):#x}\n",
            encoding="utf-8",
        )
        (self.exefs / "support").write_bytes(make_nso(
            exports=(("supportFunction", 0x2000),),
            imports=(),
            compressed=False,
            build_id=bytes(reversed(range(32))),
        ))

        patched = sky.parse_patched_module(patched_path)
        self.assertEqual(patched.filename, "main")
        self.assertEqual(patched.relocations, reference.relocations)
        self.assertEqual(patched.symbols, reference.symbols)
        self.assertEqual(patched.needed, reference.needed)
        self.assertEqual(patched.build_id, build_id.hex())

        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(self.exefs), str(self.output)],
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        modules = self.rows("modules.tsv")
        patched_row = next(row for row in modules if row["filename"] == "main")
        self.assertEqual(patched_row["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(patched_row["build_id"], build_id.hex())
        self.assertEqual(patched_row["file_bytes"], str(len(raw)))
        self.assertEqual(patched_row["image_bytes"], str(len(raw)))
        self.assertEqual(patched_row["bss_bytes"], "0")
        self.assertEqual(patched_row["compressed_segments"], "")
        self.assertEqual(patched_row["relocation_count"], "4")
        self.assertEqual(patched_row["unsupported_relocation_count"], "0")

        imports = {(row["module"], row["name"]): row for row in self.rows("imports.tsv")}
        self.assertEqual(imports[("main", "supportFunction")]["provider_modules"], "support")
        self.assertEqual(imports[("main", "supportFunction")]["resolution"], "resolved")
        self.assertEqual(imports[("main", "orphan")]["resolution"], "unresolved")
        self.assertEqual(self.rows("dependencies.tsv"), [{
            "module": "main",
            "provider_module": "support",
            "resolved_import_count": "1",
        }])
        inputs = {row["filename"]: row for row in self.rows("inputs.tsv")}
        self.assertEqual(inputs["main.patched.bin"]["kind"], "patched-memory-image")

    def test_patched_memory_image_rejects_duplicate_module_name(self):
        build_id = bytes(range(32))
        nso = make_nso(exports=(("supportFunction", 0x1000),), imports=(), compressed=False, build_id=build_id)
        raw = nso[0x100:]
        (self.exefs / "main").write_bytes(nso)
        (self.exefs / "main.patched.bin").write_bytes(raw)
        (self.exefs / "main.fixups.tsv").write_text(
            f"# NSO_BUILD_ID\t{build_id.hex()}\n"
            "# TEXT_END\t0x100\n"
            "# RO_OFF\t0x100\n"
            "# DATA_OFF\t0x200\n"
            f"# PATCH_OFF\t{len(raw):#x}\n",
            encoding="utf-8",
        )
        with self.assertRaises(sky.InventoryError):
            sky.inventory(self.exefs, self.output)

    def test_patched_memory_image_rejects_invalid_fixups(self):
        raw = make_nso(exports=(("supportFunction", 0x1000),), imports=(), compressed=False)[0x100:]
        patched_path = self.exefs / "main.patched.bin"
        patched_path.write_bytes(raw)
        fixups_path = self.exefs / "main.fixups.tsv"
        with self.assertRaises(sky.InventoryError):
            sky.parse_patched_module(patched_path)

        build_id = bytes(range(32)).hex()
        fixups_path.write_text(
            "# NSO_BUILD_ID\tnot-a-build-id\n"
            "# TEXT_END\t0x100\n"
            "# RO_OFF\t0x100\n"
            "# DATA_OFF\t0x200\n"
            f"# PATCH_OFF\t{len(raw):#x}\n",
            encoding="utf-8",
        )
        with self.assertRaises(sky.InventoryError):
            sky.parse_patched_module(patched_path)

        fixups_path.write_text(
            f"# NSO_BUILD_ID\t{build_id}\n"
            "# TEXT_END\t0x100\n"
            "# RO_OFF\t0x100\n"
            f"# DATA_OFF\t{len(raw) + 1:#x}\n"
            f"# PATCH_OFF\t{len(raw):#x}\n",
            encoding="utf-8",
        )
        with self.assertRaises(sky.InventoryError):
            sky.parse_patched_module(patched_path)


if __name__ == "__main__":
    unittest.main()
