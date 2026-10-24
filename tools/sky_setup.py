#!/usr/bin/env python3
"""Prepare the supported Skyrim build from owned game dumps using pinned tools."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

if __name__ == "__main__":
    sys.dont_write_bytecode = True

from sky_tools import check_tools, ensure_tools, tool_environment
from sky_extract_xci import extract_xci
from sky_shader_cache import REFERENCE as SHADER_REFERENCE, fingerprint as shader_fingerprint

REPO = Path(__file__).resolve().parents[1]
PIN = "base"
MODULES = ("main", "sdk")
MODULE_FILES = ("patched.bin", "fixups.tsv", "gates.tsv")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_workspace(source, work):
    if source is not None and source.is_dir():
        require(not work.resolve().is_relative_to(source.resolve()),
                "Keep --work-dir outside the original split dump; setup must not modify that dump.")


def run(command, env):
    # Commands that receive console keys are intentionally not echoed.
    subprocess.run([str(part) for part in command], env=env, check=True, cwd=REPO)


def game_paths(work):
    root = work / "data/switch/skyrim" / PIN
    return root / "Update", root / "meta"


def validate_game(game):
    for name in MODULES:
        source = game / "ExeFS" / name
        require(source.is_file(), f"Missing cartridge ExeFS/{name}.")
        with source.open("rb") as stream:
            header = stream.read(0x80)
        require(len(header) == 0x80 and header[:4] == b"NSO0", f"Invalid cartridge ExeFS/{name}.")
    nacp = game / "Control/control.nacp"
    require(nacp.is_file() and nacp.stat().st_size >= 0x3158,
            "The cartridge Control NACP is required for its save settings.")
    romfs = game / "RomFS"
    require((romfs / "Skyrim.ini").is_file() and (romfs / "Data").is_dir(),
            "A complete extracted RomFS with Skyrim.ini and Data is required.")
    archives = list((romfs / "Data").glob("*.bsa"))
    require(archives, "RomFS/Data contains no game archives.")
    for path in (romfs / "Data").iterdir():
        if path.suffix.lower() in (".bsa", ".esm", ".esl"):
            require(path.is_file() and path.stat().st_size > 0,
                    f"Incomplete game file: {path.name}. Restore it from your dump.")


def shader_problems(allow_missing=False):
    cache = REPO / "skyrim-work/build/shaders"
    try:
        if cache.is_symlink() or (cache.exists() and not cache.is_dir()):
            return [f"Local shader cache must be a regular directory: {cache}."]
        sources = sorted(cache.glob("*.metal"))
        reflections = sorted(path for path in cache.glob("*.json")
                             if re.fullmatch(r"[0-9a-f]{64}", path.stem))
        for reflection in reflections:
            if not reflection.with_suffix(".metal").is_file():
                return [f"Local shader source is missing for {reflection.name}. Restore or translate that pair."]
        if not sources:
            if allow_missing and (not cache.exists() or not any(cache.iterdir())):
                return []
            return [f"Local translated shaders are required in {cache}. "
                    "Run setup with your XCI and prod.keys to rebuild them automatically. See skyhost/README.md."]
        for source in sources:
            reflection = source.with_suffix(".json")
            if not reflection.is_file():
                return [f"Local shader reflection is missing for {source.name}. Restore or translate that pair."]
            if not source.stat().st_size or not reflection.stat().st_size:
                return [f"Local shader pair is empty for {source.name}. Restore or translate that pair."]
        if shader_fingerprint(cache) != SHADER_REFERENCE:
            return ["Local shader cache differs from the recorded v327680 reference. "
                    "It was preserved; use a verified cache or move it to a backup before rerunning setup."]
    except (OSError, ValueError) as error:
        return [f"Local shader cache could not be verified: {error}"]
    return []


def rewrite_identity(game):
    return {
        "modules": {name: sha256(game / "ExeFS" / name) for name in MODULES},
        "rewriter": sha256(REPO / "tools/sky_patch_nso.py"),
        "requirements": sha256(REPO / "tools/requirements-skyrim.txt"),
        "control_nacp": sha256(game / "Control/control.nacp")
        if (game / "Control/control.nacp").is_file() else None,
    }


def rewrite_is_current(output, identity):
    try:
        state = json.loads((output / "setup.json").read_text())
        if state["identity"] != identity:
            return False
        for name in MODULES:
            for suffix in MODULE_FILES:
                if f"{name}.{suffix}" not in state["outputs"]:
                    return False
        return all((output / name).is_file() and sha256(output / name) == digest
                   for name, digest in state["outputs"].items())
    except (OSError, KeyError, ValueError, TypeError):
        return False


def prepare_rewrite(work, game, env):
    output = work / "build/rewrite-native"
    identity = rewrite_identity(game)
    if rewrite_is_current(output, identity):
        print("[sky_setup] Rewritten modules are current.", flush=True)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    backup = output.with_name("rewrite-native.previous")
    require(not backup.exists(), f"Preserved rewrite backup exists at {backup}; inspect it before retrying.")
    with tempfile.TemporaryDirectory(prefix="rewrite-", dir=output.parent) as temporary:
        staged = Path(temporary) / "output"
        for name in MODULES:
            command = [env["SKY_PYTHON"], REPO / "tools/sky_patch_nso.py", game / "ExeFS" / name, staged, name]
            if name == "main" and (game / "Control/control.nacp").is_file():
                command.extend(("--control-nacp", game / "Control/control.nacp"))
            run(command, env)
            for suffix in MODULE_FILES:
                path = staged / f"{name}.{suffix}"
                require(path.is_file() and (suffix != "patched.bin" or path.stat().st_size > 0),
                        f"Rewriter did not produce {name}.{suffix}.")
        state = {"identity": identity, "outputs": {
            path.name: sha256(path) for path in sorted(staged.iterdir()) if path.is_file()}}
        (staged / "setup.json").write_text(json.dumps(state, indent=2) + "\n")
        if output.exists():
            output.rename(backup)
        try:
            staged.rename(output)
        except BaseException:
            if backup.exists():
                backup.rename(output)
            raise
        if backup.exists():
            shutil.rmtree(backup)


def validate_inventory(metadata):
    for name in ("modules.tsv", "imports.tsv", "relocations.tsv"):
        require((metadata / name).is_file(), f"Missing generated inventory: {name}.")
    with (metadata / "modules.tsv").open() as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    require(rows, "Generated module inventory is empty.")
    require(all(int(row["unsupported_relocation_count"]) == 0 for row in rows),
            "Game inventory contains unsupported relocations.")


def record_romfs(game, metadata):
    """Keep a private file/hash manifest for detecting incomplete game transfers."""
    root = game / "RomFS"
    files = {str(path.relative_to(root)): path for path in sorted(root.rglob("*"))
             if path.is_file() and path.name != ".DS_Store"}
    destination = metadata / "romfs-files.json"
    try:
        previous = json.loads(destination.read_text())
    except (OSError, ValueError):
        previous = {}
    if previous:
        validate_romfs_manifest(game, metadata)
    result = {}
    for name, path in files.items():
        stat = path.stat()
        old = previous.get(name, {})
        if old.get("bytes") == stat.st_size and old.get("mtime_ns") == stat.st_mtime_ns and old.get("sha256"):
            result[name] = old
        else:
            result[name] = {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns, "sha256": sha256(path)}
    if result != previous:
        destination.write_text(json.dumps(result, indent=2) + "\n")


def validate_romfs_manifest(game, metadata):
    root = game / "RomFS"
    manifest = metadata / "romfs-files.json"
    require(manifest.is_file(), "Missing generated RomFS transfer manifest; run setup with your dump.")
    expected = json.loads(manifest.read_text())
    actual = {str(path.relative_to(root)): path for path in root.rglob("*")
              if path.is_file() and path.name != ".DS_Store"}
    require(actual.keys() == expected.keys(), "Extracted RomFS file inventory changed; restore it from your dump.")
    for name, path in actual.items():
        stat = path.stat()
        record = expected[name]
        require(stat.st_size == record["bytes"], f"Incomplete extracted game file: {name}.")
        if stat.st_mtime_ns != record["mtime_ns"]:
            require(sha256(path) == record["sha256"], f"Extracted game file checksum mismatch: {name}.")


def workspace_problems(work, ios=False):
    problems = check_tools(work, ios) + shader_problems()
    game, metadata = game_paths(work)
    try:
        validate_game(game)
        validate_inventory(metadata)
        validate_romfs_manifest(game, metadata)
        require(rewrite_is_current(work / "build/rewrite-native", rewrite_identity(game)),
                "Rewritten modules are missing or stale; run setup with your dump.")
    except (OSError, ValueError, KeyError) as error:
        problems.append(str(error))
    return problems


def input_path(value, option, label, valid):
    """Prompt only for omitted inputs on a terminal; explicit paths fail fast."""
    if value is not None:
        path = value.expanduser().resolve()
        require(valid(path), f"{label} not found: {path}. Supply {option} with an existing path.")
        return path
    require(sys.stdin.isatty(), f"Supply {option}, or run setup in a terminal to be prompted for {label.lower()}.")
    while True:
        try:
            answer = input(f"{label} path: ").strip()
        except EOFError:
            raise ValueError(f"No {label.lower()} supplied. Retry with {option}.") from None
        # Accept pasted paths with spaces, optional shell quotes, and a leading ~.
        if len(answer) >= 2 and answer[0] == answer[-1] and answer[0] in "\"'":
            answer = answer[1:-1]
        if answer:
            path = Path(answer).expanduser().resolve()
            if valid(path):
                return path
        print(f"Enter an existing {label.lower()} path, or press Ctrl-C to cancel.", file=sys.stderr)


def default_dump_path():
    """Find one repo-local dump without inspecting or changing its contents."""
    directory = REPO / "inputs"
    if not directory.is_dir():
        return None
    entries = list(directory.iterdir())
    candidates = [path for path in entries
                  if path.suffix.lower() == ".xci" and (path.is_file() or path.is_dir())]
    if any(re.fullmatch(r"[0-9]+", path.name) for path in entries):
        candidates.append(directory)
    require(len(candidates) <= 1,
            "Multiple game dumps found in inputs/. Supply the dump path or --base PATH explicitly.")
    return candidates[0] if candidates else None


def prepare(args):
    require(args.base is not None and args.base.exists(), "Supply your XCI file or split-dump directory.")
    validate_workspace(args.base, args.work_dir)
    require(args.keys is not None and args.keys.is_file(), "Supply --keys with your console's prod.keys file.")
    problems = shader_problems(allow_missing=True)
    require(not problems, " ".join(problems))
    env = ensure_tools(args.work_dir, args.ios)
    work = args.work_dir.resolve()
    game, metadata = extract_xci(args.base, args.keys, work, env)
    env.update(SK_ROOT=str(work), SKY_WORK_DIR=str(work), SKY_REWRITE_DIR=str(work / "build/rewrite-native"),
               SKY_META_DIR=str(metadata), SKY_ROMFS=str(game / "RomFS"),
               SKY_SHADER_CACHE_DIR=str(REPO / "skyrim-work/build/shaders"))
    validate_game(game)
    run([env["SKY_PYTHON"], REPO / "tools/sky_inventory.py", game / "ExeFS", metadata], env)
    validate_inventory(metadata)
    prepare_rewrite(work, game, env)
    record_romfs(game, metadata)
    run([env["SKY_PYTHON"], REPO / "skyhost/tools/rebuild_shaders.py",
         "--archive", game / "RomFS/Data/Skyrim - Shaders.bsa",
         "--main", work / "build/rewrite-native/main.patched.bin",
         "--output", REPO / "skyrim-work/build/shaders",
         "--work-dir", work, "--ensure"], env)
    if args.ios:
        run([env["SKY_PYTHON"], REPO / "skyhost/ios/build.py", "--sdk", "device",
             "--configuration", "Release", "--bundle-id", args.bundle_id, "--prepare-only"], env)
        print("[sky_setup] Device project ready. Run ./setup-skyrim.sh --deploy-ios to sign, install, verify RomFS, and launch.")
    else:
        run(["/usr/bin/make", "-C", REPO / "skyhost", "-j1"], env)
        print("[sky_setup] Host built. Run SKY_WORK_DIR=" + shlex.quote(str(work)) + " "
              + shlex.quote(str(REPO / "skyhost/run.sh")), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", nargs="?", type=Path,
                        help="Your XCI file or split-dump directory (defaults to one dump in inputs/, otherwise prompted)")
    parser.add_argument("--base", type=Path, help="Alternative flag for the XCI file or split dump")
    parser.add_argument("--keys", type=Path,
                        help="Your console's prod.keys (defaults to inputs/prod.keys, otherwise prompted; never copied by setup)")
    parser.add_argument("--work-dir", type=Path, default=REPO / "skyrim-work")
    parser.add_argument("--ios", action="store_true", help="Prepare the Release device project instead of the macOS host")
    parser.add_argument("--bundle-id", help="iOS bundle ID (project default: dev.skyhost.skyrim; deployment default includes team ID)")
    parser.add_argument("--team", help="Apple development team ID for --deploy-ios")
    parser.add_argument("--device", help="Physical device name or UDID for --deploy-ios")
    parser.add_argument("--romfs", type=Path, help="Existing extracted RomFS override for --deploy-ios")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="Read-only validation of tools and prepared workspace")
    mode.add_argument("--tools-only", action="store_true", help="Install pinned tools without game inputs")
    mode.add_argument("--deploy-ios", action="store_true", help="Sign/install the prepared iOS app, verify RomFS on the device, and launch")
    args = parser.parse_args(argv)
    if args.dump is not None and args.base is not None:
        parser.error("Use either the positional dump path or --base, not both.")
    if args.deploy_ios and (args.dump is not None or args.base is not None or args.keys is not None):
        parser.error("--deploy-ios uses the prepared workspace; run setup with your dump and keys first.")
    if not args.deploy_ios and any(value is not None for value in (args.team, args.device, args.romfs)):
        parser.error("--team, --device and --romfs apply only to --deploy-ios.")
    args.base = args.dump if args.dump is not None else args.base
    args.work_dir = args.work_dir.expanduser().resolve()
    for name in ("base", "keys", "romfs"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).expanduser().resolve())
    try:
        if args.deploy_ios:
            env = tool_environment(args.work_dir)
            command = [sys.executable, REPO / "tools/sky_deploy_ios.py", "--work-dir", args.work_dir]
            for flag, value in (("--team", args.team), ("--device", args.device),
                                ("--bundle-id", args.bundle_id), ("--romfs", args.romfs)):
                if value is not None:
                    command.extend((flag, value))
            run(command, env)
            return 0
        if args.check:
            problems = workspace_problems(args.work_dir.resolve(), args.ios)
            for problem in problems:
                print("[sky_setup] " + problem, file=sys.stderr)
            return int(bool(problems))
        if args.tools_only:
            validate_workspace(args.base, args.work_dir)
            ensure_tools(args.work_dir, args.ios)
        else:
            args.bundle_id = args.bundle_id or "dev.skyhost.skyrim"
            if args.base is None:
                args.base = default_dump_path()
            args.base = input_path(args.base, "the dump path or --base PATH (or place one dump in inputs/)",
                                   "XCI file or split-dump directory",
                                   lambda path: path.is_file() or path.is_dir())
            validate_workspace(args.base, args.work_dir)
            if args.keys is None and (REPO / "inputs/prod.keys").is_file():
                args.keys = REPO / "inputs/prod.keys"
            args.keys = input_path(args.keys, "--keys PATH (or place prod.keys in inputs/)",
                                   "Console prod.keys file", Path.is_file)
            prepare(args)
    except KeyboardInterrupt:
        print("\n[sky_setup] Setup cancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("[sky_setup] " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
