from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class ReadOnlyCheckTests(unittest.TestCase):
    def test_first_check_does_not_create_bytecode_or_workspace(self):
        source = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory) / "fresh checkout"
            tools = checkout / "tools"
            tools.mkdir(parents=True)
            for name in ("sky_setup.py", "sky_tools.py", "sky_dump.py", "sky_extract_xci.py",
                         "sky_shader_cache.py", "sky-tools.lock.json", "requirements-skyrim.txt"):
                shutil.copy2(source / name, tools / name)
            before = {str(p.relative_to(checkout)): p.read_bytes()
                      for p in checkout.rglob("*") if p.is_file()}
            result = subprocess.run([sys.executable, str(tools / "sky_setup.py"), "--check"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)  # Unprepared, with no installed local tools.
            self.assertIn("Local translated shaders are required", result.stderr)
            self.assertIn("skyhost/README.md", result.stderr)
            after = {str(p.relative_to(checkout)): p.read_bytes()
                     for p in checkout.rglob("*") if p.is_file()}
            self.assertEqual(after, before)
            self.assertFalse((checkout / "skyrim-work").exists())

    def test_check_rejects_incomplete_paired_cache_without_site_packages_or_writes(self):
        source = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory) / "fresh checkout"
            tools = checkout / "tools"
            tools.mkdir(parents=True)
            for name in ("sky_setup.py", "sky_tools.py", "sky_dump.py", "sky_extract_xci.py",
                         "sky_shader_cache.py", "sky-tools.lock.json", "requirements-skyrim.txt"):
                shutil.copy2(source / name, tools / name)
            cache = checkout / "skyrim-work/build/shaders"
            cache.mkdir(parents=True)
            for suffix in ("metal", "json"):
                (cache / ("a" * 64 + "." + suffix)).write_bytes(b"arbitrary nonempty text")
            before = {str(p.relative_to(checkout)): (p.read_bytes(), p.stat().st_mtime_ns)
                      for p in checkout.rglob("*") if p.is_file()}
            result = subprocess.run([sys.executable, "-S", str(tools / "sky_setup.py"), "--check"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Local shader cache differs from the recorded", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            after = {str(p.relative_to(checkout)): (p.read_bytes(), p.stat().st_mtime_ns)
                     for p in checkout.rglob("*") if p.is_file()}
            self.assertEqual(after, before)
