#!/usr/bin/env python3
"""Exercise the launcher with fake build/host executables and no game data."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
PATH_VARIABLES = ("SKY_WORK_DIR", "SKY_ROMFS", "SKY_SAVE_DIR", "SKY_REWRITE_DIR",
                  "SKY_SHADER_CACHE_DIR", "SKY_SHADER_CAPTURE_DIR")


class SkyRunTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sky launch '")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "relocated checkout with spaces"
        self.host = self.repo / "skyhost"
        self.host.mkdir(parents=True)
        shutil.copy2(REPO / "skyhost/run.sh", self.host / "run.sh")
        (self.repo / "tools").mkdir()
        for name in ("sky_tools.py", "sky-tools.lock.json", "requirements-skyrim.txt"):
            shutil.copy2(REPO / "tools" / name, self.repo / "tools" / name)
        self.caller = self.root / "foreign working directory"
        self.caller.mkdir()
        self.work = self.root / "private workspace with spaces"
        self.records = self.root / "calls.jsonl"
        self.trace = self.root / "selected-python.txt"
        self.bin = self.root / "fake build tools"
        self.bin.mkdir()
        recorder = self.root / "record.py"
        recorder.write_text(
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "record = {'kind': sys.argv[1], 'arguments': sys.argv[2:], 'cwd': str(Path.cwd()),\n"
            "          'env': {key: value for key, value in os.environ.items() if key.startswith('SKY_')}}\n"
            "with open(os.environ['SKY_RUN_RECORDS'], 'a') as stream:\n"
            "    stream.write(json.dumps(record) + '\\n')\n"
        )
        for path, kind in ((self.bin / "make", "make"), (self.host / "skyhost", "host")):
            path.write_text("#!/bin/sh\nexec " + shlex.join([sys.executable, str(recorder), kind]) + ' "$@"\n')
            path.chmod(0o755)
        for work in (self.work, self.repo / "skyrim-work"):
            python = work / "build/tools/python/bin/python3"
            python.parent.mkdir(parents=True)
            python.write_text("#!/bin/sh\nprintf '%s\\n' \"$0\" > \"$SKY_LAUNCH_PYTHON_TRACE\"\nexec "
                              + shlex.quote(sys.executable) + ' "$@"\n')
            python.chmod(0o755)

    def launch(self, work=None, overrides=None, headless=False, expected_exit=0):
        self.records.unlink(missing_ok=True)
        env = os.environ.copy()
        for name in (*PATH_VARIABLES, "SKY_HEADLESS"):
            env.pop(name, None)
        env.update(PATH=str(self.bin) + ":/usr/bin:/bin", SKY_RUN_RECORDS=str(self.records),
                   SKY_LAUNCH_PYTHON_TRACE=str(self.trace))
        if work is not None:
            env["SKY_WORK_DIR"] = work
        env.update(overrides or {})
        command = ["/bin/sh", str(self.host / "run.sh")]
        if headless:
            command.append("--headless")
        result = subprocess.run(command, cwd=self.caller, env=env, text=True,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        if expected_exit:
            return result
        records = [json.loads(line) for line in self.records.read_text().splitlines()]
        self.assertEqual([record["kind"] for record in records], ["make", "host"])
        self.assertEqual(records[0]["arguments"], ["-C", str(self.host), "-j4"])
        return records[1]

    def test_defaults_are_anchored_to_relocated_checkout_and_workspace(self):
        cases = ((None, self.repo / "skyrim-work"), (str(self.work), self.work),
                 (os.path.relpath(self.work, self.caller), self.work))
        for configured, expected in cases:
            with self.subTest(work=configured):
                result = self.launch(configured, headless=True)
                env = result["env"]
                self.assertTrue(Path(env["SKY_WORK_DIR"]).is_absolute())
                self.assertEqual(Path(env["SKY_WORK_DIR"]).resolve(), expected)
                defaults = {
                    "SKY_ROMFS": expected / "data/switch/skyrim/base/Update/RomFS",
                    "SKY_SAVE_DIR": expected / "run/save",
                    "SKY_SHADER_CACHE_DIR": self.repo / "skyrim-work/build/shaders",
                    "SKY_SHADER_CAPTURE_DIR": expected / "build/shader-captures",
                }
                for name, path in defaults.items():
                    self.assertEqual(Path(env[name]).resolve(), path)
                self.assertEqual(result["arguments"][0], "--run")
                self.assertEqual(Path(result["arguments"][1]).resolve(), expected / "build/rewrite-native")
                self.assertEqual(env["SKY_HEADLESS"], "1")
                self.assertEqual(Path(self.trace.read_text().strip()).resolve(),
                                 expected / "build/tools/python/bin/python3")
                self.assertTrue((expected / "run").is_dir())
                self.assertFalse((self.caller / "skyrim-work").exists())

    def test_explicit_paths_are_preserved(self):
        overrides = {name: str(self.root / "custom paths" / name.lower())
                     for name in PATH_VARIABLES if name != "SKY_WORK_DIR"}
        result = self.launch(str(self.work), overrides)
        for name, value in overrides.items():
            if name != "SKY_REWRITE_DIR":
                self.assertEqual(result["env"][name], value)
        self.assertEqual(result["arguments"], ["--run", overrides["SKY_REWRITE_DIR"]])
        self.assertNotIn("SKY_HEADLESS", result["env"])

    def test_failed_local_python_stops_before_building_or_starting_host(self):
        python = self.work / "build/tools/python/bin/python3"
        python.write_text("#!/bin/sh\nprintf 'export SKY_PARTIAL_ENV=1\\n'\nexit 1\n")
        self.launch(str(self.work), expected_exit=1)
        self.assertFalse(self.records.exists(), "Neither make nor the native host may run after tool setup fails")
        self.assertFalse((self.work / "run").exists())


if __name__ == "__main__":
    unittest.main()
