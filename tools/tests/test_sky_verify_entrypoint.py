"""The optional verifier must discover LLVM without a Homebrew layout."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "sky_verify_rewrite.sh"


class VerifyEntrypointTests(unittest.TestCase):
    def test_llvm_discovery_and_explicit_override(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binaries = root / "tools with spaces"
            binaries.mkdir()
            for name in ("dirname", "date", "mkdir"):
                (binaries / name).symlink_to(shutil.which(name))
            for name, body in (("llvm-objcopy", "exit 0"), ("llvm-objdump", "exit 0"),
                               ("cc", "exit 1"), ("python3", 'printf "LLVM=%s\\n" "$LLVM_BIN"')):
                executable = binaries / name
                executable.write_text("#!/bin/sh\n" + body + "\n")
                executable.chmod(0o755)
            env = os.environ.copy()
            env.pop("LLVM_BIN", None)
            env.pop("SK_PY", None)
            env.pop("SK_VERIFY_RUN", None)
            env["PATH"] = str(binaries)
            env["SKY_PYTHON"] = str(binaries / "python3")
            for explicit in (False, True):
                with self.subTest(explicit=explicit):
                    output = root / ("explicit" if explicit else "discovered")
                    if explicit:
                        env["LLVM_BIN"] = str(binaries)
                    result = subprocess.run([str(SCRIPT), str(output), str(root / "NSO")],
                                            env=env, capture_output=True, text=True)
                    # The compiler deliberately fails; our stub records the selected LLVM path.
                    self.assertEqual(result.returncode, 1)
                    log = next((output / "validation").glob("verify_*.log"))
                    self.assertIn("LLVM=" + str(binaries), log.read_text())
            invalid_output = root / "must remain absent"
            env["LLVM_BIN"] = str(root / "missing llvm")
            result = subprocess.run([str(SCRIPT), str(invalid_output), str(root / "NSO")],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Set LLVM_BIN", result.stderr)
            self.assertFalse(invalid_output.exists())


if __name__ == "__main__":
    unittest.main()
