#!/usr/bin/env python3
"""Prepare private game inputs and build the native Skyrim iOS app.

Generated projects, embedded guest code, shaders, and products stay in the
ignored skyhost/build tree. RomFS is never copied into the app by this helper.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[1]
REPO = SOURCE.parent
BUILD = SOURCE / "build/ios"
MODULES = ("main", "sdk")
sys.path.insert(0, str(REPO / "tools"))
from sky_tools import tool_environment


def run(command, **kwargs):
    print("+ " + shlex.join(str(part) for part in command), flush=True)
    kwargs.setdefault("cwd", REPO)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def write_if_changed(path, content):
    if not path.is_file() or path.read_text() != content:
        path.write_text(content)


def copy_if_changed(source, destination):
    if destination.is_file():
        before, after = source.stat(), destination.stat()
        if before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns:
            return
    shutil.copy2(source, destination)


def synchronize_files(destination, sources):
    destination.mkdir(parents=True, exist_ok=True)
    wanted = set()
    for source in sources:
        wanted.add(source.name)
        copy_if_changed(source, destination / source.name)
    # Only our generated flat resource folder is pruned; source data is read-only.
    for old in destination.iterdir():
        if old.is_file() and old.name not in wanted:
            old.unlink()


def prepare(rewrite, shaders, environment=None):
    generated = BUILD / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    sizes, metadata, images = {}, [], []
    for module in MODULES:
        image = rewrite / f"{module}.patched.bin"
        if not image.is_file() or image.stat().st_size <= 0:
            raise ValueError(f"Missing rewritten game module: {image}")
        sizes[module] = image.stat().st_size
        images.append(image)
        for suffix in ("fixups.tsv", "gates.tsv"):
            path = rewrite / f"{module}.{suffix}"
            if not path.is_file():
                raise ValueError(f"Missing rewritten module metadata: {path}")
            metadata.append(path)
    header = "#pragma once\n\n/* Generated from the unpadded rewritten image file sizes. */\n"
    header += "\n".join(f"#define SKY_IOS_{name.upper()}_FILE_SIZE {size}ull" for name, size in sizes.items()) + "\n"
    write_if_changed(generated / "sky_ios_embed_sizes.h", header)
    next_assembly = generated / "GuestImages.next.S"
    run([sys.executable, SOURCE / "ios/tools/gen_embed.py", rewrite,
         next_assembly, *MODULES], env=environment)
    assembly = generated / "GuestImages.S"
    if assembly.is_file() and assembly.read_bytes() == next_assembly.read_bytes():
        next_assembly.unlink()
    else:
        next_assembly.replace(assembly)
    synchronize_files(BUILD / "resources/rewrite", metadata)
    pairs = []
    for metal in sorted(shaders.glob("*.metal")):
        if not re.fullmatch(r"[0-9a-f]{64}\.metal", metal.name):
            continue
        reflection = metal.with_suffix(".json")
        if not reflection.is_file():
            raise ValueError(f"Missing translated shader reflection: {reflection}")
        pairs.extend((metal, reflection))
    if not pairs:
        raise ValueError(f"No translated shader source/reflection pairs in {shaders}")
    synchronize_files(BUILD / "resources/shaders", pairs)
    manifest = {
        "modules": {image.name: {"bytes": image.stat().st_size,
            "sha256": hashlib.sha256(image.read_bytes()).hexdigest()} for image in images},
        "shader_pairs": len(pairs) // 2,
        "romfs_bundled": False,
    }
    write_if_changed(generated / "inputs.json", json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared {len(images)} signed guest images and {len(pairs) // 2} translated shader pairs; RomFS remains external.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", choices=("simulator", "device"), default="simulator")
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--work-dir", type=Path, default=Path(os.environ.get("SKY_WORK_DIR", REPO / "skyrim-work")),
                        help="Workspace containing tools installed by setup-skyrim.sh")
    parser.add_argument("--rewrite-dir", type=Path, default=None)
    parser.add_argument("--shader-cache", type=Path, default=Path(os.environ.get("SKY_SHADER_CACHE_DIR", SOURCE.parent / "skyrim-work/build/shaders")))
    parser.add_argument("--bundle-id", default="dev.skyhost.skyrim")
    parser.add_argument("--prepare-only", action="store_true", help="Generate private resources/project without invoking xcodebuild")
    parser.add_argument("--reuse-audio", action="store_true", help="Use an existing real DSP archive/link response instead of republishing it")
    parser.add_argument("--test", action="store_true", help="Execute all UIKit controls XCTest cases; requires an explicit Simulator destination")
    parser.add_argument("--derived-data", type=Path, default=BUILD / "DerivedData", help="Separate Xcode output folder, useful for concurrent device/Simulator builds")
    parser.add_argument("--destination", help="xcodebuild destination, e.g. platform=iOS Simulator,id=<owned-test-device-UUID>")
    args = parser.parse_args()
    if args.test and (args.sdk != "simulator" or not args.destination):
        parser.error("--test requires --sdk simulator and an explicit --destination")
    work = args.work_dir.expanduser().resolve()
    environment = tool_environment(work)
    environment["SKY_WORK_DIR"] = str(work)
    xcodegen = shutil.which("xcodegen", path=environment["PATH"])
    if not xcodegen:
        parser.error("xcodegen is missing; run " + shlex.quote(str(REPO / "setup-skyrim.sh"))
                     + " --tools-only --ios --work-dir " + shlex.quote(str(work)))
    rewrite = (args.rewrite_dir or Path(os.environ.get("SKY_REWRITE_DIR", work / "build/rewrite-native"))).expanduser().resolve()
    shaders = args.shader_cache.expanduser().resolve()
    prepare(rewrite, shaders, environment)
    rid = "iossimulator-arm64" if args.sdk == "simulator" else "ios-arm64"
    response = SOURCE / "build/audio-renderer-ios" / rid / "SkyAudioRenderer.link.rsp"
    if not args.reuse_audio:
        run([sys.executable, SOURCE / "audio_renderer/build_ios.py", rid, "--work-dir", work], env=environment)
    if not response.is_file() or not (response.parent / "SkyAudioRenderer.a").is_file():
        parser.error(f"The real iOS DSP link inputs are missing: {response}; run without --reuse-audio")
    environment.update(SKYHOST_SOURCE_DIR=str(SOURCE), SKY_IOS_BUILD_DIR=str(BUILD), SKY_IOS_BUNDLE_ID=args.bundle_id)
    run([xcodegen, "generate", "--spec", SOURCE / "ios/project.yml", "--project", BUILD, "--project-root", BUILD], env=environment)
    if args.prepare_only:
        print(BUILD / "SkyrimIOS.xcodeproj")
        return
    sdk = "iphonesimulator" if args.sdk == "simulator" else "iphoneos"
    derived_data = args.derived_data.expanduser().resolve()
    command = ["xcodebuild", "-project", BUILD / "SkyrimIOS.xcodeproj", "-scheme", "SkyrimIOS",
        "-configuration", args.configuration, "-sdk", sdk, "-derivedDataPath", derived_data,
        "CODE_SIGNING_ALLOWED=NO"]
    if args.destination:
        command.extend(("-destination", args.destination))
    elif args.sdk == "device":
        command.extend(("-destination", "generic/platform=iOS"))
    else:
        command.extend(("-destination", "generic/platform=iOS Simulator"))
    if args.test:
        command.extend(("-collect-test-diagnostics", "never"))
    command.append("test" if args.test else "build")
    log = BUILD / f"{args.sdk}-{'tests' if args.test else 'build'}.log"
    print("+ " + shlex.join(str(part) for part in command), flush=True)
    print(f"Build log: {log}", flush=True)
    with log.open("w") as output:
        completed = subprocess.run([str(part) for part in command], stdout=output, stderr=subprocess.STDOUT,
                                   cwd=REPO, env=environment)
    if completed.returncode:
        lines = log.read_text(errors="replace").splitlines()
        selected = set()
        for index, line in enumerate(lines):
            if "error:" in line or "Undefined symbols" in line or line.startswith("ld:") or "symbol(s) not found" in line:
                selected.update(range(max(0, index - 1), min(len(lines), index + 5)))
        if not selected:
            selected.update(range(max(0, len(lines) - 25), len(lines)))
        print("\n".join(lines[index][:1200] for index in sorted(selected)), file=sys.stderr)
        raise subprocess.CalledProcessError(completed.returncode, command)
    product_sdk = "iphonesimulator" if args.sdk == "simulator" else "iphoneos"
    print(derived_data / "Build/Products" / f"{args.configuration}-{product_sdk}" / "SkyrimIOS.app")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"iOS build failed: {error}", file=sys.stderr)
        sys.exit(1)
