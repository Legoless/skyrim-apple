#!/usr/bin/env python3
"""Deployment orchestration with synthetic builds and no device commands."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sky_deploy_ios as sky

RUN_LOGGED = sky.run_logged


class IOSDeploymentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sky deploy ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "checkout with spaces"
        self.work = self.root / "private workspace"
        self.romfs = self.root / "owned RomFS"
        self.derived = self.root / "derived data"
        (self.romfs / "Data").mkdir(parents=True)
        (self.romfs / "Skyrim.ini").write_text("[General]\n")
        (self.romfs / "Data/game.bsa").write_bytes(b"synthetic archive")
        self.bundle = "dev.example.skyrim"
        self.actual_bundle = self.bundle
        self.team = "ABC123DEF4"
        self.selected = {"identifier": "device fixture", "udid": "fixture-udid", "name": "Owned iPad"}
        self.calls = []
        self.events = []
        self.guest_launches = []
        self.failure = None
        self.cleanup_failure = False
        self.holding_pid = 4242
        self.wrong_bundle = False
        self.verified = True
        self.expected_romfs = self.romfs
        self.save = b"preserve this existing save"
        self.remote = {"Documents/save/slot.sav0": self.save}
        self.present_saves = True
        self.save_backups = []
        self.device = Mock()
        self.device.require_unlocked.side_effect = lambda: self.events.append("unlocked")
        self.device.sync_romfs.side_effect = self.sync
        self.device.backup_saves.side_effect = self.backup_saves
        self.device.verify_saves.side_effect = self.verify_saves
        for name, value in (("REPO", self.repo), ("BUILD", self.repo / "skyhost/build/ios")):
            replacement = patch.object(sky, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)
        for name, options in (
            ("check_tools", {"return_value": []}),
            ("workspace_problems", {"return_value": []}),
            ("tool_environment", {"return_value": {"SKY_PYTHON": "/pinned/python3", "PATH": "/pinned/bin"}}),
            ("select_device", {"return_value": self.selected}),
            ("Device", {"return_value": self.device}),
            ("run_logged", {"side_effect": self.build}),
            ("run_devicectl", {"side_effect": self.device_command}),
        ):
            replacement = patch.object(sky, name, **options)
            setattr(self, name, replacement.start())
            self.addCleanup(replacement.stop)

    def stage(self, name):
        self.events.append(name)
        if self.failure == name:
            raise RuntimeError("synthetic " + name + " failure")

    def backup_saves(self, destination):
        self.stage("backup-saves")
        if not self.present_saves:
            backup = {"present": False}
        else:
            destination = Path(destination)
            self.assertTrue(destination.is_absolute())
            self.assertFalse(destination.exists(), "Each deployment must preserve a new independent backup")
            path = destination / "Saves/slot.sav0"
            path.parent.mkdir(parents=True)
            path.write_bytes(self.save)
            backup = {"present": True, "path": str(destination),
                      "files": {"Saves/slot.sav0": {"bytes": len(self.save),
                                "sha256": hashlib.sha256(self.save).hexdigest()}}}
        self.save_backups.append(backup)
        return backup

    def verify_saves(self, backup):
        self.stage("verify-saves")
        self.assertIs(backup, self.save_backups[-1])
        if backup["present"]:
            self.assertEqual((Path(backup["path"]) / "Saves/slot.sav0").read_bytes(), self.save)
            self.assertEqual(self.remote["Documents/save/slot.sav0"], self.save)

    def build(self, command, environment, log):
        command = [str(part) for part in command]
        self.calls.append((command, environment.copy(), log))
        if "--prepare-only" in command:
            self.stage("prepare")
            self.actual_bundle = command[command.index("--bundle-id") + 1]
        elif "xcodebuild" in command:
            self.stage("signed-build")
            app = self.derived / "Build/Products/Release-iphoneos/SkyrimIOS.app"
            app.mkdir(parents=True, exist_ok=True)
            with (app / "Info.plist").open("wb") as stream:
                plistlib.dump({"CFBundleIdentifier": "dev.wrong.app" if self.wrong_bundle else self.actual_bundle}, stream)
        elif command[0] == "/usr/bin/codesign":
            self.stage("signature")
        else:
            raise AssertionError("Unexpected build command: " + repr(command))

    def device_command(self, command, **kwargs):
        command = [str(part) for part in command]
        self.calls.append((command, kwargs, None))
        self.assertNotIn("uninstall", command)
        self.assertNotIn("--remove-existing-content", command)
        if command[:3] == ["device", "install", "app"]:
            self.stage("install")
        elif command[:3] == ["device", "process", "launch"]:
            environment = json.loads(command[command.index("--environment-variables") + 1]) \
                if "--environment-variables" in command else {}
            transfer = "--controls-test-mode" in command or environment.get("SKY_CONTROLS_TEST_MODE") == "1"
            if not transfer:
                self.guest_launches.append(command)
            self.stage("transfer-ui" if transfer else "launch")
            return {"process": {"processIdentifier": self.holding_pid if transfer else 5252}}
        elif command[:3] == ["device", "process", "terminate"]:
            self.stage("terminate")
            if self.cleanup_failure:
                raise RuntimeError("synthetic cleanup failure")
        else:
            raise AssertionError("Unexpected device command: " + repr(command))
        return {}

    def assert_transfer_cleanup(self):
        calls = [(command, options) for command, options, _ in self.calls
                 if command[:3] == ["device", "process", "terminate"]]
        self.assertEqual(len(calls), 1)
        command, options = calls[0]
        self.assertEqual(command[command.index("--pid") + 1], str(self.holding_pid))
        self.assertEqual(command[command.index("--device") + 1], self.selected["identifier"])
        self.assertEqual(options["timeout"], 30)

    def sync(self, source):
        self.stage("sync")
        self.assertEqual(source, self.expected_romfs)
        for path in source.rglob("*"):
            if path.is_file():
                self.remote["Documents/Skyrim/RomFS/" + str(path.relative_to(source))] = path.read_bytes()
        return {"files": 2, "bytes": 27, "copied": 2, "verified": self.verified}

    def invoke(self, extra=(), *, team=True, bundle=True, device=True, romfs=True):
        args = ["--work-dir", str(self.work), "--derived-data", str(self.derived)]
        for enabled, flag, value in ((team, "--team", self.team), (bundle, "--bundle-id", self.bundle),
                                     (device, "--device", "Owned iPad"), (romfs, "--romfs", str(self.romfs))):
            if enabled:
                args.extend((flag, value))
        errors = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
            result = sky.main([*args, *extra])
        return result, errors.getvalue()

    def test_signed_release_install_verify_launch_in_order_and_preserve_saves(self):
        result, errors = self.invoke()
        self.assertEqual(result, 0, errors)
        ordered = ["backup-saves", "prepare", "signed-build", "signature", "install", "transfer-ui",
                   "sync", "verify-saves", "launch"]
        self.assertEqual([event for event in self.events if event in ordered], ordered)
        self.Device.assert_called_once_with(self.selected["identifier"], self.bundle)
        self.check_tools.assert_called_once_with(self.work, ios=True)
        self.workspace_problems.assert_not_called()
        self.assertEqual(self.remote["Documents/save/slot.sav0"], self.save)
        prepare, signed, signature = [call for call in self.calls if call[2] is not None]
        self.assertEqual(prepare[0][0], "/pinned/python3")
        self.assertIn(str(self.work), prepare[0])
        self.assertIn("DEVELOPMENT_TEAM=" + self.team, signed[0])
        self.assertIn("CODE_SIGNING_ALLOWED=YES", signed[0])
        self.assertEqual(signed[0][signed[0].index("-destination") + 1], "id=" + self.selected["udid"])
        self.assertIn("Release", signed[0])
        self.assertEqual(signature[0][:3], ["/usr/bin/codesign", "--verify", "--strict"])
        standby = next(command for command, _, _ in self.calls if "--controls-test-mode" in command)
        self.assertEqual(standby[-3:], [self.bundle, "--", "--controls-test-mode"])
        self.assertIn("--terminate-existing", standby)
        self.assertEqual(json.loads(standby[standby.index("--environment-variables") + 1]),
                         {"SKY_CONTROLS_TEST_MODE": "1"})
        self.assertNotIn("terminate", self.events)
        self.assertEqual(len(self.guest_launches), 1)
        launch = self.guest_launches[0]
        self.assertEqual(launch[-1], self.bundle)
        self.assertIn("--terminate-existing", launch)
        self.assertEqual(json.loads(launch[launch.index("--environment-variables") + 1]),
                         {"SKY_CONTROLS_TEST_MODE": "0"})
        report = json.loads((self.work / "build/deploy-ios/deployment.json").read_text())
        self.assertTrue(report["verified"])
        self.assertEqual(report["bundle_id"], self.bundle)
        self.assertEqual(report["save_backup"], self.save_backups[-1]["path"])
        self.assertTrue(report["saves_verified"])

    def test_each_failed_stage_stops_before_game_launch(self):
        for stage in ("backup-saves", "prepare", "signed-build", "signature", "install", "transfer-ui",
                      "sync", "verify-saves"):
            with self.subTest(stage=stage):
                self.failure = stage
                self.events.clear()
                self.calls.clear()
                result, errors = self.invoke()
                self.assertEqual(result, 1)
                self.assertIn("synthetic " + stage + " failure", errors)
                self.assertNotIn("launch", self.events)
                self.assertEqual(self.remote["Documents/save/slot.sav0"], self.save)
                if stage in ("sync", "verify-saves"):
                    self.assert_transfer_cleanup()
                else:
                    self.assertNotIn("terminate", self.events)

    def test_incomplete_verification_stops_before_launch_and_success_report(self):
        self.verified = False
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("verification", errors.lower())
        self.assertNotIn("launch", self.events)
        self.assert_transfer_cleanup()
        self.assertFalse((self.work / "build/deploy-ios/deployment.json").exists())

    def test_save_backup_failure_prevents_build_install_and_transfer(self):
        self.failure = "backup-saves"
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("synthetic backup-saves failure", errors)
        self.run_logged.assert_not_called()
        self.run_devicectl.assert_not_called()
        self.device.sync_romfs.assert_not_called()
        self.device.verify_saves.assert_not_called()

    def test_save_verification_failure_preserves_old_backups_and_prevents_guest(self):
        result, errors = self.invoke()
        self.assertEqual(result, 0, errors)
        first_backup = Path(self.save_backups[-1]["path"])
        self.failure = "verify-saves"
        self.events.clear()
        self.calls.clear()
        self.guest_launches.clear()
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("synthetic verify-saves failure", errors)
        self.assertEqual(self.guest_launches, [])
        self.assert_transfer_cleanup()
        self.assertNotEqual(first_backup, Path(self.save_backups[-1]["path"]))
        for backup in self.save_backups:
            self.assertEqual((Path(backup["path"]) / "Saves/slot.sav0").read_bytes(), self.save)
        self.assertFalse((self.work / "build/deploy-ios/deployment.json").exists())

    def test_first_install_with_no_existing_save_continues_after_absence_verification(self):
        self.present_saves = False
        self.remote.clear()
        result, errors = self.invoke()
        self.assertEqual(result, 0, errors)
        self.device.verify_saves.assert_called_once_with({"present": False})
        self.assertLess(self.events.index("verify-saves"), self.events.index("launch"))
        report = json.loads((self.work / "build/deploy-ios/deployment.json").read_text())
        self.assertIsNone(report["save_backup"])
        self.assertTrue(report["saves_verified"])

    def test_later_failed_transfer_removes_prior_verified_success_report(self):
        result, errors = self.invoke()
        self.assertEqual(result, 0, errors)
        report = self.work / "build/deploy-ios/deployment.json"
        self.assertTrue(json.loads(report.read_text())["verified"])
        self.failure = "sync"
        self.events.clear()
        self.calls.clear()
        self.guest_launches.clear()
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("synthetic sync failure", errors)
        self.assertFalse(report.exists())
        self.assertEqual(self.guest_launches, [])
        self.assert_transfer_cleanup()

    def test_wrong_signed_bundle_is_rejected_before_install(self):
        self.wrong_bundle = True
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("bundle ID", errors)
        self.assertNotIn("install", self.events)
        self.assertNotIn("sync", self.events)

    def test_no_terminal_requires_explicit_team_without_building(self):
        with patch.object(sys.stdin, "isatty", return_value=False), patch("builtins.input") as prompt:
            result, errors = self.invoke(team=False)
        self.assertEqual(result, 1)
        self.assertIn("--team", errors)
        prompt.assert_not_called()
        self.select_device.assert_not_called()
        self.assertEqual(self.events, [])

    def test_interactive_team_and_default_bundle_forward_device_selection(self):
        with patch.object(sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", side_effect=[self.team.lower(), ""]) as prompt:
            result, errors = self.invoke(team=False, bundle=False, device=False)
        self.assertEqual(result, 0, errors)
        self.assertEqual(prompt.call_count, 2)
        self.select_device.assert_called_once_with(None)
        self.Device.assert_called_once_with(self.selected["identifier"], "dev.skyhost.skyrim.t" + self.team.lower())

    def test_no_terminal_default_bundle_is_scoped_to_supplied_team(self):
        with patch.object(sys.stdin, "isatty", return_value=False), patch("builtins.input") as prompt:
            result, errors = self.invoke(bundle=False)
        self.assertEqual(result, 0, errors)
        prompt.assert_not_called()
        self.assertEqual(self.actual_bundle, "dev.skyhost.skyrim.t" + self.team.lower())

    def test_device_selection_or_locked_device_failure_prevents_build(self):
        self.select_device.side_effect = ValueError("Supply --device without a terminal")
        result, errors = self.invoke(device=False)
        self.assertEqual(result, 1)
        self.assertIn("--device", errors)
        self.assertEqual(self.events, [])
        self.select_device.side_effect = None
        self.device.require_unlocked.side_effect = ValueError("Device must be unlocked")
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("unlocked", errors)
        self.run_logged.assert_not_called()

    def test_missing_selected_udid_fails_before_build_or_install(self):
        for value in (None, ""):
            with self.subTest(udid=value):
                self.selected["udid"] = value
                result, errors = self.invoke()
                self.assertEqual(result, 1)
                self.assertIn("UDID", errors)
        self.Device.assert_not_called()
        self.run_logged.assert_not_called()
        self.run_devicectl.assert_not_called()

    def test_prepared_workspace_without_romfs_override_uses_workspace_checks(self):
        self.expected_romfs = self.work / "data/switch/skyrim/base/Update/RomFS"
        (self.expected_romfs / "Data").mkdir(parents=True)
        (self.expected_romfs / "Skyrim.ini").write_text("fixture")
        result, errors = self.invoke(romfs=False)
        self.assertEqual(result, 0, errors)
        self.workspace_problems.assert_called_once_with(self.work, ios=True)
        self.check_tools.assert_not_called()

    def test_preflight_and_invalid_signing_choices_fail_before_device_actions(self):
        self.check_tools.return_value = ["Missing pinned tool"]
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("Missing pinned tool", errors)
        self.select_device.assert_not_called()
        self.check_tools.return_value = []
        for extra in (["--team", "short"], ["--bundle-id", "not a bundle id"]):
            with self.subTest(extra=extra):
                result, _ = self.invoke(extra)
                self.assertEqual(result, 1)
        self.select_device.assert_not_called()

    def test_interrupt_returns_cancel_status_without_launch(self):
        self.device.sync_romfs.side_effect = KeyboardInterrupt()
        result, errors = self.invoke()
        self.assertEqual(result, 130)
        self.assertIn("cancelled", errors)
        self.assertNotIn("launch", self.events)
        self.assert_transfer_cleanup()

    def test_failed_game_launch_cleans_transfer_process_without_success_report(self):
        self.failure = "launch"
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("synthetic launch failure", errors)
        self.assert_transfer_cleanup()
        self.assertFalse((self.work / "build/deploy-ios/deployment.json").exists())

    def test_cleanup_failure_preserves_original_transfer_error(self):
        self.failure = "sync"
        self.cleanup_failure = True
        result, errors = self.invoke()
        self.assertEqual(result, 1)
        self.assertIn("synthetic sync failure", errors)
        self.assertIn("synthetic cleanup failure", errors)
        self.assertNotIn("launch", self.events)
        self.assert_transfer_cleanup()

    def test_build_runner_uses_repo_cwd_and_preserves_failure_log(self):
        log = self.root / "signing output.log"
        environment = {"PATH": "/pinned/bin"}

        def fail(command, **kwargs):
            self.assertEqual(command, ["xcodebuild", "argument with spaces"])
            self.assertEqual(kwargs["cwd"], self.repo)
            self.assertEqual(kwargs["env"], environment)
            kwargs["stdout"].write("synthetic signing failure details\n")
            return subprocess.CompletedProcess(command, 1)

        with patch.object(sky.subprocess, "run", side_effect=fail), \
             contextlib.redirect_stdout(io.StringIO()), \
             self.assertRaisesRegex(RuntimeError, "synthetic signing failure details"):
            RUN_LOGGED(["xcodebuild", "argument with spaces"], environment, log)
        self.assertIn("synthetic signing failure details", log.read_text())


if __name__ == "__main__":
    unittest.main()
