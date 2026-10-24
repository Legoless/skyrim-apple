#!/usr/bin/env python3
"""Exercise original game code through GPU completion, not an expected trap.

Three completed frames are a host integration check. Visual verification of the
main menu, New Game, and intro is a separate, stronger acceptance requirement.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


HOST = Path(os.environ.get("SKYHOST", Path(__file__).resolve().parents[1] / "skyhost"))
WORK_DIR = Path(os.environ.get("SKY_WORK_DIR", Path(__file__).resolve().parents[2] / "skyrim-work"))
REWRITE_DIR = Path(os.environ.get(
    "SKY_REWRITE_DIR", str(WORK_DIR / "build/rewrite-native")
))
RUN_DIR = Path(os.environ.get("SKY_RUN_DIR", str(WORK_DIR / "run")))
ROMFS_DIR = Path(os.environ.get(
    "SKY_ROMFS", str(WORK_DIR / "data/switch/skyrim/v327680/Update/RomFS")
))


class RuntimeIntegrationTests(unittest.TestCase):
    def test_real_game_submits_and_completes_rendered_frames(self):
        self.assertTrue(HOST.is_file(), HOST)
        for module in ("main", "sdk"):
            for extension in ("patched.bin", "fixups.tsv", "gates.tsv"):
                self.assertTrue((REWRITE_DIR / f"{module}.{extension}").is_file())
        self.assertTrue((ROMFS_DIR / "Skyrim.ini").is_file())
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        capture = RUN_DIR / "skyhost-runtime-test.ppm"
        capture.unlink(missing_ok=True)
        environment = os.environ.copy()
        environment.update(SKY_HEADLESS="1", SKY_RUNTIME_TIMEOUT="90",
                           SKY_PRESENT_LIMIT="3", SKY_SHADER_CAPTURE="0",
                           SKY_ROMFS=str(ROMFS_DIR), SKY_FRAME_CAPTURE=str(capture))
        with tempfile.TemporaryDirectory(prefix="skyhost-save-", dir=RUN_DIR) as save_parent:
            environment["SKY_SAVE_DIR"] = str(Path(save_parent) / "save")
            try:
                result = subprocess.run(
                    [str(HOST), "--run", str(REWRITE_DIR)], text=True,
                    capture_output=True, env=environment, timeout=105, check=False,
                )
                output = result.stdout + result.stderr
                returncode = result.returncode
            except subprocess.TimeoutExpired as error:
                parts = (error.stdout or b"", error.stderr or b"")
                output = "".join(part.decode("utf-8", "replace") if isinstance(part, bytes)
                                 else part for part in parts)
                returncode = None
        log = RUN_DIR / "skyhost-runtime-test.log"
        log.write_text(output, encoding="utf-8")
        diagnostic = f"Full log: {log}\n{output[-24000:]}"
        self.assertEqual(returncode, 0, diagnostic)
        self.assertNotRegex(output, r"RUN_(?:TIMEOUT|CRASH|STOP)|NVN_UNIMPL|NVN_\w*ERROR", diagnostic)
        for marker in ("NNMAIN_ENTERED", "FS_OPEN path=/Skyrim.ini", "ACCOUNT_READY",
                       "OE_STARTUP_LOGO_FINISHED", "NVN_PROGRAM_SHADERS_SET",
                       "NVN_QUEUE_SUBMIT", "NVN_METAL_SHADER_COMPILED"):
            self.assertIn(marker, output, diagnostic)
        completed = re.search(r"PRESENT_LIMIT_REACHED frames=(\d+) draws=(\d+) dispatches=(\d+)", output)
        self.assertIsNotNone(completed, diagnostic)
        self.assertGreaterEqual(int(completed[1]), 3, diagnostic)
        self.assertGreater(int(completed[2]), 0, diagnostic)
        self.assertTrue(capture.is_file(), diagnostic)
        pixels = capture.read_bytes()
        header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", pixels)
        self.assertIsNotNone(header, f"Invalid frame capture: {capture}")
        width, height = int(header[1]), int(header[2])
        self.assertGreater(width, 0)
        self.assertGreater(height, 0)
        self.assertEqual(len(pixels) - header.end(), width * height * 3)


if __name__ == "__main__":
    unittest.main()
