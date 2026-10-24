#!/usr/bin/env python3
"""Publish the real DSP plus its NativeAOT link inputs for a native iOS app.

The resulting SkyAudioRenderer.link.rsp is a clang response file. It links the
managed archive and the NativeAOT bootstrapper/runtime/shims directly into the
app; no dynamically loaded macOS renderer or .NET iOS workload is required.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from sky_tools import tool_environment


def write_link_response(output):
    manifest = output / "native-link-inputs"
    library = output / "SkyAudioRenderer.a"
    if not library.is_file():
        raise RuntimeError(f"NativeAOT static archive was not produced: {library}")
    native_dir = output / "native"
    native_dir.mkdir(exist_ok=True)
    replacements = {}
    destination_names = set()
    for line in manifest.with_suffix(".libraries").read_text(encoding="utf-8-sig").splitlines():
        source = Path(line)
        if not source.is_file():
            raise RuntimeError(f"NativeAOT native dependency is missing: {source}")
        destination = native_dir / source.name
        if source.name in destination_names:
            raise RuntimeError(f"NativeAOT native dependency names collide: {source.name}")
        shutil.copy2(source, destination)
        replacements[line] = str(destination)
        destination_names.add(source.name)

    arguments = [str(library)]
    for line in manifest.with_suffix(".arguments").read_text(encoding="utf-8-sig").splitlines():
        if line in replacements:
            arguments.append(replacements[line])
        else:
            arguments.extend(shlex.split(line))
    response = output / "SkyAudioRenderer.link.rsp"
    # Clang response files accept shell-style quoting; every token is separate.
    response.write_text("\n".join(shlex.quote(arg) for arg in arguments) + "\n")
    return response


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rid", choices=("ios-arm64", "iossimulator-arm64"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--work-dir", type=Path,
                        default=Path(os.environ.get("SKY_WORK_DIR", REPO / "skyrim-work")))
    parser.add_argument("--minimum-version", default="17.0")
    args = parser.parse_args(argv)
    project_dir = REPO / "skyhost/audio_renderer"
    work = args.work_dir.expanduser().resolve()
    output = (args.output or project_dir.parent / "build/audio-renderer-ios" / args.rid).expanduser().resolve()
    environment = tool_environment(work)
    environment["SKY_WORK_DIR"] = str(work)
    dotnet = shutil.which("dotnet", path=environment["PATH"])
    if not dotnet:
        command = shlex.join([str(REPO / "setup-skyrim.sh"), "--tools-only", "--ios", "--work-dir", str(work)])
        parser.error("The .NET SDK is missing. Run " + command)
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "native-link-inputs"
    subprocess.run([
        dotnet, "publish", str(project_dir / "SkyAudioRenderer.csproj"),
        "-c", "Release", "-r", args.rid, "-o", str(output), "-v", "minimal",
        f"-p:AppleMinOSVersion={args.minimum_version}",
        f"-p:SkyNativeLinkManifest={manifest}",
    ], check=True, cwd=REPO, env=environment)
    response = write_link_response(output)
    print(f"Native iOS DSP link response: {response}")


if __name__ == "__main__":
    main()
