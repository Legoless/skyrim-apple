#!/usr/bin/env python3
"""Synthetic checks for embedded shader recovery; no game inputs required."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shader_embedded
import shaders


def package(control_size, stage=0):
    """Author a small NVN container; instruction bytes are only test data."""
    control = bytearray(control_size)
    control[:4] = shader_embedded.shader_archive.CONTROL_MAGIC
    code = bytearray(0x100 if stage == 5 else 0x200)
    if stage == 5:
        code[:8] = b"testcode"
        start, length, constant_size, workgroup = 0, 8, 0, [8, 1, 1]
    else:
        struct.pack_into("<I", code, 0, 0x12345678)
        struct.pack_into("<I", code, 0x30, (1 if stage == 0 else 5) << 10)
        # The scanner must skip control-looking bytes inside shader code.
        code[0x100:0x104] = shader_embedded.shader_archive.CONTROL_MAGIC
        start, length, constant_size, workgroup = 0x30, 0x50, 16, [0, 0, 0]
    struct.pack_into("<5I", control, 0x6f8, length, constant_size, 0x100, len(code), start)
    struct.pack_into("<3I", control, 0x748, *workgroup)
    header = struct.pack("<4I", 16, len(control), 16 + len(control), len(code))
    return header + control + code, bytes(code)


def nso_image(payload, compressed):
    """A valid, minimal NSO with a null symbol and embedded authored payload."""
    image = bytearray(0x400) + payload
    struct.pack_into("<I", image, 4, 0x100)
    image[0x100:0x104] = b"MOD0"
    struct.pack_into("<i", image, 0x104, 0x40)
    for index, (tag, value) in enumerate(((5, 0x318), (10, 1), (6, 0x300), (11, 24), (0, 0))):
        struct.pack_into("<qQ", image, 0x140 + 16 * index, tag, value)
    if compressed:
        stored = bytearray([0xF0])
        extra = len(image) - 15
        while extra >= 255:
            stored.append(255)
            extra -= 255
        stored.append(extra)
        stored.extend(image)
    else:
        stored = image
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 0xC, int(compressed))
    struct.pack_into("<3I", header, 0x10, 0x100, 0, len(image))
    for offset in (0x20, 0x30):
        struct.pack_into("<3I", header, offset, 0x100 + len(stored), len(image), 0)
    struct.pack_into("<3I", header, 0x60, len(stored), 0, 0)
    return bytes(header + stored)


class EmbeddedShaderTests(unittest.TestCase):
    def test_recovers_variable_control_layouts_and_deduplicates(self):
        packages = [package(0x7D1), package(0x838, 1), package(0x848, 5)]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, output = directory / "main.patched.bin", directory / "captures"
            original = b"authored module prefix" + b"".join(record for record, _ in packages)
            original += packages[0][0]
            source.write_bytes(original)
            keys = shader_embedded.extract_embedded(source, output)
            self.assertEqual(len(keys), 3)
            self.assertEqual(source.read_bytes(), original)
            recovered = [shaders.load_capture(output / (key + ".json")) for key in keys]
            self.assertEqual({metadata["stage_ordinal"] for metadata, _ in recovered}, {0, 1, 5})
            self.assertEqual({code for _, code in recovered}, {code for _, code in packages})
            self.assertEqual(len(list(output.iterdir())), 6)

    def test_original_nso_and_compressed_nso_recover_identical_inputs(self):
        record, _ = package(0x838)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            keys = []
            codes = []
            for compressed in (False, True):
                with self.subTest(compressed=compressed):
                    source = directory / ("main-compressed" if compressed else "main")
                    output = directory / ("compressed-captures" if compressed else "captures")
                    source.write_bytes(nso_image(record, compressed))
                    result = shader_embedded.extract_embedded(source, output)
                    self.assertEqual(len(result), 1)
                    keys.append(result[0])
                    codes.append(shaders.load_capture(output / (result[0] + ".json"))[1])
            self.assertEqual(keys[0], keys[1])
            self.assertEqual(codes[0], codes[1])

    def test_invalid_later_package_leaves_no_partial_capture_directory(self):
        record, _ = package(0x838)
        invalid = bytearray(record)
        struct.pack_into("<I", invalid, 16 + 0x6F8, 7)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, output = directory / "main", directory / "captures"
            source.write_bytes(record + invalid)
            with self.assertRaisesRegex(ValueError, "program range"):
                shader_embedded.extract_embedded(source, output)
            self.assertFalse(output.exists())

    def test_existing_output_is_never_modified(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, output = directory / "main", directory / "captures"
            source.write_bytes(package(0x838)[0])
            output.mkdir()
            sentinel = output / "keep.bin"
            sentinel.write_bytes(b"keep")
            with self.assertRaisesRegex(ValueError, "already exists"):
                shader_embedded.extract_embedded(source, output)
            self.assertEqual(list(output.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_empty_truncated_and_unsupported_modules_fail(self):
        valid, _ = package(0x838)
        unsupported, _ = package(0x880)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for index, data in enumerate((b"no shader records", valid[:-1], unsupported, b"NSO0")):
                with self.subTest(index=index):
                    source, output = directory / "main", directory / ("captures-" + str(index))
                    source.write_bytes(data)
                    with self.assertRaises(ValueError):
                        shader_embedded.extract_embedded(source, output)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
