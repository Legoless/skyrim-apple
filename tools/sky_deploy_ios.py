#!/usr/bin/env python3
"""Sign, install, verify game files, and launch Skyrim on a physical iOS device."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time

if __name__ == "__main__":
    sys.dont_write_bytecode = True

from sky_ios_device import Device, run_devicectl, select_device
from sky_setup import workspace_problems
from sky_tools import check_tools, tool_environment

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "skyhost/build/ios"


def prompt(label, default=None):
    if not sys.stdin.isatty():
        if default is not None:
            return default
        raise ValueError(f"Supply {label} when running without a terminal.")
    try:
        value = input(label + (f" [{default}]" if default else "") + ": ").strip()
    except EOFError:
        raise ValueError("No input supplied for " + label) from None
    return value or default or ""


def signing_choices(team, bundle_id):
    team = (team or prompt("--team (Apple development team ID from Xcode Settings > Accounts)")).strip().upper()
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise ValueError("--team must be a 10-character Apple development team ID.")
    bundle_id = bundle_id or prompt("--bundle-id", "dev.skyhost.skyrim.t" + team.lower())
    if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", bundle_id):
        raise ValueError("--bundle-id must be a reverse-domain bundle identifier, such as dev.example.skyrim.")
    return team, bundle_id


def run_logged(command, env, log):
    print(f"[sky_deploy_ios] {log.stem}; log: {log}", flush=True)
    with log.open("w") as stream:
        result = subprocess.run([str(part) for part in command], cwd=REPO, env=env,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = log.read_text(errors="replace").splitlines()[-30:]
        raise RuntimeError(f"{log.stem} failed. Check Xcode account/signing and {log}.\n" + "\n".join(tail))


def deploy(args):
    work = args.work_dir.expanduser().resolve()
    romfs = (args.romfs or work / "data/switch/skyrim/base/Update/RomFS").expanduser().resolve()
    problems = check_tools(work, ios=True) if args.romfs else workspace_problems(work, ios=True)
    if problems:
        raise ValueError("\n".join(problems) + "\nRun setup-skyrim.sh with your XCI and keys, adding --ios.")
    if not (romfs / "Skyrim.ini").is_file() or not (romfs / "Data").is_dir():
        raise ValueError(f"Missing extracted RomFS at {romfs}; run setup or supply --romfs PATH.")
    team, bundle = signing_choices(args.team, args.bundle_id)
    selected = select_device(args.device)
    if not selected.get("udid"):
        raise ValueError("The selected device did not report a UDID; reconnect it and trust this Mac.")
    identifier = selected["identifier"]
    device = Device(identifier, bundle)
    device.require_unlocked()
    env = tool_environment(work)
    env["SKY_WORK_DIR"] = str(work)
    logs = work / "build/deploy-ios"
    logs.mkdir(parents=True, exist_ok=True)
    report_path = logs / "deployment.json"
    report_path.unlink(missing_ok=True)
    print("[sky_deploy_ios] Backing up existing saves before installation.", flush=True)
    save_backup = device.backup_saves(logs / "save-backups" / f"{time.time_ns()}-{bundle}")
    derived = args.derived_data.expanduser().resolve()
    print(f"[sky_deploy_ios] Deploying {bundle} to {selected['name']}.", flush=True)
    run_logged([env["SKY_PYTHON"], REPO / "skyhost/ios/build.py", "--sdk", "device",
                "--configuration", "Release", "--work-dir", work, "--bundle-id", bundle,
                "--prepare-only"], env, logs / "prepare.log")
    run_logged(["/usr/bin/xcrun", "xcodebuild", "-project", BUILD / "SkyrimIOS.xcodeproj",
                "-scheme", "SkyrimIOS", "-configuration", "Release", "-sdk", "iphoneos",
                "-destination", "id=" + selected["udid"], "-derivedDataPath", derived,
                "-allowProvisioningUpdates", "-allowProvisioningDeviceRegistration",
                "DEVELOPMENT_TEAM=" + team, "CODE_SIGN_STYLE=Automatic",
                "CODE_SIGN_IDENTITY=Apple Development", "CODE_SIGNING_ALLOWED=YES", "build"],
               env, logs / "signed-release.log")
    app = derived / "Build/Products/Release-iphoneos/SkyrimIOS.app"
    with (app / "Info.plist").open("rb") as stream:
        if plistlib.load(stream).get("CFBundleIdentifier") != bundle:
            raise ValueError("Built app bundle ID does not match the selected --bundle-id.")
    run_logged(["/usr/bin/codesign", "--verify", "--strict", app], env, logs / "signature.log")
    device.require_unlocked()
    print("[sky_deploy_ios] Installing the signed Release build.", flush=True)
    run_devicectl(["device", "install", "app", "--device", identifier, str(app)], timeout=300)
    # The existing UIKit-only mode keeps the device awake without loading any
    # guest/game files while their contents are being replaced and verified.
    print("[sky_deploy_ios] Keeping the device awake while verifying game files.", flush=True)
    holding = run_devicectl(["device", "process", "launch", "--device", identifier, "--terminate-existing",
                            "--environment-variables", '{"SKY_CONTROLS_TEST_MODE":"1"}',
                            bundle, "--", "--controls-test-mode"], timeout=120)
    try:
        transferred = device.sync_romfs(romfs)
        if not transferred.get("verified"):
            raise ValueError("RomFS verification did not complete; the game will not be launched.")
        device.verify_saves(save_backup)
        run_devicectl(["device", "process", "launch", "--device", identifier, "--terminate-existing",
                       "--environment-variables", '{"SKY_CONTROLS_TEST_MODE":"0"}', bundle], timeout=120)
    except BaseException:
        pid = holding.get("process", {}).get("processIdentifier")
        if isinstance(pid, int) and pid > 0:
            try:
                run_devicectl(["device", "process", "terminate", "--device", identifier, "--pid", str(pid)], timeout=30)
            except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
                print("[sky_deploy_ios] Could not close the transfer screen: " + str(error), file=sys.stderr)
        raise
    report = {"device": identifier, "bundle_id": bundle, "team": team, "romfs": str(romfs),
              "app": str(app), "save_backup": save_backup.get("path"), "saves_verified": True, **transferred}
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"[sky_deploy_ios] Release launched; {transferred['files']} RomFS files verified.", flush=True)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=Path(os.environ.get("SKY_WORK_DIR", REPO / "skyrim-work")))
    parser.add_argument("--team", help="Apple development team ID; prompted if omitted")
    parser.add_argument("--device", help="Physical device name or UDID; selected from connected devices if omitted")
    parser.add_argument("--bundle-id", help="Bundle ID; defaults to dev.skyhost.skyrim.t<team-id>")
    parser.add_argument("--romfs", type=Path, help="Use an existing extracted RomFS instead of the prepared cartridge workspace")
    parser.add_argument("--derived-data", type=Path, default=BUILD / "DerivedData")
    args = parser.parse_args(argv)
    try:
        deploy(args)
    except KeyboardInterrupt:
        print("\n[sky_deploy_ios] Deployment cancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("[sky_deploy_ios] " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
