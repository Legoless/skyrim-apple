"""Exercise the public shell entrypoint from a relocated checkout."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "relocated checkout with spaces"
        tools = self.checkout / "tools"
        tools.mkdir(parents=True)
        source = Path(__file__).resolve().parents[2]
        shutil.copy2(source / "setup-skyrim.sh", self.checkout)
        for name in ("sky_setup.py", "sky_tools.py", "sky_dump.py", "sky_extract_xci.py",
                     "sky_shader_cache.py", "sky-tools.lock.json", "requirements-skyrim.txt"):
            shutil.copy2(source / "tools" / name, tools)
        self.bin = self.root / "custom interpreter directory"
        self.bin.mkdir()
        self.marker = self.root / "selected-interpreter.txt"
        self.python = self.bin / "python3"
        self.python.write_text("#!/bin/sh\n"
                               "printf '%s\\n' \"$0\" >> " + shlex.quote(str(self.marker)) + "\n"
                               "exec " + shlex.quote(sys.executable) + ' "$@"\n')
        self.python.chmod(0o755)
        self.env = os.environ.copy()
        self.env.pop("SKY_PYTHON", None)

    def invoke(self, *arguments):
        return subprocess.run([str(self.checkout / "setup-skyrim.sh"), *arguments],
                              cwd=self.root, env=self.env, capture_output=True, text=True)

    def assert_help(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--keys", result.stdout)
        self.assertIn("dev.skyhost.skyrim", result.stdout)
        self.assertEqual(self.marker.read_text().splitlines(), [str(self.python)] * 2)
        self.assertFalse(list(self.checkout.rglob("__pycache__")))
        self.assertFalse((self.checkout / "skyrim-work").exists())

    def test_finds_python_on_path_from_another_working_directory(self):
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]
        self.assert_help(self.invoke("--help"))

    def test_accepts_explicit_interpreter_path_with_spaces(self):
        self.env["SKY_PYTHON"] = str(self.python)
        self.assert_help(self.invoke("--help"))

    def test_rejects_invalid_explicit_interpreter_without_falling_back(self):
        for interpreter in (self.root / "missing python", self.python):
            with self.subTest(interpreter=interpreter):
                self.python.write_text("#!/bin/sh\nexit 1\n")
                self.env["SKY_PYTHON"] = str(interpreter)
                result = self.invoke("--help")
                self.assertEqual(result.returncode, 1)
                self.assertIn("SKY_PYTHON", result.stderr)
                self.assertIn("3.9", result.stderr)
                self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
