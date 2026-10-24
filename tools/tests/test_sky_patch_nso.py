#!/usr/bin/env python3
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "sky_patch_nso.py"
NOP = 0xD503201F
TEXT_SIZE = 0x1000
MOD0_OFFSET = 0x1000
EH_HEADER_OFFSET = 0x1040
RO_SIZE = 0x1000
DATA_OFFSET = 0x2000
DATA_SIZE = 0x100
BSS_SIZE = 0x100


def branch(source, target):
    offset = target - source
    assert offset % 4 == 0 and -(1 << 27) <= offset < (1 << 27)
    return 0x14000000 | ((offset >> 2) & 0x3FFFFFF)


def adr(register, source, target):
    immediate = target - source
    assert -(1 << 20) <= immediate < (1 << 20)
    return (
        0x10000000
        | ((immediate & 3) << 29)
        | (((immediate >> 2) & 0x7FFFF) << 5)
        | register
    )


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


def make_nso(words=None, starts=(0x100,), compressed=False, eh_encoding=0x1B):
    words = words or {}
    text = bytearray(struct.pack("<I", NOP) * (TEXT_SIZE // 4))
    for offset, word in words.items():
        struct.pack_into("<I", text, offset, word)
    struct.pack_into("<I", text, 4, MOD0_OFFSET)

    rodata = bytearray(RO_SIZE)
    mod0 = MOD0_OFFSET - TEXT_SIZE
    eh_header = EH_HEADER_OFFSET - TEXT_SIZE
    rodata[mod0:mod0 + 4] = b"MOD0"
    struct.pack_into("<i", rodata, mod0 + 4, 24)
    struct.pack_into("<i", rodata, mod0 + 16, EH_HEADER_OFFSET - MOD0_OFFSET)
    rodata[eh_header:eh_header + 4] = bytes((1, eh_encoding, 0x03, 0x3B))
    struct.pack_into("<i", rodata, eh_header + 4, 0)
    struct.pack_into("<I", rodata, eh_header + 8, len(starts))
    for index, start in enumerate(starts):
        struct.pack_into(
            "<ii",
            rodata,
            eh_header + 12 + index * 8,
            start - EH_HEADER_OFFSET,
            -EH_HEADER_OFFSET,
        )

    data = bytes((index * 37 + 11) & 0xFF for index in range(DATA_SIZE))
    image = bytes(text + rodata + data + bytes(BSS_SIZE))
    stored_text = literal_lz4(text) if compressed else bytes(text)
    stored_segments = (stored_text, bytes(rodata), data)
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 0x0C, 1 if compressed else 0)
    file_offset = len(header)
    memory_offsets = (0, MOD0_OFFSET, DATA_OFFSET)
    sizes = (TEXT_SIZE, RO_SIZE, DATA_SIZE)
    for index in range(3):
        struct.pack_into(
            "<III",
            header,
            0x10 + index * 0x10,
            file_offset,
            memory_offsets[index],
            sizes[index],
        )
        file_offset += len(stored_segments[index])
    struct.pack_into("<I", header, 0x3C, BSS_SIZE)
    header[0x40:0x60] = bytes(range(32))
    struct.pack_into(
        "<III",
        header,
        0x60,
        len(stored_text),
        RO_SIZE,
        DATA_SIZE,
    )
    return bytes(header) + b"".join(stored_segments), image


def make_control_nacp(size=0x2800000, journal=0x1800000,
                      maximum=0x1E000000, maximum_journal=0x1800000):
    data = bytearray(0x4000)
    for offset, value in ((0x3080, size), (0x3088, journal),
                          (0x3148, maximum), (0x3150, maximum_journal)):
        struct.pack_into("<q", data, offset, value)
    return bytes(data)


def read_words(data, start=0, end=None):
    end = len(data) if end is None else end
    return {
        offset: struct.unpack_from("<I", data, offset)[0]
        for offset in range(start, end, 4)
    }


def branch_target(data, offset):
    word = struct.unpack_from("<I", data, offset)[0]
    if word & 0xFC000000 != 0x14000000:
        return None
    immediate = word & 0x3FFFFFF
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return offset + immediate * 4


class SkyPatchNsoTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_patch(self, fixture, output_name="out", module="main", extra_env=None,
                  control_nacp=None):
        source = self.root / (output_name + ".nso")
        output = self.root / output_name
        source.write_bytes(fixture)
        environment = os.environ.copy()
        if extra_env:
            environment.update(extra_env)
        command = [sys.executable, str(SCRIPT), str(source), str(output), module]
        if control_nacp is not None:
            control_path = self.root / (output_name + ".nacp")
            control_path.write_bytes(control_nacp)
            command.extend(("--control-nacp", str(control_path)))
        result = subprocess.run(
            command,
            text=True,
            capture_output=True,
            env=environment,
            timeout=60,
            check=False,
        )
        return result, output

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_generic_rewrites_layout_and_empty_game_rules(self):
        fixture, original = make_nso(
            words={
                0x100: 0xAA1203E0,  # mov x0, x18
                0x104: 0xD53BD041,  # mrs x1, tpidr_el0
                0x108: 0xD51BD042,  # msr tpidr_el0, x2
                0x10C: 0xD53BD063,  # mrs x3, tpidrro_el0
                0x110: 0xD4002461,  # svc #0x123
                0x114: 0xD53BE024,  # mrs x4, cntpct_el0
                0x118: 0xD53BE045,  # mrs x5, cntvct_el0
                0x11C: 0xB4000052,  # cbz x18, #0x124
                0x120: adr(18, 0x120, 0x800),
            },
            compressed=True,
        )
        hostile_environment = {
            "DM_RULESET": "touch-run-v4",
            "DM_PRIVATE_GATES": "1",
        }
        result, output = self.run_patch(fixture, extra_env=hostile_environment)
        self.assert_success(result)
        baseline_result, baseline = self.run_patch(fixture, "baseline")
        self.assert_success(baseline_result)

        expected_names = {
            "main.patched.bin",
            "main.fixups.tsv",
            "main.gates.tsv",
            "main.sites.tsv",
            "main.cuts.tsv",
            "main.obs.tsv",
            "main.params.tsv",
            "build_src.sha256",
        }
        self.assertEqual({path.name for path in output.iterdir()}, expected_names)
        self.assertEqual(
            {path.name: path.read_bytes() for path in output.iterdir()},
            {path.name: path.read_bytes() for path in baseline.iterdir()},
        )
        for extension in ("cuts", "obs", "params"):
            self.assertEqual((output / f"main.{extension}.tsv").read_bytes(), b"")

        fixups = (output / "main.fixups.tsv").read_text(encoding="utf-8")
        self.assertIn("# PATCH_OFF\t0x10000\n", fixups)
        self.assertIn("# KEY_PAGE_OFF\t0x4000\n", fixups)
        self.assertIn("# TEXT_END\t0x1000\n", fixups)
        self.assertIn("# KEY_K18_IDX\t0\n", fixups)
        self.assertIn("# KEY_KGATE_IDX\t3\n", fixups)
        self.assertIn("# KEY_KASPECTUI_IDX\t7\n", fixups)

        sites = {}
        for line in (output / "main.sites.tsv").read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            sites[int(fields[0], 16)] = fields[1]
        self.assertEqual(sites, {
            0x100: "x18",
            0x104: "tpidr_r",
            0x108: "tpidr_w",
            0x10C: "tpidrro_r",
            0x110: "svc",
            0x114: "cntpct",
            0x118: "cntvct",
            0x11C: "x18",
            0x120: "x18",
        })
        gates = [line.split("\t") for line in (output / "main.gates.tsv").read_text(encoding="utf-8").splitlines()]
        self.assertEqual([(row[1], int(row[2]), int(row[3], 16)) for row in gates], [
            ("svc", 0x123, 0x110),
            ("cntpct", 4, 0x114),
            ("cntvct", 5, 0x118),
        ])

        patched = (output / "main.patched.bin").read_bytes()
        self.assertGreater(len(patched), 0x10000)
        self.assertEqual(patched[TEXT_SIZE:len(original)], original[TEXT_SIZE:])
        self.assertEqual(patched[len(original):0x10000], bytes(0x10000 - len(original)))
        original_words = read_words(original, 0, TEXT_SIZE)
        patched_words = read_words(patched, 0, TEXT_SIZE)
        for offset in original_words:
            if offset not in sites:
                self.assertEqual(patched_words[offset], original_words[offset], hex(offset))
                continue
            target = branch_target(patched, offset)
            self.assertIsNotNone(target, hex(offset))
            self.assertGreaterEqual(target, 0x10000)
            self.assertLess(target, len(patched))
            self.assertNotEqual(patched_words[offset], original_words[offset])

    def test_jump_table_inventory(self):
        table = 0x200
        words = {
            0x100: adr(18, 0x100, table),
            0x104: 0x39400020,  # ldrb w0, [x1]
            0x108: 0x8B000A52,  # add x18, x18, x0, lsl #2
            0x10C: 0xD61F0240,  # br x18
        }
        for index in range(256):
            words[table + index * 4] = branch(table + index * 4, 0x800)
        fixture, _ = make_nso(words=words)
        result, output = self.run_patch(fixture)

        self.assert_success(result)
        site_lines = (output / "main.sites.tsv").read_text(encoding="utf-8").splitlines()
        self.assertIn("0x10c\tx18", site_lines)
        self.assertIn("0x10c\tJ\t0x200\t256", site_lines)
        self.assertIn("'br': 1", result.stdout)
        patched = (output / "main.patched.bin").read_bytes()
        self.assertGreaterEqual(branch_target(patched, 0x10C), 0x10000)

    def test_exclusive_window_inventory(self):
        fixture, original = make_nso(words={
            0x100: 0x885F7E48,  # ldxr w8, [x18]
            0x104: 0x88097E4A,  # stxr w9, w10, [x18]
            0x108: 0x35FFFFC9,  # cbnz w9, #0x100
        })
        result, output = self.run_patch(fixture)

        self.assert_success(result)
        self.assertEqual(
            (output / "main.sites.tsv").read_text(encoding="utf-8"),
            "0x100\tW\t0x10c\n",
        )
        self.assertIn("'window': 1", result.stdout)
        patched = (output / "main.patched.bin").read_bytes()
        self.assertGreaterEqual(branch_target(patched, 0x100), 0x10000)
        self.assertEqual(patched[0x104:0x10C], original[0x104:0x10C])
        self.assertEqual((output / "main.gates.tsv").read_bytes(), b"")

    def test_malformed_inputs_fail_without_artifacts(self):
        valid, _ = make_nso()
        bad_eh, _ = make_nso(eh_encoding=0xFF)
        cases = {
            "magic": (b"NOPE" + valid[4:], "NSO0"),
            "truncated": (valid[:-1], "segment 2"),
            "eh-frame": (bad_eh, "eh_frame_hdr"),
        }
        for name, (fixture, message) in cases.items():
            with self.subTest(name=name):
                result, output = self.run_patch(fixture, "bad-" + name)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_source_build_id_is_preserved_for_each_module(self):
        fixture, _ = make_nso()
        for module in ("main", "sdk"):
            with self.subTest(module=module):
                result, output = self.run_patch(fixture, "identity-" + module, module=module)
                self.assert_success(result)
                fixups = (output / f"{module}.fixups.tsv").read_text()
                self.assertIn("# NSO_BUILD_ID\t" + fixture[0x40:0x60].hex() + "\n", fixups)
                self.assertNotIn("# SAVE_", fixups)

    def test_control_nacp_save_capacities_are_recorded(self):
        fixture, _ = make_nso()
        for journal, maximum, maximum_journal in ((0x1800000, 0x1E000000, 0x2000000),
                                                   (0x1800000, 0, 0), (0, 0, 0)):
            with self.subTest(journal=journal, maximum=maximum, maximum_journal=maximum_journal):
                result, output = self.run_patch(
                    fixture, f"capacities-{journal}-{maximum}",
                    control_nacp=make_control_nacp(
                        journal=journal, maximum=maximum, maximum_journal=maximum_journal),
                )
                self.assert_success(result)
                fixups = (output / "main.fixups.tsv").read_text()
                expected = {
                    "SAVE_DATA_SIZE": 0x2800000,
                    "SAVE_JOURNAL_SIZE": journal,
                    "SAVE_DATA_MAX": maximum or 0x2800000,
                    "SAVE_JOURNAL_MAX": maximum_journal or journal,
                }
                for key, value in expected.items():
                    self.assertIn(f"# {key}\t{value:#x}\n", fixups)

    def test_invalid_control_nacp_fails_without_artifacts(self):
        fixture, _ = make_nso()
        cases = {
            "truncated": (make_control_nacp()[:-1], "at least 0x4000 bytes"),
            "negative-size": (make_control_nacp(size=-1), "SAVE_DATA_SIZE must not be negative"),
            "negative-journal": (make_control_nacp(journal=-1), "SAVE_JOURNAL_SIZE must not be negative"),
            "negative-max": (make_control_nacp(maximum=-1), "SAVE_DATA_MAX must not be negative"),
            "negative-max-journal": (make_control_nacp(maximum_journal=-1), "SAVE_JOURNAL_MAX must not be negative"),
            "zero-size": (make_control_nacp(size=0), "SAVE_DATA_SIZE must be positive"),
            "small-max": (make_control_nacp(maximum=1), "SAVE_DATA_MAX is smaller than SAVE_DATA_SIZE"),
            "small-max-journal": (make_control_nacp(maximum_journal=1), "SAVE_JOURNAL_MAX is smaller than SAVE_JOURNAL_SIZE"),
            "overflow": (make_control_nacp(size=1, journal=1, maximum=(1 << 63) - 1,
                                           maximum_journal=1), "combined save maxima exceed INT64_MAX"),
        }
        for name, (nacp, message) in cases.items():
            with self.subTest(name=name):
                result, output = self.run_patch(fixture, "bad-nacp-" + name, control_nacp=nacp)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_control_nacp_is_rejected_for_sdk(self):
        fixture, _ = make_nso()
        result, output = self.run_patch(fixture, module="sdk", control_nacp=make_control_nacp())
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("--control-nacp is only valid for main", result.stderr)
        self.assertFalse(output.exists())

    def test_unsupported_x18_control_flow_fails_precisely(self):
        fixture, _ = make_nso(words={0x100: 0xD65F0240})  # ret x18
        result, output = self.run_patch(fixture, "unsupported")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("unsupported x18 instruction at 0x100: ret x18", result.stderr)
        self.assertFalse(output.exists())

    def test_unsupported_jump_pattern_fails_precisely(self):
        fixture, _ = make_nso(words={0x100: 0xD61F0240})  # br x18 without table setup
        result, output = self.run_patch(fixture, "unsupported-jump")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("unsupported jump table at 0x100: br x18", result.stderr)
        self.assertFalse(output.exists())

    def test_unsafe_module_name_is_rejected(self):
        fixture, _ = make_nso()
        result, output = self.run_patch(fixture, "unsafe-name", module="../main")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("unsafe module name", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
