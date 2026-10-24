import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shader_archive
import shaders


def shader_record(stage="vertex", marker=1):
    """Author a format fixture, not executable or game-derived shader code."""
    start = 0x100 if stage == "compute" else 0x30
    length = 0x80
    constant = (start + length + 255) & ~255
    size = (constant + 16 + 255) & ~255
    code = bytearray(size)
    struct.pack_into("<I", code, 0, 0x12345679 if stage == "compute" else 0x12345678)
    if stage != "compute":
        struct.pack_into("<I", code, start, (1 if stage == "vertex" else 5) << 10)
    code[start + 0x50] = marker
    control = bytearray(0x880)
    struct.pack_into("<I", control, 0, 0x98761234)
    struct.pack_into("<5I", control, 0x6f8, length, 16, constant, size, start)
    struct.pack_into("<3I", control, 0x748, 8, 1, 1)
    return struct.pack("<4I", 16, len(control), 16 + len(control), size) + control + code


def archive_bytes(payload, filename=b"synthetic.fxp", flags=3):
    folder = b"shadersfx\0"
    name = filename + b"\0"
    table_end = 36 + 24 + 1 + len(folder) + 16 + len(name)
    header = struct.pack("<4s8I", b"BSA\0", 105, 36, flags, 1, 1, len(folder), len(name), 32)
    folder_record = struct.pack("<QIIQ", 0, 1, 0, 60 + len(name))
    file_record = struct.pack("<QII", 0, len(payload), table_end)
    return header + folder_record + bytes([len(folder)]) + folder + file_record + name + payload


class ShaderArchiveTests(unittest.TestCase):
    def test_graphics_compute_and_duplicates_match_runtime_capture_identity(self):
        first = shader_record()
        payload = b"authored!" + first + shader_record("fragment", 2) + first + shader_record("compute", 3)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "source.bsa"
            archive.write_bytes(archive_bytes(payload))
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            keys = shader_archive.extract_archive(archive, root / "captures")
            self.assertEqual(len(keys), 3)
            stages = set()
            for key in keys:
                metadata, code = shaders.load_capture(root / "captures" / (key + ".json"))
                self.assertEqual(metadata["shader_key"], key)
                self.assertEqual(shaders.shader_key(metadata, code), key)
                stages.add(metadata["stage"])
            self.assertEqual(stages, {"vertex", "fragment", "compute"})
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), digest)

    def test_rejects_archive_bounds_compression_and_truncation(self):
        valid = archive_bytes(b"authored!" + shader_record())
        invalid_offset = bytearray(valid)
        struct.pack_into("<I", invalid_offset, 60 + 1 + len(b"shadersfx\0") + 12, len(valid))
        for data in (valid[:20], valid[:-1], archive_bytes(shader_record(), flags=7), invalid_offset):
            with self.subTest(length=len(data)):
                with self.assertRaises(ValueError):
                    list(shader_archive.bsa_members(data))

    def test_rejects_invalid_layout_before_creating_any_output(self):
        invalid = bytearray(shader_record())
        struct.pack_into("<I", invalid, 16 + 0x704, 999)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "bad.bsa"
            archive.write_bytes(archive_bytes(b"authored!" + shader_record() + invalid))
            with self.assertRaisesRegex(ValueError, "code size mismatch"):
                shader_archive.extract_archive(archive, root / "captures")
            self.assertFalse((root / "captures").exists())

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "source.bsa"
            archive.write_bytes(archive_bytes(b"authored!" + shader_record()))
            output = root / "captures"
            output.mkdir()
            marker = output / "keep.json"
            marker.write_text("existing")
            with self.assertRaisesRegex(ValueError, "already exists"):
                shader_archive.extract_archive(archive, output)
            self.assertEqual(marker.read_text(), "existing")
            self.assertEqual(len(list(output.iterdir())), 1)

    def test_empty_or_unrecognized_input_cannot_succeed(self):
        for payload, filename in ((b"no shaders", b"synthetic.fxp"), (shader_record(), b"other.dat")):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / "empty.bsa"
                archive.write_bytes(archive_bytes(payload, filename))
                with self.assertRaisesRegex(ValueError, "no .*shader"):
                    shader_archive.extract_archive(archive, root / "captures")
                self.assertFalse((root / "captures").exists())

    def test_magic_inside_code_is_not_an_extra_record(self):
        record = bytearray(shader_record())
        record[shader_archive.CODE_OFFSET + 0x90:shader_archive.CODE_OFFSET + 0x94] = shader_archive.CONTROL_MAGIC
        captures = list(shader_archive.fxp_captures(b"authored!" + record, "synthetic.fxp"))
        self.assertEqual(len(captures), 1)

    def test_compute_workgroup_and_constant_ranges_are_validated(self):
        for offset, value, message in ((0x748, 0, "workgroup"), (0x700, 3, "constant offset"),
                                       (0x6f8, 7, "program range")):
            record = bytearray(shader_record("compute"))
            struct.pack_into("<I", record, 16 + offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, message):
                list(shader_archive.fxp_captures(b"authored!" + record, "synthetic.fxp"))


if __name__ == "__main__":
    unittest.main()
