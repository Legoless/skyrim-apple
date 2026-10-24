#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("sky_dump", Path(__file__).resolve().parents[1] / "sky_dump.py")
sky_dump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sky_dump)


class SkyDumpTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.work = self.root / "work with spaces"
        self.dump = self.root / "Skyrim split dump.xci"
        self.dump.mkdir()

    def parts(self, directory=None):
        directory = directory or self.dump
        directory.mkdir(parents=True, exist_ok=True)
        first = b"signature".ljust(0x100, b"\0") + b"HEAD" + b"first".ljust(60, b"\0")
        contents = [first, b"middle".ljust(len(first), b"\0"), b"last"]
        for number, content in enumerate(contents):
            (directory / f"{number:02}").write_bytes(content)
        return b"".join(contents)

    def output(self):
        return self.work / "build/cache/skyrim/input.xci"

    def test_regular_xci_returns_original_without_creating_workspace(self):
        source = self.root / "ordinary input.xci"
        source.write_bytes(self.parts())
        self.assertEqual(sky_dump.materialize_xci(source, self.work), source)
        self.assertFalse(self.work.exists())

    def test_direct_parts_stream_and_record_combined_hash(self):
        expected = self.parts()
        original = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in self.dump.iterdir()}
        with patch.object(sky_dump, "BLOCK_SIZE", 37):
            output = sky_dump.materialize_xci(self.dump, self.work)
        self.assertEqual(output, self.output())
        self.assertEqual(output.read_bytes(), expected)
        stamp = json.loads(output.with_suffix(".xci.json").read_text())
        self.assertEqual(stamp["sha256"], hashlib.sha256(expected).hexdigest())
        self.assertEqual([part["name"] for part in stamp["input"]["parts"]], ["00", "01", "02"])
        self.assertEqual(original, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.dump.iterdir()})

    def test_nested_parts_ignore_certificate_and_initial_data(self):
        expected = self.parts(self.dump / "Skyrim split dump.xci")
        (self.dump / "certificate.bin").write_bytes(b"certificate must not be joined")
        (self.dump / "initialData.bin").write_bytes(b"initial data must not be joined")
        self.assertEqual(sky_dump.materialize_xci(self.dump, self.work).read_bytes(), expected)

    def test_reuses_only_verified_cache_and_unchanged_parts_metadata(self):
        expected = self.parts()
        output = sky_dump.materialize_xci(self.dump, self.work)
        before = output.stat().st_mtime_ns
        with patch.object(sky_dump, "_copy_parts", side_effect=AssertionError("cache rebuilt")):
            self.assertEqual(sky_dump.materialize_xci(self.dump, self.work), output)
        self.assertEqual(output.stat().st_mtime_ns, before)
        (self.dump / "02").write_bytes(b"new!")
        with patch.object(sky_dump, "_copy_parts", wraps=sky_dump._copy_parts) as copy:
            self.assertEqual(sky_dump.materialize_xci(self.dump, self.work).read_bytes(), expected[:-4] + b"new!")
            copy.assert_called_once()

    def test_rejects_same_size_cache_tampering(self):
        expected = self.parts()
        output = sky_dump.materialize_xci(self.dump, self.work)
        damaged = bytearray(expected)
        damaged[-1] ^= 1
        output.write_bytes(damaged)
        self.assertEqual(sky_dump.materialize_xci(self.dump, self.work).read_bytes(), expected)

    def test_rejects_missing_duplicate_or_nonzero_initial_part_numbers(self):
        self.parts()
        (self.dump / "01").unlink()
        with self.assertRaisesRegex(ValueError, "continuous"):
            sky_dump.materialize_xci(self.dump, self.work)
        (self.dump / "02").rename(self.dump / "01")
        (self.dump / "0").write_bytes(b"duplicate index")
        with self.assertRaisesRegex(ValueError, "continuous"):
            sky_dump.materialize_xci(self.dump, self.work)
        (self.dump / "0").unlink()
        (self.dump / "00").unlink()
        with self.assertRaisesRegex(ValueError, "start at 00"):
            sky_dump.materialize_xci(self.dump, self.work)
        self.assertFalse(self.work.exists())

    def test_rejects_ambiguous_nested_or_direct_and_nested_dumps(self):
        self.parts(self.dump / "first.xci")
        self.parts(self.dump / "second.xci")
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            sky_dump.materialize_xci(self.dump, self.work)
        for path in (self.dump / "second.xci").iterdir():
            path.unlink()
        (self.dump / "second.xci").rmdir()
        self.parts()
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            sky_dump.materialize_xci(self.dump, self.work)

    def test_rejects_empty_and_inconsistent_parts(self):
        self.parts()
        (self.dump / "02").write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty"):
            sky_dump.materialize_xci(self.dump, self.work)
        (self.dump / "02").write_bytes(b"last")
        (self.dump / "01").write_bytes(b"truncated middle")
        with self.assertRaisesRegex(ValueError, "inconsistent sizes"):
            sky_dump.materialize_xci(self.dump, self.work)
        self.assertFalse(self.work.exists())

    def test_rejects_missing_or_incomplete_xci_header(self):
        self.parts()
        for content in (b"short", b"\0" * 320):
            (self.dump / "00").write_bytes(content)
            (self.dump / "01").write_bytes(content)
            (self.dump / "02").write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "HEAD header"):
                sky_dump.materialize_xci(self.dump, self.work)
        self.assertFalse(self.work.exists())

    def test_interrupted_copy_preserves_old_complete_cache_and_stamp(self):
        expected = self.parts()
        output = sky_dump.materialize_xci(self.dump, self.work)
        stamp = output.with_suffix(".xci.json")
        previous_stamp = stamp.read_bytes()
        (self.dump / "02").write_bytes(b"new!")
        def interrupted(parts, destination, digest):
            destination.write(b"partial write")
            raise KeyboardInterrupt()
        with patch.object(sky_dump, "_copy_parts", side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                sky_dump.materialize_xci(self.dump, self.work)
        self.assertEqual(output.read_bytes(), expected)
        self.assertEqual(stamp.read_bytes(), previous_stamp)
        self.assertEqual(sorted(path.name for path in output.parent.iterdir()), ["input.xci", "input.xci.json"])

    def test_source_change_during_copy_preserves_old_complete_cache(self):
        expected = self.parts()
        output = sky_dump.materialize_xci(self.dump, self.work)
        (self.dump / "02").write_bytes(b"new!")
        copy = sky_dump._copy_parts
        def changed(parts, destination, digest):
            copy(parts, destination, digest)
            (self.dump / "02").write_bytes(b"changed after copy")
        with patch.object(sky_dump, "_copy_parts", side_effect=changed):
            with self.assertRaisesRegex(RuntimeError, "changed while"):
                sky_dump.materialize_xci(self.dump, self.work)
        self.assertEqual(output.read_bytes(), expected)

    def test_rejects_work_directory_inside_input_dump(self):
        self.parts()
        with self.assertRaisesRegex(ValueError, "outside the input dump"):
            sky_dump.materialize_xci(self.dump, self.dump / "generated")
        self.assertFalse((self.dump / "generated").exists())


if __name__ == "__main__":
    unittest.main()
