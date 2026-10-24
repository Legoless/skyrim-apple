#!/usr/bin/env python3
"""Device transfers are modeled with temporary files; no device is changed."""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sky_ios_device as ios


class DevicectlTests(unittest.TestCase):
    def test_json_output_is_parsed_and_removed_without_shell_interpolation(self):
        outputs = []
        def run(command, **options):
            self.assertEqual(command[:2], ["/usr/bin/xcrun", "devicectl"])
            self.assertIn("a name with ' quotes", command)
            self.assertNotIn("shell", options)
            output = Path(command[command.index("--json-output") + 1])
            outputs.append(output)
            output.write_text(json.dumps({"result": {"value": "ok"}}))
            return subprocess.CompletedProcess(command, 0, "", "")
        with patch.object(ios.subprocess, "run", side_effect=run):
            self.assertEqual(ios.run_devicectl(["list", "devices", "--device", "a name with ' quotes"]),
                             {"value": "ok"})
        self.assertFalse(outputs[0].exists())

    def test_failure_invalid_json_and_timeout_do_not_return_success(self):
        for failure in ("command", "json", "timeout"):
            with self.subTest(failure=failure):
                def run(command, **options):
                    if failure == "timeout":
                        raise subprocess.TimeoutExpired(command, 1)
                    output = Path(command[command.index("--json-output") + 1])
                    output.write_text("not JSON" if failure == "json" else '{"error":{"code":3}}')
                    return subprocess.CompletedProcess(command, 1 if failure == "command" else 0,
                                                       "", "device unavailable")
                with patch.object(ios.subprocess, "run", side_effect=run), self.assertRaises(ios.DeviceError):
                    ios.run_devicectl(["list", "devices"])

    def test_cli_options_precede_launch_argument_delimiter(self):
        def run(command, **options):
            delimiter = command.index("--")
            for flag in ("--quiet", "--timeout", "--json-output"):
                self.assertLess(command.index(flag), delimiter)
            self.assertEqual(command[delimiter + 1:], ["--controls-test-mode"])
            output = Path(command[command.index("--json-output") + 1])
            output.write_text('{"result":{"processIdentifier":123}}')
            return subprocess.CompletedProcess(command, 0, "", "")
        with patch.object(ios.subprocess, "run", side_effect=run):
            result = ios.run_devicectl(["device", "process", "launch", "--device", "test-device",
                                       "example.owned.skyrim", "--", "--controls-test-mode"])
            self.assertEqual(result["processIdentifier"], 123)

    def test_selection_uses_physical_udid_and_rejects_ambiguity(self):
        records = [
            {"identifier": "core-pad", "hardwareProperties": {"udid": "pad-udid", "platform": "iOS", "reality": "physical"},
             "connectionProperties": {"pairingState": "paired"}, "deviceProperties": {"name": "Owned Pad"}},
            {"identifier": "simulator", "hardwareProperties": {"platform": "iOS", "reality": "simulated"}},
        ]
        with patch.object(ios, "run_devicectl", return_value={"devices": records}):
            self.assertEqual(ios.select_device()["identifier"], "pad-udid")
            self.assertEqual(ios.select_device("core-pad")["udid"], "pad-udid")
            self.assertEqual(ios.select_device("Owned Pad")["name"], "Owned Pad")
        devices = [{"identifier": "one", "udid": "one", "name": "First", "state": "connected"},
                   {"identifier": "two", "udid": "two", "name": "Second", "state": "connected"}]
        with patch.object(ios, "list_devices", return_value=devices), \
             patch.object(ios.sys.stdin, "isatty", return_value=False), self.assertRaisesRegex(ios.DeviceError, "--device"):
            ios.select_device()
        with patch.object(ios, "list_devices", return_value=devices), \
             patch.object(ios.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value="2"), patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(ios.select_device()["identifier"], "two")

    def test_default_selection_prefers_connected_device_but_explicit_selector_can_choose_old_pair(self):
        devices = [{"identifier": "pad", "udid": "pad", "name": "Connected Pad", "state": "connected"},
                   {"identifier": "phone", "udid": "phone", "name": "Old Phone", "state": "disconnected"},
                   {"identifier": "other", "udid": "other", "name": "Old Pair", "state": "unavailable"}]
        with patch.object(ios, "list_devices", return_value=devices), \
             patch.object(ios.sys.stdin, "isatty", return_value=False):
            self.assertEqual(ios.select_device()["identifier"], "pad")
            self.assertEqual(ios.select_device("Old Phone")["identifier"], "phone")


class RomFSSyncTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sky sync ' ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "owned RomFS"
        (self.source / "Data/nested").mkdir(parents=True)
        for name, contents in {
            "Skyrim.ini": b"[General]\n",
            "Data/missing.bsa": b"missing file contents",
            "Data/zero.bsa": b"nonzero archive contents",
            "Data/texture 'quoted'.bsa": b"complete quoted texture archive",
            "Data/nested/keep.esm": b"unchanged game master",
        }.items():
            (self.source / name).write_bytes(contents)
        self.container = self.root / "device container"
        self.save = self.container / "Documents/save"
        self.save.mkdir(parents=True)
        (self.save / "important.sav").write_bytes(b"preserve player progress")
        self.remote = self.container / ios.ROMFS
        self.calls = []
        self.locked = False
        self.fail_copy = False
        self.fail_list = False
        self.corrupt_upload = False
        self.nest_directory = False
        self.installed = True
        self.fail_apps = False
        self.fail_readback = False
        self.device = ios.Device("test-device-udid", "example.owned.skyrim")
        self.original = self.snapshot(self.source)
        replacement = patch.object(ios, "run_devicectl", side_effect=self.transport)
        replacement.start()
        self.addCleanup(replacement.stop)

    def snapshot(self, root):
        return {p.relative_to(root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in root.rglob("*") if p.is_file()}

    def existing(self):
        self.remote.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(self.source, self.remote)

    def option(self, arguments, key):
        return arguments[arguments.index(key) + 1]

    def transport(self, arguments, **options):
        arguments = list(arguments)
        self.calls.append(arguments)
        self.assertNotIn("--remove-existing-content", arguments)
        self.assertEqual(self.option(arguments, "--device"), "test-device-udid")
        if arguments[:3] == ["device", "info", "lockState"]:
            return {"passcodeRequired": self.locked, "unlockedSinceBoot": True}
        if arguments[:3] == ["device", "info", "apps"]:
            if self.fail_apps:
                raise ios.DeviceError("installed-app query failed")
            self.assertEqual(self.option(arguments, "--bundle-id"), "example.owned.skyrim")
            return {"apps": [{"bundleIdentifier": "example.owned.skyrim"}] if self.installed else []}
        self.assertEqual(self.option(arguments, "--domain-type"), "appDataContainer")
        self.assertEqual(self.option(arguments, "--domain-identifier"), "example.owned.skyrim")
        if arguments[:3] == ["device", "info", "files"]:
            if self.fail_list:
                raise ios.DeviceError("permission or transport failure")
            relative = self.option(arguments, "--subdirectory") if "--subdirectory" in arguments else ""
            path = self.container / relative
            self.assertTrue(path.is_dir(), "A missing directory must be established by its successful parent listing")
            entries = path.rglob("*") if "--recurse" in arguments else path.iterdir()
            return {"files": [{"relativePath": p.relative_to(path).as_posix(),
                               "resources": {"isDirectory": p.is_dir(), "isSymbolicLink": p.is_symlink()},
                               "metadata": {"size": p.stat().st_size}} for p in entries]}
        direction = arguments[2]
        if direction == "to":
            if self.fail_copy:
                raise ios.DeviceError("copy failed")
            relative = self.option(arguments, "--destination")
            self.assertTrue(relative == ios.ROMFS or relative.startswith(ios.ROMFS + "/"))
            self.assertNotIn("..", relative.split("/"))
            source, destination = Path(self.option(arguments, "--source")), self.container / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                if self.nest_directory:
                    destination = destination / source.name
                shutil.copytree(source, destination, dirs_exist_ok=True)
            else:
                # Model the CLI's unchanged-file optimization. Staging must
                # defeat it even when corrupted bytes retain size and mtime.
                if (destination.exists() and source.stat().st_size == destination.stat().st_size
                        and source.stat().st_mtime_ns == destination.stat().st_mtime_ns):
                    return {}
                shutil.copy2(source, destination)
                if self.corrupt_upload:
                    destination.write_bytes(b"wrong uploaded bytes")
            return {}
        self.assertEqual(direction, "from")
        if self.fail_readback:
            raise ios.DeviceError("readback copy failed")
        relative = self.option(arguments, "--source")
        self.assertTrue(relative in (ios.ROMFS, ios.SAVES) or relative.startswith(ios.ROMFS + "/"))
        source, destination = self.container / relative, Path(self.option(arguments, "--destination"))
        if source.is_dir():
            shutil.copytree(source, destination)
        else:
            shutil.copy2(source, destination)
        return {}

    def assert_preserved(self):
        self.assertEqual(self.snapshot(self.source), self.original)
        self.assertEqual((self.save / "important.sav").read_bytes(), b"preserve player progress")

    def test_fresh_copy_and_verified_reuse_do_not_touch_saves_or_source(self):
        first = self.device.sync_romfs(self.source)
        self.assertTrue(first["verified"])
        self.assertEqual(first["files"], 5)
        self.assertEqual(first["copied"], 5)
        start = len(self.calls)
        second = self.device.sync_romfs(self.source)
        self.assertEqual(second["copied"], 0)
        calls = self.calls[start:]
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in calls))
        self.assertEqual(sum(call[:3] == ["device", "copy", "from"] for call in calls), 1)
        self.assert_preserved()

    def test_missing_zero_truncated_and_same_metadata_corrupt_files_are_repaired(self):
        self.existing()
        (self.remote / "Data/missing.bsa").unlink()
        (self.remote / "Data/zero.bsa").write_bytes(b"")
        (self.remote / "Data/texture 'quoted'.bsa").write_bytes(b"truncated")
        ini = self.remote / "Skyrim.ini"
        stamp = ini.stat().st_mtime_ns
        ini.write_bytes(b"x" * ini.stat().st_size)
        os.utime(ini, ns=(stamp, stamp))
        result = self.device.sync_romfs(self.source)
        self.assertTrue(result["verified"])
        self.assertEqual(result["copied"], 4)
        uploads = [call for call in self.calls if call[:3] == ["device", "copy", "to"]]
        self.assertEqual(len(uploads), 4)
        self.assertTrue(all("--remove-existing-content" not in call for call in uploads))
        self.assertEqual({p.relative_to(self.remote).as_posix(): p.read_bytes()
                          for p in self.remote.rglob("*") if p.is_file()},
                         {name: value[0] for name, value in self.original.items()})
        self.assert_preserved()

    def test_extra_file_stops_before_writes_and_preserves_existing_data(self):
        self.existing()
        (self.remote / "Data/obsolete.esm").write_bytes(b"another version")
        with self.assertRaisesRegex(ios.DeviceError, "new --bundle-id"):
            self.device.sync_romfs(self.source)
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))
        self.assertEqual((self.remote / "Data/obsolete.esm").read_bytes(), b"another version")
        self.assert_preserved()

    def test_copy_failure_and_incorrect_readback_never_succeed(self):
        self.fail_copy = True
        with self.assertRaisesRegex(ios.DeviceError, "copy failed"):
            self.device.sync_romfs(self.source)
        self.fail_copy = False
        self.existing()
        (self.remote / "Data/zero.bsa").write_bytes(b"")
        self.corrupt_upload = True
        with self.assertRaisesRegex(ios.DeviceError, "checksum mismatch"):
            self.device.sync_romfs(self.source)
        self.assert_preserved()

    def test_duplicate_directory_nesting_is_rejected_before_verified_result(self):
        self.nest_directory = True
        with self.assertRaisesRegex(ios.DeviceError, "unexpected files"):
            self.device.sync_romfs(self.source)
        self.assert_preserved()

    def test_locked_device_and_inventory_errors_are_not_treated_as_empty(self):
        self.locked = True
        with self.assertRaisesRegex(ios.DeviceError, "Unlock"):
            self.device.sync_romfs(self.source)
        self.assertEqual(len(self.calls), 1)
        self.locked = False
        self.fail_list = True
        with self.assertRaisesRegex(ios.DeviceError, "permission or transport"):
            self.device.sync_romfs(self.source)
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))

    def test_source_and_device_symlinks_and_path_traversal_are_rejected(self):
        link = self.source / "Data/link"
        link.symlink_to(self.save, target_is_directory=True)
        with self.assertRaisesRegex(ios.DeviceError, "Symlinks"):
            self.device.sync_romfs(self.source)
        self.assertEqual(self.calls, [])
        link.unlink()
        self.remote.parent.mkdir(parents=True)
        self.remote.symlink_to(self.save, target_is_directory=True)
        with self.assertRaisesRegex(ios.DeviceError, "real directories"):
            self.device.sync_romfs(self.source)
        for value in ("../save/important.sav", "/Documents/save", "Data/../save", "Data//file"):
            with self.subTest(path=value), self.assertRaises(ios.DeviceError):
                self.device._copy_to(self.source / "Skyrim.ini", value)
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))

    def test_save_backup_and_verification_include_zero_byte_files_and_preserve_backup(self):
        (self.save / "Saves").mkdir()
        (self.save / "Saves/owned 'save'.sav0").write_bytes(b"latest player progress")
        (self.save / "Saves/owned 'save'.SAV1").write_bytes(b"")
        destination = self.root / "save backups" / "new 'baseline'"
        backup = self.device.backup_saves(destination)
        self.assertTrue(backup["present"])
        self.assertEqual(backup["path"], str(destination))
        self.assertEqual(backup["files"]["Saves/owned 'save'.SAV1"]["bytes"], 0)
        self.assertEqual(len(backup["files"]), 3)
        original_backup = self.snapshot(destination)
        self.device.verify_saves(backup)
        self.assertEqual(self.snapshot(destination), original_backup)
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))
        self.assert_preserved()

    def test_absent_app_or_save_is_distinct_from_app_or_filesystem_query_failure(self):
        destination = self.root / "unused backup"
        self.installed = False
        self.fail_list = True
        self.assertEqual(self.device.backup_saves(destination), {"present": False})
        self.assertFalse(any(call[:3] == ["device", "info", "files"] for call in self.calls))
        self.installed = True
        self.fail_list = False
        shutil.rmtree(self.save)
        self.assertEqual(self.device.backup_saves(destination), {"present": False})
        self.assertFalse(destination.exists())
        self.fail_list = True
        with self.assertRaisesRegex(ios.DeviceError, "permission or transport"):
            self.device.backup_saves(destination)
        self.fail_apps = True
        with self.assertRaisesRegex(ios.DeviceError, "installed-app query"):
            self.device.backup_saves(destination)

    def test_save_backup_copy_failure_does_not_return_a_successful_baseline(self):
        self.fail_readback = True
        destination = self.root / "failed backup"
        with self.assertRaisesRegex(ios.DeviceError, "Save backup failed") as failure:
            self.device.backup_saves(destination)
        self.assertIn(str(destination), str(failure.exception))
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))
        self.assert_preserved()

    def test_incomplete_save_readback_is_retained_but_never_accepted_as_backup(self):
        destination = self.root / "partial backup"
        def incomplete_copy(arguments, **options):
            result = self.transport(arguments, **options)
            if arguments[:3] == ["device", "copy", "from"]:
                (destination / "important.sav").write_bytes(b"")
            return result
        with patch.object(ios, "run_devicectl", side_effect=incomplete_copy), \
             self.assertRaisesRegex(ios.DeviceError, "complete file inventory"):
            self.device.backup_saves(destination)
        self.assertTrue(destination.is_dir())
        self.assertEqual((destination / "important.sav").read_bytes(), b"")
        self.assert_preserved()

    def test_changed_or_missing_device_saves_stop_verification_and_keep_backup(self):
        destination = self.root / "independent backup"
        backup = self.device.backup_saves(destination)
        original_backup = self.snapshot(destination)
        (self.save / "important.sav").write_bytes(b"unexpectedly changed progress")
        with self.assertRaisesRegex(ios.DeviceError, "save files changed") as changed:
            self.device.verify_saves(backup)
        self.assertIn(str(destination), str(changed.exception))
        self.assertEqual(self.snapshot(destination), original_backup)
        shutil.rmtree(self.save)
        with self.assertRaisesRegex(ios.DeviceError, "saves are missing") as missing:
            self.device.verify_saves(backup)
        self.assertIn(str(destination), str(missing.exception))
        self.assertEqual(self.snapshot(destination), original_backup)
        self.assertFalse(any(call[:3] == ["device", "copy", "to"] for call in self.calls))

    def test_existing_backup_is_never_overwritten_and_modified_backup_fails_closed(self):
        destination = self.root / "existing backup"
        backup = self.device.backup_saves(destination)
        original_backup = self.snapshot(destination)
        with self.assertRaisesRegex(ios.DeviceError, "never overwritten"):
            self.device.backup_saves(destination)
        self.assertEqual(self.snapshot(destination), original_backup)
        (destination / "important.sav").write_bytes(b"locally modified baseline")
        modified_backup = self.snapshot(destination)
        with self.assertRaisesRegex(ios.DeviceError, "local save backup has changed"):
            self.device.verify_saves(backup)
        self.assertEqual(self.snapshot(destination), modified_backup)

    def test_save_symlinks_are_rejected_before_copy(self):
        (self.save / "link").symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(ios.DeviceError, "Symlinks"):
            self.device.backup_saves(self.root / "must not exist")
        self.assertFalse(any(call[:2] == ["device", "copy"] for call in self.calls))


if __name__ == "__main__":
    unittest.main()
