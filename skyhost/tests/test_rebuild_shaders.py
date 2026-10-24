import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rebuild_shaders as rebuild
import shaders


class ExactShaderRebuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "game.bsa"
        self.module = self.root / "main"
        self.archive.write_bytes(b"original archive")
        self.module.write_bytes(b"original module")
        self.output = self.root / "new-cache"
        self.reference = self.root / "reference"
        self.reference.mkdir()
        self.key = "a" * 64
        self.write_pair(self.reference)
        self.expected = rebuild.fingerprint(self.reference)
        key = self.capture(self.root / "seed")
        self.expected_keys_sha256 = hashlib.sha256((key + "\n").encode("ascii")).hexdigest()

    def write_pair(self, directory, source=b"authored Metal", metadata=b'{"route":"B"}'):
        directory.mkdir(exist_ok=True)
        (directory / (self.key + ".metal")).write_bytes(source)
        (directory / (self.key + ".json")).write_bytes(metadata)

    def capture(self, directory, data=b"synthetic input"):
        directory.mkdir()
        metadata = {"accepted": True, "stage": "vertex", "stage_ordinal": 0,
                    "program_start": 0, "program_length": len(data),
                    "constant_offset": len(data), "constant_size": 0,
                    "workgroup": [0, 0, 0], "code_size": len(data),
                    "code_sha256": hashlib.sha256(data).hexdigest(), "code_file": "input.bin"}
        metadata["shader_key"] = shaders.shader_key(metadata, data)
        (directory / "input.bin").write_bytes(data)
        (directory / "capture.json").write_text(json.dumps(metadata))
        return metadata["shader_key"]

    def run_rebuild(self, translator, ensure=False):
        def extract(_source, output):
            self.capture(output)

        with patch.object(rebuild, "extract_archive", side_effect=extract), \
             patch.object(rebuild, "extract_embedded", side_effect=extract), \
             patch.object(rebuild, "tool_environment", return_value={"SKY_PYTHON": sys.executable}), \
             patch.object(rebuild.subprocess, "run", side_effect=translator):
            return rebuild.rebuild(self.archive, self.module, self.output, self.root,
                                   expected=self.expected, ensure=ensure,
                                   expected_keys_sha256=self.expected_keys_sha256)

    def test_fingerprint_covers_names_contents_and_inventory(self):
        digest = hashlib.sha256()
        for extension, data in [("json", b'{"route":"B"}'), ("metal", b"authored Metal")]:
            digest.update((self.key + "." + extension + "\0" + hashlib.sha256(data).hexdigest()
                           + "\n").encode("ascii"))
        self.assertEqual(self.expected["sha256"], digest.hexdigest())
        self.write_pair(self.reference, source=b"different Metal")
        self.assertNotEqual(rebuild.fingerprint(self.reference), self.expected)
        (self.reference / ("b" * 64 + ".metal")).write_bytes(b"extra shader")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            rebuild.fingerprint(self.reference)

    def test_rejects_empty_files_and_symbolic_links(self):
        source = self.reference / (self.key + ".metal")
        source.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty file"):
            rebuild.fingerprint(self.reference)
        source.unlink()
        source.symlink_to(self.archive)
        with self.assertRaisesRegex(ValueError, "symbolic links"):
            rebuild.fingerprint(self.reference)

    def test_capture_merge_deduplicates_and_checks_original_bytes(self):
        first, second = self.root / "first", self.root / "second"
        self.capture(first)
        self.capture(second)
        output = self.root / "merged"
        keys = rebuild.merge_captures([first, second], output)
        self.assertEqual(len(keys), 1)
        self.assertEqual(len(list(output.glob("*.json"))), 1)
        (second / "input.bin").write_bytes(b"corrupted input")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            rebuild.merge_captures([first, second], self.root / "corrupt-merge")

    def test_existing_output_is_never_modified(self):
        self.output.mkdir()
        sentinel = self.output / "existing"
        sentinel.write_bytes(b"keep")
        with patch.object(rebuild, "extract_archive") as extract:
            with self.assertRaisesRegex(ValueError, "Output already exists"):
                rebuild.rebuild(self.archive, self.module, self.output, self.root)
        extract.assert_not_called()
        self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_only_exact_outputs_are_published(self):
        def translate(command, **kwargs):
            self.assertEqual(kwargs["cwd"], rebuild.ROOT)
            self.write_pair(Path(command[3]))

        self.assertEqual(self.run_rebuild(translate), self.expected)
        self.assertEqual(rebuild.fingerprint(self.output), self.expected)
        report = json.loads((self.output / "rebuild-verification.json").read_text())
        self.assertTrue(report["verified"])
        self.assertEqual(self.archive.read_bytes(), b"original archive")
        self.assertEqual(self.module.read_bytes(), b"original module")
        self.assertFalse(list(self.root.glob(".shader-rebuild-*")))

    def test_mismatch_retains_diagnostics_without_publishing(self):
        def translate(command, **_kwargs):
            self.write_pair(Path(command[3]), metadata=b'{"route":"changed"}')

        with self.assertRaisesRegex(ValueError, "differs from the reference"):
            self.run_rebuild(translate)
        self.assertFalse(self.output.exists())
        attempts = list(self.root.glob(".shader-rebuild-*"))
        self.assertEqual(len(attempts), 1)
        self.assertFalse((attempts[0] / "cache/rebuild-verification.json").exists())

    def test_failed_translation_never_publishes_partial_cache(self):
        def translate(command, **_kwargs):
            self.write_pair(Path(command[3]))
            raise subprocess.CalledProcessError(1, command)

        with self.assertRaisesRegex(ValueError, "Rebuild artifacts retained"):
            self.run_rebuild(translate)
        self.assertFalse(self.output.exists())

    def test_wrong_input_count_fails_before_translation(self):
        self.expected = dict(self.expected, pairs=2)
        with self.assertRaisesRegex(ValueError, "requires 2"):
            self.run_rebuild(lambda *_args, **_kwargs: self.fail("translation must not run"))
        self.assertFalse(self.output.exists())

    def test_ensure_reuses_matching_cache_without_writing_or_compiling(self):
        self.write_pair(self.output)
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.output.iterdir()}
        result = self.run_rebuild(lambda *_args, **_kwargs: self.fail("cache must be reused"), ensure=True)
        self.assertEqual(result, self.expected)
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns)
                                  for p in self.output.iterdir()})
        self.assertFalse(list(self.root.glob(".shader-rebuild-*")))

    def test_ensure_refuses_same_count_but_different_game_inputs(self):
        self.write_pair(self.output)
        self.expected_keys_sha256 = "0" * 64
        with self.assertRaisesRegex(ValueError, "inputs do not match"):
            self.run_rebuild(lambda *_args, **_kwargs: self.fail("must not translate"), ensure=True)
        self.assertEqual(rebuild.fingerprint(self.output), self.expected)

    def test_ensure_preserves_changed_cache(self):
        self.write_pair(self.output, source=b"preserve changed source")
        before = rebuild.fingerprint(self.output)
        with self.assertRaisesRegex(ValueError, "Move it to a backup"):
            self.run_rebuild(lambda *_args, **_kwargs: self.fail("must not replace"), ensure=True)
        self.assertEqual(rebuild.fingerprint(self.output), before)

    def test_ensure_can_fill_empty_directory(self):
        self.output.mkdir()
        self.run_rebuild(lambda command, **_kwargs: self.write_pair(Path(command[3])), ensure=True)
        self.assertEqual(rebuild.fingerprint(self.output), self.expected)

    def test_ensure_preserves_files_added_to_empty_output_during_translation(self):
        self.output.mkdir()

        def translate(command, **_kwargs):
            self.write_pair(Path(command[3]))
            (self.output / "concurrent-file").write_bytes(b"preserve")

        with self.assertRaisesRegex(ValueError, "Rebuild artifacts retained"):
            self.run_rebuild(translate, ensure=True)
        self.assertEqual((self.output / "concurrent-file").read_bytes(), b"preserve")

    def test_ensure_rejects_symlink_output(self):
        self.output.symlink_to(self.reference, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "regular directory"):
            self.run_rebuild(lambda *_args, **_kwargs: self.fail("must not run"), ensure=True)
        self.assertEqual(rebuild.fingerprint(self.reference), self.expected)


if __name__ == "__main__":
    unittest.main()
