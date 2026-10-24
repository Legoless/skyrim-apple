#!/usr/bin/env python3
"""Rebuild the recorded Skyrim shader cache from owned game files, byte for byte."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from sky_tools import tool_environment
from sky_shader_cache import REFERENCE, fingerprint

from shader_archive import extract_archive
from shader_embedded import extract_embedded
from shaders import load_capture

INPUT_KEYS_SHA256 = "8c2d8ca133dc438d426a7909cc4aad108cd4ab9849eb40ed09b4f9b788fd31a5"


def merge_captures(directories, output):
    output.mkdir()
    keys = set()
    for directory in directories:
        for path in sorted(directory.glob("*.json")):
            capture = load_capture(path)
            if capture is None:
                raise ValueError("Extracted input contains an unsupported shader capture")
            metadata, code = capture
            key = metadata["shader_key"]
            if key in keys:
                continue
            metadata["code_file"] = key + ".bin"
            (output / metadata["code_file"]).write_bytes(code)
            (output / (key + ".json")).write_text(json.dumps(metadata) + "\n")
            keys.add(key)
    return keys


def rebuild(archive, main_module, output, work_dir, jobs=8, expected=REFERENCE,
            ensure=False, expected_keys_sha256=INPUT_KEYS_SHA256):
    archive, main_module = archive.resolve(), main_module.resolve()
    output = output.absolute()
    existing = os.path.lexists(output)
    if existing and not ensure:
        raise ValueError("Output already exists; choose a new directory to preserve the existing cache")
    if existing and (output.is_symlink() or not output.is_dir()):
        raise ValueError("Existing shader cache must be a regular directory")
    empty = existing and not any(output.iterdir())
    for path in (archive, main_module):
        if not path.is_file():
            raise ValueError(f"Game input is missing: {path}")
    if jobs < 1:
        raise ValueError("--jobs must be at least 1")
    env = tool_environment(work_dir)
    if not Path(env["SKY_PYTHON"]).is_file():
        raise ValueError("Local tools are missing; run ./setup-skyrim.sh --tools-only first")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".shader-rebuild-", dir=output.parent))
    try:
        archive_captures, embedded_captures = temporary / "archive", temporary / "embedded"
        extract_archive(archive, archive_captures)
        extract_embedded(main_module, embedded_captures)
        captures = temporary / "captures"
        keys = merge_captures([archive_captures, embedded_captures], captures)
        if len(keys) != expected["pairs"]:
            raise ValueError(f"Game inputs contain {len(keys)} unique shaders; "
                             f"the recorded v327680 cache requires {expected['pairs']}. "
                             "This game build does not match the supported shader reference")
        input_digest = hashlib.sha256("".join(key + "\n" for key in sorted(keys)).encode("ascii")).hexdigest()
        if input_digest != expected_keys_sha256:
            raise ValueError("This game's shader inputs do not match the recorded v327680 reference; "
                             "base-cartridge compatibility remains unverified")
        if existing and not empty:
            actual = fingerprint(output)
            if actual != expected:
                raise ValueError("Existing shader cache differs from the reference and was preserved. "
                                 "Move it to a backup directory and rerun setup")
            shutil.rmtree(temporary)
            print(f"SHADER_REBUILD reused={actual['files']} sha256={actual['sha256']} output={output}", flush=True)
            return actual
        cache = temporary / "cache"
        print(f"SHADER_REBUILD inputs={len(keys)} reference={expected['pairs']}", flush=True)
        subprocess.run([env["SKY_PYTHON"], str(ROOT / "skyhost/tools/shaders.py"),
                        str(captures), str(cache), "--jobs", str(jobs)],
                       env=env, cwd=ROOT, check=True)
        actual = fingerprint(cache)
        if actual != expected:
            raise ValueError(f"Rebuilt cache differs from the reference: {actual}; expected {expected}")
        (cache / "rebuild-verification.json").write_text(json.dumps({
            "reference": expected,
            "actual": actual,
            "verified": True,
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "module_sha256": hashlib.sha256(main_module.read_bytes()).hexdigest(),
        }, indent=2) + "\n")
        if empty:
            # rmdir can only remove the previously empty directory, never files.
            # If another writer filled or replaced it, preserve it and fail.
            if output.is_symlink():
                raise ValueError("Output changed during rebuilding; preserving it without replacement")
            output.rmdir()
        elif os.path.lexists(output):
            raise ValueError("Output was created during rebuilding; preserving it without replacement")
        cache.rename(output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise ValueError(f"{error}. Rebuild artifacts retained at {temporary}") from error
    shutil.rmtree(temporary)
    print(f"SHADER_REBUILD verified={actual['files']} sha256={actual['sha256']} output={output}", flush=True)
    return actual


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path,
                        help="Extracted RomFS/Data/Skyrim - Shaders.bsa")
    parser.add_argument("--main", required=True, type=Path,
                        help="Extracted ExeFS/main NSO, or an existing flat main.patched.bin")
    parser.add_argument("--output", type=Path, default=ROOT / "skyrim-work/build/shaders",
                        help="New private cache directory; an existing directory is never replaced")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "skyrim-work",
                        help="Workspace containing the pinned local tools")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--ensure", action="store_true",
                        help="Verify and reuse a matching cache, or create one if absent/empty")
    args = parser.parse_args(argv)
    try:
        rebuild(args.archive, args.main, args.output, args.work_dir, args.jobs, ensure=args.ensure)
    except (OSError, ValueError) as error:
        print(f"SHADER_REBUILD_ERROR {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
