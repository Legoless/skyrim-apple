#!/usr/bin/env python3
"""Exercise prepare-skyrim.sh across interactive PTY, CLI, redeploy, and safety paths."""
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


REPO_SOURCE = Path(__file__).resolve().parents[2]


class PrepareSkyrimEntrypointTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.checkout = self.root / "checkout with spaces"
        (self.checkout / "tools").mkdir(parents=True)
        (self.checkout / "skyhost").mkdir(parents=True)
        (self.checkout / "inputs").mkdir(parents=True)

        shutil.copy2(REPO_SOURCE / "prepare-skyrim.sh", self.checkout / "prepare-skyrim.sh")
        (self.checkout / "prepare-skyrim.sh").chmod(0o755)
        shutil.copy2(REPO_SOURCE / "tools/sky_dump.py", self.checkout / "tools/sky_dump.py")

        self.log_file = self.root / "calls.jsonl"
        self.check_ready = self.root / "check-ready.flag"
        self._install_stubs()

        self.external = self.root / "owned game files"
        self.split_dump = self.external / "The Elder Scrolls V Skyrim.xci"
        self.split_dump.mkdir(parents=True)
        (self.split_dump / "00").write_bytes(bytes(0x100) + b"HEAD" + b"part0")
        (self.split_dump / "01").write_bytes(b"part1")

        self.single_xci = self.external / "Skyrim.xci"
        self.single_xci.write_bytes(bytes(0x100) + b"HEAD" + b"cartridge")

        self.keys_file = self.external / "prod.keys"
        self.keys_file.write_text("header_key = 00\n")

        self.env = os.environ.copy()
        self.env["SKY_PYTHON"] = sys.executable
        self.env["TEST_LOG_FILE"] = str(self.log_file)
        self.env["TEST_CHECK_READY_FLAG"] = str(self.check_ready)

    def _install_stubs(self):
        (self.checkout / "setup-skyrim.sh").write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, pathlib, sys\n"
            "argv = sys.argv[1:]\n"
            "with open(os.environ['TEST_LOG_FILE'], 'a', encoding='utf-8') as f:\n"
            "    f.write(json.dumps({'tool': 'setup-skyrim.sh', 'argv': argv}) + '\\n')\n"
            "if '--check' in argv:\n"
            "    sys.exit(0 if pathlib.Path(os.environ['TEST_CHECK_READY_FLAG']).is_file() else 1)\n"
        )
        (self.checkout / "setup-skyrim.sh").chmod(0o755)

        (self.checkout / "skyhost/run.sh").write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "with open(os.environ['TEST_LOG_FILE'], 'a', encoding='utf-8') as f:\n"
            "    f.write(json.dumps({'tool': 'run.sh', 'argv': sys.argv[1:], "
            "'work_dir': os.environ.get('SKY_WORK_DIR')}) + '\\n')\n"
        )
        (self.checkout / "skyhost/run.sh").chmod(0o755)

    def recorded_calls(self):
        if not self.log_file.is_file():
            return []
        return [json.loads(line) for line in self.log_file.read_text().splitlines() if line.strip()]

    def snapshot(self, directory: Path):
        return {str(p.relative_to(directory)): p.read_bytes()
                for p in directory.rglob("*") if p.is_file()}

    def test_help_and_python_validation_have_no_side_effects(self):
        result = subprocess.run(
            [str(self.checkout / "prepare-skyrim.sh"), "--help"],
            cwd=self.root,
            env=self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--keys PATH", result.stdout)
        self.assertIn("No game update", result.stdout)
        self.assertNotIn("--update", result.stdout)
        self.assertFalse(list(self.checkout.rglob("__pycache__")))
        self.assertFalse((self.checkout / "skyrim-work").exists())

        bad_env = self.env.copy()
        bad_env["SKY_PYTHON"] = str(self.root / "missing-python")
        failed = subprocess.run(
            [str(self.checkout / "prepare-skyrim.sh"), "--help"],
            cwd=self.root,
            env=bad_env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        self.assertEqual(failed.returncode, 1)
        self.assertIn("SKY_PYTHON", failed.stderr)
        self.assertNotIn("Traceback", failed.stderr)

    def test_interactive_pty_prompts_reprompts_and_runs_all_steps(self):
        invalid_dir = self.root / "plain directory"
        invalid_dir.mkdir()
        escaped_split = str(self.split_dump).replace(" ", "\\ ")
        quoted_keys = f'"{self.keys_file}"'
        before_split = self.snapshot(self.split_dump)

        master_fd, slave_fd = pty.openpty()
        proc = subprocess.Popen(
            [str(self.checkout / "prepare-skyrim.sh")],
            cwd=self.root,
            env=self.env,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            close_fds=True,
        )
        os.close(slave_fd)

        transcript = ""
        responses = [
            ("Cartridge dump", str(invalid_dir) + "\n"),
            ("Enter an existing valid .xci file or split-dump directory path.", escaped_split + "\n"),
            ("Console prod.keys file", quoted_keys + "\n"),
            ("Target [1]:", "5\n"),
            ("Apple Development Team ID", "abcde12345\n"),
            ("iOS Bundle ID [dev.skyhost.skyrim.tabcde12345]:", "\n"),
            ("Physical iOS device UDID or name", "My iPad\n"),
        ]
        step = 0
        deadline = time.monotonic() + 15.0
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master_fd], [], [], 0.2)
                if ready:
                    try:
                        chunk = os.read(master_fd, 4096).decode("utf-8", errors="replace")
                    except OSError:
                        break
                    if not chunk:
                        break
                    transcript += chunk
                    while step < len(responses) and responses[step][0] in transcript:
                        _, reply = responses[step]
                        os.write(master_fd, reply.encode("utf-8"))
                        step += 1
                elif proc.poll() is not None:
                    break
            proc.wait(timeout=10)
        finally:
            os.close(master_fd)

        self.assertEqual(step, len(responses), transcript)
        self.assertEqual(proc.returncode, 0, transcript)
        self.assertEqual(self.snapshot(self.split_dump), before_split)

        calls = self.recorded_calls()
        self.assertEqual([c["tool"] for c in calls], ["setup-skyrim.sh", "setup-skyrim.sh", "setup-skyrim.sh"])
        # 1) macOS prepare + build
        self.assertEqual(
            calls[0]["argv"],
            ["--base", str(self.split_dump), "--keys", str(self.keys_file),
             "--work-dir", str(self.checkout / "skyrim-work")],
        )
        # 2) iOS prepare
        self.assertEqual(
            calls[1]["argv"],
            ["--base", str(self.split_dump), "--keys", str(self.keys_file),
             "--work-dir", str(self.checkout / "skyrim-work"),
             "--ios", "--bundle-id", "dev.skyhost.skyrim.tabcde12345"],
        )
        # 3) iOS deploy
        self.assertEqual(
            calls[2]["argv"],
            ["--deploy-ios", "--work-dir", str(self.checkout / "skyrim-work"),
             "--team", "ABCDE12345", "--bundle-id", "dev.skyhost.skyrim.tabcde12345",
             "--device", "My iPad"],
        )

    def test_prepared_workspace_redeploy_skips_dump_and_key_prompts(self):
        self.check_ready.write_text("ready\n")
        result = subprocess.run(
            [str(self.checkout / "prepare-skyrim.sh"), "--target", "ios-deploy", "--team", "ABCDE12345"],
            cwd=self.root,
            env=self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipping dump and key prompts", result.stdout)
        calls = self.recorded_calls()
        self.assertEqual(len(calls), 2)
        self.assertIn("--check", calls[0]["argv"])
        self.assertEqual(
            calls[1]["argv"],
            ["--deploy-ios", "--work-dir", str(self.checkout / "skyrim-work"),
             "--team", "ABCDE12345", "--bundle-id", "dev.skyhost.skyrim.tabcde12345"],
        )

    def test_noninteractive_autodiscovery_launch_and_split_dump_guard(self):
        before = self.snapshot(self.split_dump)
        inside_work = subprocess.run(
            [
                str(self.checkout / "prepare-skyrim.sh"),
                str(self.split_dump),
                "--keys", str(self.keys_file),
                "--work-dir", str(self.split_dump / "inside"),
                "--target", "macos",
            ],
            cwd=self.root,
            env=self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        self.assertEqual(inside_work.returncode, 1)
        self.assertIn("outside the original split dump", inside_work.stderr)
        self.assertEqual(self.snapshot(self.split_dump), before)
        self.assertEqual(self.recorded_calls(), [])

        shutil.copy2(self.single_xci, self.checkout / "inputs/Skyrim.xci")
        shutil.copy2(self.keys_file, self.checkout / "inputs/prod.keys")
        ok = subprocess.run(
            [str(self.checkout / "prepare-skyrim.sh"), "--target", "both", "--run"],
            cwd=self.checkout,
            env=self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        self.assertEqual(ok.returncode, 0, ok.stderr)
        calls = self.recorded_calls()
        self.assertEqual([c["tool"] for c in calls], ["setup-skyrim.sh", "setup-skyrim.sh", "run.sh"])
        self.assertNotIn("--ios", calls[0]["argv"])
        self.assertIn("--ios", calls[1]["argv"])
        self.assertEqual(calls[2]["work_dir"], str(self.checkout / "skyrim-work"))


if __name__ == "__main__":
    unittest.main()
