import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "nsoimg", Path(__file__).resolve().parents[1] / "verify/nsoimg.py")
nsoimg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsoimg)


class FixupConsumerTests(unittest.TestCase):
    def test_structural_verifier_accepts_build_identity_and_save_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            build_id = "abcdef01" * 8
            (Path(directory) / "main.fixups.tsv").write_text(
                f"# NSO_BUILD_ID\t{build_id}\n# TEXT_END\t0x4000\n"
                "# KEY_K18_IDX\t7\n# SAVE_DATA_SIZE\t0x2800000\n")
            metadata = nsoimg.header(directory, "main")
            self.assertEqual(metadata["NSO_BUILD_ID"], build_id)
            self.assertEqual(metadata["TEXT_END"], 0x4000)
            self.assertEqual(metadata["KEY_K18_IDX"], 7)
            self.assertEqual(metadata["SAVE_DATA_SIZE"], 0x2800000)
