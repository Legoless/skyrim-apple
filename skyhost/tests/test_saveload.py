"""Regression test: loading an existing save leaves the main menu.

Drives the real game headless through Touch-to-begin -> LOAD -> Show All
Saves -> first save -> Yes (controller pulse) using SKY_AUTO_TAPS, then
requires in-game evidence after the load is confirmed: either the HUD menu
joins the UI stack, or the renderer reaches world draw rates (the HUD probe
runs on the guest main thread's HID poll, which the synchronous ESS load
bypasses, so the stack line can miss the transition). Before the fix, the
game read the whole .sav0 on its save-load worker and then never left the
(faded-out) main menu: a LINEAR_RENDER_TARGET screenshot texture reported a
block-linear storage size (327680 instead of 245760), so every save written
by the host was rejected by the loader's decompression gate.

Fixture saves live outside tracked git files (like the RomFS): set
SKY_SAVE_FIXTURE_DIR to a directory containing Saves/*.sav0 from a real
session (default <repo>/skyrim-work/saves-fixture).
"""
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

HOST = Path(os.environ.get("SKYHOST", Path(__file__).resolve().parents[1] / "skyhost"))
WORK_DIR = Path(os.environ.get("SKY_WORK_DIR", Path(__file__).resolve().parents[2] / "skyrim-work"))
REWRITE_DIR = Path(os.environ.get(
    "SKY_REWRITE_DIR", str(WORK_DIR / "build/rewrite-native")))
RUN_DIR = Path(os.environ.get("SKY_RUN_DIR", str(WORK_DIR / "run")))
ROMFS_DIR = Path(os.environ.get(
    "SKY_ROMFS", str(WORK_DIR / "data/switch/skyrim/v327680/Update/RomFS")))
FIXTURE_DIR = Path(os.environ.get(
    "SKY_SAVE_FIXTURE_DIR", str(WORK_DIR / "saves-fixture")))

OBSERVE_S = 420


class SaveLoadIntegrationTests(unittest.TestCase):
    def test_load_existing_save_leaves_main_menu(self):
        # Data availability is a hard failure, never a skip: same contract as
        # test_runtime.py for the RomFS.
        self.assertTrue((REWRITE_DIR / "main.patched.bin").is_file(),
                        f"rewrite artifacts missing: {REWRITE_DIR}")
        self.assertTrue((ROMFS_DIR / "Data").is_dir(),
                        f"RomFS matching the rewrite is required: {ROMFS_DIR}")
        fixtures = sorted((FIXTURE_DIR / "Saves").glob("*.sav0"))
        self.assertTrue(fixtures,
                        f"save fixtures required: {FIXTURE_DIR}/Saves/*.sav0")

        with tempfile.TemporaryDirectory() as save_parent:
            save_root = Path(save_parent) / "save"
            shutil.copytree(FIXTURE_DIR / "Saves", save_root / "Saves")
            environment = os.environ.copy()
            environment.update(
                SKY_HEADLESS="1",
                SKY_RUNTIME_TIMEOUT=str(OBSERVE_S),
                SKY_SHADER_CAPTURE="0",
                SKY_ROMFS=str(ROMFS_DIR),
                SKY_SAVE_DIR=str(save_root),
                SKY_AUTO_TAPS="20,0.5,0.5;30,0.92,0.80;45,0.71,0.49;60,0.71,0.49;70,-1,1",
            )
            RUN_DIR.mkdir(parents=True, exist_ok=True)
            proc = subprocess.Popen(
                [str(HOST), "--run", str(REWRITE_DIR)], cwd=RUN_DIR,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment)
            confirmed_at = None
            hud_after_confirm = False
            present_re = re.compile(
                r"NVN_PRESENT_COMPLETED frames=(\d+) draws=(\d+)")
            present_baseline = None
            world_rendering = False
            output_tail = []
            try:
                started = time.time()
                while time.time() - started < OBSERVE_S:
                    line = proc.stdout.readline()
                    if not line:
                        break
                    text = line.decode("utf-8", "replace")
                    output_tail = (output_tail + [text])[-50:]
                    if "AUTO_TAP index=4" in text:
                        confirmed_at = time.time() - started
                    # The HUD menu (vt 0x1a8fd90) joins the stack only once
                    # the native load actually starts leaving the main menu.
                    if (confirmed_at is not None and "UI_MENU_STACK" in text
                            and "0x1a8fd90" in text):
                        hud_after_confirm = True
                        break
                    # World rendering evidence: menu rendering averages well
                    # under 100 draws/frame; a loaded world exceeds 1000.
                    if confirmed_at is not None:
                        found = present_re.search(text)
                        if found:
                            frames, draws = (int(found.group(1)),
                                              int(found.group(2)))
                            if present_baseline is None:
                                present_baseline = (frames, draws)
                            elif frames - present_baseline[0] >= 60:
                                rate = ((draws - present_baseline[1])
                                        / (frames - present_baseline[0]))
                                if rate > 200:
                                    world_rendering = True
                                    break
                                present_baseline = (frames, draws)
                    self.assertNotIn("RUN_CRASH", text, "".join(output_tail))
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                if proc.stdout is not None:
                    proc.stdout.close()
        self.assertIsNotNone(confirmed_at, "scripted load confirmation never ran")
        self.assertTrue(
            hud_after_confirm or world_rendering,
            "load confirmed but no HUD stack entry and no world draw rate "
            "(save load stalled after reading the .sav0)")


if __name__ == "__main__":
    unittest.main()
