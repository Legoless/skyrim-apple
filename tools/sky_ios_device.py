#!/usr/bin/env python3
"""Select an iOS device and synchronize only the app's verified RomFS files."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

ROMFS = "Documents/Skyrim/RomFS"
SAVES = "Documents/save"


class DeviceError(RuntimeError):
    pass


def run_devicectl(arguments, timeout=120):
    """Return devicectl's result object; temporary JSON is always removed."""
    with tempfile.TemporaryDirectory(prefix="sky-device-json-") as temporary:
        output = Path(temporary) / "result.json"
        command = ["/usr/bin/xcrun", "devicectl", "--quiet", "--timeout", str(timeout),
                   "--json-output", str(output), *map(str, arguments)]
        try:
            completed = subprocess.run(command, text=True, capture_output=True, timeout=timeout + 10)
        except subprocess.TimeoutExpired:
            raise DeviceError("devicectl timed out; reconnect and unlock the device, then retry.") from None
        except OSError as error:
            raise DeviceError(f"Unable to run Xcode's devicectl: {error}") from None
        try:
            payload = json.loads(output.read_text())
        except (OSError, ValueError):
            payload = {}
        if completed.returncode or payload.get("error"):
            detail = completed.stderr.strip() or completed.stdout.strip()
            if not detail:
                detail = json.dumps(payload.get("error", {}))
            raise DeviceError("devicectl failed: " + detail[-2000:])
        if not isinstance(payload.get("result"), dict):
            raise DeviceError("devicectl did not return a valid JSON result.")
        return payload["result"]


def list_devices():
    devices = []
    for item in run_devicectl(["list", "devices"]).get("devices", []):
        hardware = item.get("hardwareProperties", {})
        connection = item.get("connectionProperties", {})
        properties = item.get("deviceProperties", {})
        if (hardware.get("platform") != "iOS" or hardware.get("reality") != "physical"
                or connection.get("pairingState") != "paired"):
            continue
        udid = hardware.get("udid")
        identifier = udid or item.get("identifier")
        if identifier:
            devices.append({"identifier": identifier, "udid": udid,
                            "core_device_id": item.get("identifier"),
                            "name": properties.get("name", identifier),
                            "state": connection.get("tunnelState", "unknown")})
    return devices


def select_device(selector=None):
    devices = list_devices()
    if selector:
        matches = [item for item in devices if str(selector).casefold() in
                   {str(item.get(key, "")).casefold()
                    for key in ("identifier", "udid", "core_device_id", "name")}]
        if len(matches) == 1:
            return matches[0]
        raise DeviceError("Device selector did not identify one paired iPhone or iPad; check --device.")
    connected = [item for item in devices if item.get("state") == "connected"]
    if connected:
        devices = connected
    if len(devices) == 1:
        return devices[0]
    if not devices:
        raise DeviceError("No paired iPhone or iPad found. Connect it, trust this Mac, and enable Developer Mode.")
    if not sys.stdin.isatty():
        raise DeviceError("Multiple paired iOS devices found; supply --device with a name or UDID.")
    for index, item in enumerate(devices, 1):
        print(f"{index}. {item['name']} ({item['identifier']}, {item['state']})")
    while True:
        try:
            answer = input("Device number: ").strip()
        except EOFError:
            raise DeviceError("No device selected; supply --device with a name or UDID.") from None
        if answer.isdigit() and 1 <= int(answer) <= len(devices):
            return devices[int(answer) - 1]


def _relative(value):
    if (not isinstance(value, str) or not value or "\\" in value
            or any(part in ("", ".", "..") for part in value.split("/"))
            or PurePosixPath(value).is_absolute()):
        raise DeviceError("Unsafe relative RomFS path.")
    return value


def _fingerprint(path):
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise DeviceError("RomFS changed while its checksum was being read.")
    return {"bytes": after.st_size, "sha256": digest.hexdigest()}


def _inventory(root):
    if root.is_symlink() or not root.is_dir():
        raise DeviceError("RomFS must be a directory, never a symlink.")
    def walk_error(error):
        raise DeviceError(f"Cannot read the complete RomFS inventory: {error}")
    files = {}
    for directory, directories, names in os.walk(root, followlinks=False, onerror=walk_error):
        for name in directories + names:
            path = Path(directory) / name
            if path.is_symlink():
                raise DeviceError("Symlinks are not allowed in RomFS.")
            if name in directories:
                continue
            if not stat.S_ISREG(path.stat().st_mode):
                raise DeviceError("Only regular files are allowed in RomFS.")
            if name == ".DS_Store":
                continue
            relative = _relative(path.relative_to(root).as_posix())
            files[relative] = _fingerprint(path)
    return files


class Device:
    def __init__(self, identifier, bundle_id):
        if not identifier or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", bundle_id or ""):
            raise DeviceError("A device identifier and valid app bundle ID are required.")
        self.identifier = str(identifier)
        self.bundle_id = bundle_id

    def require_unlocked(self):
        result = run_devicectl(["device", "info", "lockState", "--device", self.identifier])
        if result.get("passcodeRequired") is not False or result.get("unlockedSinceBoot") is not True:
            raise DeviceError("Unlock the selected device and keep its display awake before deploying.")
        return result

    def _domain(self):
        return ["--device", self.identifier, "--domain-type", "appDataContainer",
                "--domain-identifier", self.bundle_id]

    def _list(self, relative="", recurse=False):
        arguments = ["device", "info", "files", *self._domain(),
                     "--recurse" if recurse else "--no-recurse"]
        if relative:
            arguments.extend(("--subdirectory", relative))
        result = run_devicectl(arguments)
        if not isinstance(result.get("files"), list):
            raise DeviceError("devicectl returned no file inventory.")
        return result["files"]

    def _directory_exists(self, relative):
        # Successful parent listings prove absence without hiding transport,
        # permission or locked-device errors behind a generic 'not found'.
        parent = ""
        for component in _relative(relative).split("/"):
            entries = self._list(parent)
            matches = [item for item in entries if item.get("relativePath") == component]
            if not matches:
                return False
            if len(matches) != 1:
                raise DeviceError("Ambiguous device RomFS parent inventory.")
            resources = matches[0].get("resources", {})
            if resources.get("isSymbolicLink") is not False or resources.get("isDirectory") is not True:
                raise DeviceError("The device RomFS path must contain only real directories.")
            parent = parent + "/" + component if parent else component
        return True

    def _romfs_files(self):
        if not self._directory_exists(ROMFS):
            return None
        files = set()
        for item in self._list(ROMFS, recurse=True):
            relative = _relative(item.get("relativePath"))
            resources = item.get("resources", {})
            if resources.get("isSymbolicLink") is not False:
                raise DeviceError("Symlinks are not allowed in device RomFS.")
            if not resources.get("isDirectory") and PurePosixPath(relative).name != ".DS_Store":
                files.add(relative)
        return files

    def _app_installed(self):
        result = run_devicectl(["device", "info", "apps", "--device", self.identifier,
                               "--include-default-apps", "--bundle-id", self.bundle_id])
        apps = result.get("apps")
        if not isinstance(apps, list):
            raise DeviceError("devicectl returned no installed-app inventory.")
        if not apps:
            return False
        if len(apps) != 1 or apps[0].get("bundleIdentifier") != self.bundle_id:
            raise DeviceError("devicectl returned an ambiguous installed-app inventory.")
        return True

    def _save_files(self):
        if not self._directory_exists(SAVES):
            return None
        files = {}
        for item in self._list(SAVES, recurse=True):
            relative = _relative(item.get("relativePath"))
            resources = item.get("resources", {})
            if resources.get("isSymbolicLink") is not False:
                raise DeviceError("Symlinks are not allowed in device saves.")
            if resources.get("isDirectory") or PurePosixPath(relative).name == ".DS_Store":
                continue
            size = item.get("metadata", {}).get("size")
            if not isinstance(size, int) or size < 0 or relative in files:
                raise DeviceError("devicectl returned an invalid save-file inventory.")
            files[relative] = size
        return files

    def backup_saves(self, destination):
        """Read existing saves into a new private backup before installation."""
        self.require_unlocked()
        if not self._app_installed():
            return {"present": False}
        remote = self._save_files()
        if remote is None:
            return {"present": False}
        destination = Path(destination).expanduser().absolute()
        if destination.exists() or destination.is_symlink():
            raise DeviceError("Use a new, nonexistent save-backup directory; existing backups are never overwritten.")
        destination.parent.mkdir(parents=True, exist_ok=True)
        print("[sky_ios_device] Backing up existing saves before installation...", flush=True)
        try:
            run_devicectl(["device", "copy", "from", *self._domain(), "--source", SAVES,
                           "--destination", str(destination)], timeout=300)
            files = _inventory(destination)
            if {name: value["bytes"] for name, value in files.items()} != remote:
                raise DeviceError("Save readback did not match the device's complete file inventory.")
            destination.chmod(0o700)
        except (DeviceError, OSError) as error:
            raise DeviceError(f"Save backup failed: {error}. Any partial backup is retained at {destination}.") from None
        print(f"[sky_ios_device] Save backup complete: {len(files)} files at {destination}.", flush=True)
        return {"present": True, "path": str(destination), "files": files}

    def verify_saves(self, backup):
        """Fail closed if an existing save changed; never restore automatically."""
        if not backup.get("present"):
            return
        path = Path(backup["path"])
        try:
            if _inventory(path) != backup["files"]:
                raise DeviceError("The independent local save backup has changed.")
            self.require_unlocked()
            if not self._app_installed() or self._save_files() is None:
                raise DeviceError("The app's existing saves are missing after deployment.")
            with tempfile.TemporaryDirectory(prefix="sky-save-readback-") as temporary:
                received = Path(temporary) / "save"
                run_devicectl(["device", "copy", "from", *self._domain(), "--source", SAVES,
                               "--destination", str(received)], timeout=300)
                if _inventory(received) != backup["files"]:
                    raise DeviceError("The app's existing save files changed during deployment.")
        except (DeviceError, OSError) as error:
            raise DeviceError(f"Save preservation check failed: {error} Backup retained at {path}.") from None
        print("[sky_ios_device] Existing saves verified unchanged.", flush=True)

    def _copy_to(self, source, relative=""):
        destination = ROMFS + "/" + _relative(relative) if relative else ROMFS
        arguments = ["device", "copy", "to", *self._domain(), "--source", str(source),
                     "--destination", destination]
        run_devicectl(arguments, timeout=3600)

    def _copy_from(self, destination, relative=""):
        source = ROMFS + "/" + _relative(relative) if relative else ROMFS
        destination.parent.mkdir(parents=True, exist_ok=True)
        run_devicectl(["device", "copy", "from", *self._domain(), "--source", source,
                       "--destination", str(destination)], timeout=3600)

    def sync_romfs(self, source):
        """Verify existing bytes, repair only mismatches, then verify readback.

        Unexpected destination files stop deployment before any writes. No
        operation deletes data or targets the app container root or save folder.
        """
        source = Path(source).expanduser().absolute()
        print("[sky_ios_device] Hashing source RomFS files...", flush=True)
        expected = _inventory(source)
        if (not expected.get("Skyrim.ini", {}).get("bytes")
                or not any(name.startswith("Data/") and value["bytes"]
                           for name, value in expected.items())):
            raise DeviceError("Source RomFS needs Skyrim.ini and nonempty Data files.")
        byte_count = sum(value["bytes"] for value in expected.values())
        print(f"[sky_ios_device] Source inventory: {len(expected)} files, {byte_count:,} bytes.", flush=True)
        self.require_unlocked()
        remote_files = self._romfs_files()
        if remote_files and remote_files - expected.keys():
            raise DeviceError("Device RomFS contains files from another extraction. "
                              "Use a new --bundle-id to preserve the existing app's data; no files were copied.")
        whole_copy = not remote_files
        copied = len(expected) if whole_copy else 0
        with tempfile.TemporaryDirectory(prefix="sky-romfs-readback-") as temporary:
            temporary = Path(temporary)
            if whole_copy:
                print("[sky_ios_device] Copying RomFS to the device...", flush=True)
                self._copy_to(source)
            readback = temporary / "RomFS"
            print(f"[sky_ios_device] Reading back device RomFS for SHA-256 verification "
                  f"(source: {byte_count:,} bytes)...", flush=True)
            self._copy_from(readback)
            actual = _inventory(readback)
            if actual.keys() - expected.keys():
                raise DeviceError("Device RomFS contains unexpected files after readback; deployment is not verified.")
            pending = [name for name, value in expected.items() if actual.get(name) != value]
            shutil.rmtree(readback)
            if not whole_copy:
                copied = len(pending)
            if pending:
                print(f"[sky_ios_device] Repairing and verifying {len(pending)} RomFS files...", flush=True)
            for name in pending:
                staged = temporary / "upload" / name
                staged.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / name, staged)
                # devicectl skips unchanged metadata. A fresh staging timestamp
                # forces a transfer without modifying the owned source files.
                stamp = time.time_ns() + 1_000_000_000
                os.utime(staged, ns=(stamp, stamp))
                if _fingerprint(staged) != expected[name]:
                    raise DeviceError("Source RomFS changed while deploying; retry from the original extraction.")
                self._copy_to(staged, name)
                staged.unlink()
                received = temporary / "verified" / name
                self._copy_from(received, name)
                if received.is_symlink() or not received.is_file() or _fingerprint(received) != expected[name]:
                    raise DeviceError(f"Device RomFS checksum mismatch: {name}; deployment is not verified.")
                received.unlink()
        print(f"[sky_ios_device] Verified {len(expected)} RomFS files, {byte_count:,} bytes; "
              f"copied {copied} files.", flush=True)
        return {"files": len(expected), "bytes": byte_count,
                "copied": copied, "verified": True}
