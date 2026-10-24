#!/usr/bin/env python3
"""Install pinned build tools into the ignored work directory, without Homebrew."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / "sky-tools.lock.json").read_text())
REQUIREMENTS = HERE / "requirements-skyrim.txt"


def _paths(work_dir: Path) -> dict[str, Path]:
    root = work_dir.resolve() / "build" / "tools"
    return {
        "root": root,
        "python": root / "python",
        "dotnet": root / ("dotnet-" + LOCK["dotnet"]["version"]),
        "hactool": root / ("hactool-" + LOCK["hactool"]["version"]),
        "spirv-cross": root / ("spirv-cross-" + LOCK["spirv-cross"]["version"]),
        "xcodegen": root / ("xcodegen-" + LOCK["xcodegen"]["version"]),
    }


def tool_environment(work_dir: Path) -> dict[str, str]:
    """Return a child-process environment; never create files or download tools."""
    paths = _paths(work_dir)
    env = os.environ.copy()
    bins = [paths["python"] / "bin", paths["hactool"], paths["dotnet"], paths["spirv-cross"],
            paths["xcodegen"] / "bin"]
    env.update({
        "PATH": os.pathsep.join(map(str, bins)) + os.pathsep + env.get("PATH", "/usr/bin:/bin"),
        "DOTNET_ROOT": str(paths["dotnet"]),
        "DOTNET_CLI_HOME": str(paths["root"] / "dotnet-home"),
        "NUGET_PACKAGES": str(paths["root"] / "nuget/packages"),
        "NUGET_HTTP_CACHE_PATH": str(paths["root"] / "nuget/http-cache"),
        "RestorePackagesPath": str(paths["root"] / "nuget/packages"),
        "RestoreSources": "https://api.nuget.org/v3/index.json",
        "RestoreAdditionalProjectSources": "",
        "RestoreFallbackFolders": "",
        "RestoreAdditionalProjectFallbackFolders": "",
        "NUGET_FALLBACK_PACKAGES": "",
        "RestoreConfigFile": str(HERE.parent / "NuGet.Config"),
        "HACTOOL": str(paths["hactool"] / "hactool"),
        "SKY_PYTHON": str(paths["python"] / "bin" / "python3"),
        "DOTNET_MULTILEVEL_LOOKUP": "0",
        "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
        "DOTNET_SKIP_FIRST_TIME_EXPERIENCE": "1",
        "DOTNET_NOLOGO": "1",
    })
    return env


def _run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=True, **kwargs)


def _prerequisites(ios: bool) -> list[str]:
    problems = []
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        problems.append("An Apple Silicon Mac running macOS is required.")
    if sys.version_info < (3, 9):
        problems.append("Python 3.9 or newer is required (Xcode supplies /usr/bin/python3).")
    checks = [["/usr/bin/xcrun", "--find", "clang"], ["/usr/bin/xcrun", "--find", "make"]]
    if ios:
        checks.append(["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"])
    for command in checks:
        try:
            _run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError):
            problems.append("Install and select Xcode, accept its license, and finish its first launch setup.")
            break
    return problems


def _stamp(name: str) -> str:
    spec = LOCK[name].copy()
    if name == "python":
        spec["requirements_sha256"] = hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()
    return json.dumps(spec, sort_keys=True)


def _installed(path: Path, name: str, binary: str) -> bool:
    try:
        return ((path / ".sky-tool.json").read_text() == _stamp(name)
                and os.access(path / binary, os.X_OK))
    except OSError:
        return False


def _python_ready(path: Path) -> bool:
    if not _installed(path, "python", "bin/python3"):
        return False
    script = ("import capstone, lz4.block; from importlib.metadata import version; "
              "assert " + repr(LOCK["python"]["packages"]) + " == "
              "{p: version(p) for p in ('lz4', 'capstone')}")
    try:
        _run([str(path / "bin" / "python3"), "-B", "-c", script],
             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def check_tools(work_dir: Path, ios: bool = False) -> list[str]:
    """Read-only preflight: report prerequisites and missing/incomplete local tools."""
    problems = _prerequisites(ios)
    paths = _paths(work_dir)
    for name, binary in [("dotnet", "dotnet"), ("hactool", "hactool"), ("spirv-cross", "spirv-cross")]:
        if not _installed(paths[name], name, binary):
            problems.append("Missing or incomplete pinned " + name + "; run ./setup-skyrim.sh --tools-only.")
    if not _python_ready(paths["python"]):
        problems.append("Missing or incomplete Python packages; run ./setup-skyrim.sh --tools-only.")
    if ios and not _installed(paths["xcodegen"], "xcodegen", "bin/xcodegen"):
        problems.append("Missing or incomplete pinned XcodeGen; run ./setup-skyrim.sh --tools-only --ios.")
    return problems


def _digest(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _download(spec: dict, cache: Path) -> Path:
    """Reuse verified archives; publish a new download only after its hash matches."""
    algorithm = "sha512" if "sha512" in spec else "sha256"
    expected = spec[algorithm]
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / (expected[:16] + "-" + spec["url"].rsplit("/", 1)[-1])
    if target.is_file():
        if _digest(target, algorithm) == expected:
            return target
        target.unlink()
    fd, name = tempfile.mkstemp(prefix=".download-", dir=cache)
    os.close(fd)
    temporary = Path(name)
    try:
        _run(["/usr/bin/curl", "--fail", "--location", "--retry", "2", "--connect-timeout", "20",
              "--max-time", "1200", "--output", str(temporary), spec["url"]])
        if _digest(temporary, algorithm) != expected:
            raise RuntimeError("Checksum mismatch for " + spec["url"])
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def _within(root: Path, path: Path) -> bool:
    return os.path.commonpath([root.resolve(), path.resolve()]) == str(root.resolve())


def _unpack(archive: Path, destination: Path, zipped: bool = False) -> None:
    if zipped:
        with zipfile.ZipFile(archive) as source:
            if any(not _within(destination, destination / item.filename) for item in source.infolist()):
                raise RuntimeError("Unsafe path in tool archive")
            source.extractall(destination)
    else:
        with tarfile.open(archive, "r:gz") as source:
            for item in source.getmembers():
                target = destination / item.name
                if not _within(destination, target):
                    raise RuntimeError("Unsafe path in tool archive")
                if item.issym() or item.islnk():
                    link = (target.parent if item.issym() else destination) / item.linkname
                    if not _within(destination, link):
                        raise RuntimeError("Unsafe link in tool archive")
                elif not (item.isfile() or item.isdir()):
                    raise RuntimeError("Unsupported entry in tool archive")
            source.extractall(destination)


def _install_archive(name: str, paths: dict[str, Path]) -> None:
    target = paths[name]
    binary = "bin/xcodegen" if name == "xcodegen" else name
    if _installed(target, name, binary):
        return
    print("Installing pinned " + name + " " + LOCK[name]["version"], flush=True)
    archive = _download(LOCK[name], paths["root"] / "downloads")
    with tempfile.TemporaryDirectory(prefix="." + name + "-", dir=paths["root"]) as temporary:
        staging = Path(temporary)
        _unpack(archive, staging, zipped=name == "xcodegen")
        source = staging
        if name == "xcodegen":
            source = staging / "xcodegen"
            (source / binary).chmod(0o755)
        elif name == "hactool":
            source = staging / ("hactool-" + LOCK[name]["version"])
            shutil.copyfile(source / "config.mk.template", source / "config.mk")
            # Clang ships with Xcode; mbedTLS ships inside the pinned hactool source.
            _run(["/usr/bin/make", "-j" + str(min(os.cpu_count() or 1, 8)), "CC=/usr/bin/clang"],
                 cwd=source)
        elif name == "spirv-cross":
            source = staging / ("SPIRV-Cross-vulkan-sdk-" + LOCK[name]["version"])
            _run(["/usr/bin/make", "-j" + str(min(os.cpu_count() or 1, 8)),
                  "CXX=/usr/bin/clang++", "AR=/usr/bin/ar"], cwd=source)
        if not os.access(source / binary, os.X_OK):
            raise RuntimeError("Tool archive did not produce " + binary)
        (source / ".sky-tool.json").write_text(_stamp(name))
        if target.exists():
            shutil.rmtree(target)
        source.rename(target)


def _install_python(paths: dict[str, Path]) -> None:
    target = paths["python"]
    if _python_ready(target):
        return
    print("Installing pinned Python packages", flush=True)
    # Virtualenv launchers embed their path, so create it at its final location.
    if target.exists():
        shutil.rmtree(target)
    try:
        _run([sys.executable, "-m", "venv", str(target)])
        _run([str(target / "bin" / "python3"), "-m", "pip", "--isolated", "install",
              "--disable-pip-version-check", "--only-binary=:all:", "--require-hashes",
              "--index-url", "https://pypi.org/simple", "-r", str(REQUIREMENTS)])
        (target / ".sky-tool.json").write_text(_stamp("python"))
        if not _python_ready(target):
            raise RuntimeError("Pinned Python packages could not be imported")
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        raise


def ensure_tools(work_dir: Path, ios: bool = False) -> dict[str, str]:
    """Fetch verified pinned tools and return the environment for subsequent builds."""
    problems = _prerequisites(ios)
    if problems:
        raise RuntimeError("\n".join(problems))
    paths = _paths(work_dir)
    paths["root"].mkdir(parents=True, exist_ok=True)
    for name in ["dotnet", "hactool", "spirv-cross"] + (["xcodegen"] if ios else []):
        _install_archive(name, paths)
    _install_python(paths)
    return tool_environment(work_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=HERE.parent / "skyrim-work")
    parser.add_argument("--ios", action="store_true")
    parser.add_argument("--env", action="store_true", help="Print shell exports without installing anything")
    parser.add_argument("--check", action="store_true", help="Check existing tools without installing anything")
    args = parser.parse_args()
    if args.env:
        env = tool_environment(args.work_dir)
        for name in ("PATH", "DOTNET_ROOT", "DOTNET_CLI_HOME", "NUGET_PACKAGES",
                     "NUGET_HTTP_CACHE_PATH", "RestoreConfigFile", "RestorePackagesPath", "RestoreSources",
                     "RestoreAdditionalProjectSources", "RestoreFallbackFolders",
                     "RestoreAdditionalProjectFallbackFolders", "NUGET_FALLBACK_PACKAGES", "HACTOOL", "SKY_PYTHON"):
            print("export " + name + "=" + shlex.quote(env[name]))
    elif args.check:
        problems = check_tools(args.work_dir, args.ios)
        for problem in problems:
            print(problem, file=sys.stderr)
        raise SystemExit(bool(problems))
    else:
        ensure_tools(args.work_dir, args.ios)


if __name__ == "__main__":
    main()
