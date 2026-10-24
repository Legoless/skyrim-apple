#!/usr/bin/env python3
"""Tool provisioning tests use tiny local fixtures rather than network downloads."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location("sky_tools", Path(__file__).resolve().parents[1] / "sky_tools.py")
sky_tools = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sky_tools)


class SkyToolsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name) / "work with ' spaces"

    def spec(self, data=b"verified archive", algorithm="sha256"):
        return {"url": "https://example.invalid/pinned.tar.gz",
                algorithm: hashlib.new(algorithm, data).hexdigest()}

    def download_run(self, data):
        def run(command, **kwargs):
            self.assertEqual(command[0], "/usr/bin/curl")
            Path(command[command.index("--output") + 1]).write_bytes(data)
            return subprocess.CompletedProcess(command, 0)
        return run

    def install_fixture(self, name, binary):
        paths = sky_tools._paths(self.work)
        target = paths[name]
        (target / binary).parent.mkdir(parents=True, exist_ok=True)
        (target / binary).write_text("#!/bin/sh\nexit 0\n")
        (target / binary).chmod(0o755)
        (target / ".sky-tool.json").write_text(sky_tools._stamp(name))

    def test_environment_prefers_pinned_tools_without_writing(self):
        with patch.dict(os.environ, {"PATH": "/ambient/bin", "SECRET": "private-value",
                                   "NUGET_PACKAGES": "/ambient/repacked/packages",
                                   "NUGET_HTTP_CACHE_PATH": "/ambient/http-cache",
                                   "DOTNET_CLI_HOME": "/ambient/dotnet-home",
                                   "RestoreConfigFile": "/ambient/NuGet.Config",
                                   "RestorePackagesPath": "/ambient/other/packages",
                                   "RestoreSources": "https://private.invalid/index.json",
                                   "RestoreAdditionalProjectSources": "https://private.invalid/extra",
                                   "RestoreFallbackFolders": "/ambient/fallback",
                                   "RestoreAdditionalProjectFallbackFolders": "/ambient/extra/fallback",
                                   "NUGET_FALLBACK_PACKAGES": "/ambient/repacked/packages"}):
            env = sky_tools.tool_environment(self.work)
        paths = sky_tools._paths(self.work)
        self.assertEqual(env["PATH"].split(os.pathsep)[0], str(paths["python"] / "bin"))
        self.assertEqual(env["DOTNET_ROOT"], str(paths["dotnet"]))
        self.assertEqual(env["DOTNET_CLI_HOME"], str(paths["root"] / "dotnet-home"))
        self.assertEqual(env["NUGET_PACKAGES"], str(paths["root"] / "nuget/packages"))
        self.assertEqual(env["NUGET_HTTP_CACHE_PATH"], str(paths["root"] / "nuget/http-cache"))
        self.assertEqual(env["RestoreConfigFile"], str(sky_tools.HERE.parent / "NuGet.Config"))
        self.assertEqual(env["RestorePackagesPath"], env["NUGET_PACKAGES"])
        self.assertEqual(env["RestoreSources"], "https://api.nuget.org/v3/index.json")
        for name in ("RestoreAdditionalProjectSources", "RestoreFallbackFolders",
                     "RestoreAdditionalProjectFallbackFolders", "NUGET_FALLBACK_PACKAGES"):
            self.assertEqual(env[name], "")
        self.assertEqual(env["HACTOOL"], str(paths["hactool"] / "hactool"))
        self.assertEqual(env["SKY_PYTHON"], str(paths["python"] / "bin" / "python3"))
        self.assertIn(str(paths["spirv-cross"]), env["PATH"].split(os.pathsep))
        self.assertLess(env["PATH"].split(os.pathsep).index(str(paths["spirv-cross"])),
                        env["PATH"].split(os.pathsep).index("/ambient/bin"))
        self.assertFalse(self.work.exists())

    def test_shell_exports_are_quoted_and_only_requested_variables(self):
        output = subprocess.check_output([
            os.sys.executable, str(sky_tools.HERE / "sky_tools.py"), "--env", "--work-dir", str(self.work)
        ], text=True, env=dict(os.environ, PRIVATE_SECRET="do-not-print"))
        exports = {}
        for line in output.splitlines():
            prefix, assignment = shlex.split(line)
            self.assertEqual(prefix, "export")
            name, value = assignment.split("=", 1)
            exports[name] = value
        self.assertEqual(set(exports), {"PATH", "DOTNET_ROOT", "DOTNET_CLI_HOME", "NUGET_PACKAGES",
                                        "NUGET_HTTP_CACHE_PATH", "RestoreConfigFile", "RestorePackagesPath", "RestoreSources",
                                        "RestoreAdditionalProjectSources", "RestoreFallbackFolders",
                                        "RestoreAdditionalProjectFallbackFolders", "NUGET_FALLBACK_PACKAGES", "HACTOOL", "SKY_PYTHON"})
        for name, value in exports.items():
            self.assertEqual(value, sky_tools.tool_environment(self.work)[name])
        self.assertNotIn("do-not-print", output)
        self.assertFalse(self.work.exists())

    def test_preflight_is_read_only_and_reports_missing_local_tools(self):
        with patch.object(sky_tools, "_prerequisites", return_value=[]), \
             patch.object(sky_tools, "_download", side_effect=AssertionError("network used")):
            problems = sky_tools.check_tools(self.work, ios=True)
        self.assertEqual(len(problems), 5)
        self.assertFalse(self.work.exists())

    def test_preflight_optional_ios_and_changed_lock(self):
        for name, binary in [("dotnet", "dotnet"), ("hactool", "hactool"), ("spirv-cross", "spirv-cross"), ("python", "bin/python3")]:
            self.install_fixture(name, binary)
        with patch.object(sky_tools, "_prerequisites", side_effect=lambda ios: []):
            self.assertEqual(sky_tools.check_tools(self.work), [])
            self.assertEqual(len(sky_tools.check_tools(self.work, ios=True)), 1)
            (sky_tools._paths(self.work)["hactool"] / ".sky-tool.json").write_text("stale pin")
            self.assertEqual(len(sky_tools.check_tools(self.work)), 1)

    def test_download_verifies_both_hash_algorithms_and_reuses_cache(self):
        for algorithm in ("sha256", "sha512"):
            with self.subTest(algorithm=algorithm):
                data = b"verified archive"
                with patch.object(sky_tools, "_run", side_effect=self.download_run(data)) as run:
                    target = sky_tools._download(self.spec(data, algorithm), self.work / algorithm)
                    self.assertEqual(target.read_bytes(), data)
                    self.assertEqual(sky_tools._download(self.spec(data, algorithm), self.work / algorithm), target)
                    self.assertEqual(run.call_count, 1)

    def test_corrupt_cache_is_replaced_only_with_verified_download(self):
        spec = self.spec()
        with patch.object(sky_tools, "_run", side_effect=self.download_run(b"verified archive")):
            target = sky_tools._download(spec, self.work)
            target.write_bytes(b"corrupt cache")
            self.assertEqual(sky_tools._download(spec, self.work).read_bytes(), b"verified archive")
        self.assertEqual(list(self.work.iterdir()), [target])

    def test_checksum_failure_does_not_publish_or_leave_partial_file(self):
        with patch.object(sky_tools, "_run", side_effect=self.download_run(b"wrong archive")):
            with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
                sky_tools._download(self.spec(), self.work)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_network_failure_does_not_publish_or_leave_partial_file(self):
        def fail(command, **kwargs):
            self.download_run(b"partial")(command)
            raise subprocess.CalledProcessError(22, command)
        with patch.object(sky_tools, "_run", side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                sky_tools._download(self.spec(), self.work)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_rejects_archive_traversal_and_external_symlink(self):
        destination = Path(self.temporary.name) / "out"
        destination.mkdir()
        for name, link in [("../outside", None), ("safe", "../../outside")]:
            archive = Path(self.temporary.name) / "unsafe.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                entry = tarfile.TarInfo(name)
                if link:
                    entry.type, entry.linkname = tarfile.SYMTYPE, link
                stream.addfile(entry)
            with self.assertRaisesRegex(RuntimeError, "Unsafe"):
                sky_tools._unpack(archive, destination)
        archive = Path(self.temporary.name) / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("../outside", b"bad")
        with self.assertRaisesRegex(RuntimeError, "Unsafe"):
            sky_tools._unpack(archive, destination, zipped=True)
        self.assertEqual(list(destination.iterdir()), [])

    def test_hactool_builds_bundled_source_with_apple_clang_and_stamps_install(self):
        archive = Path(self.temporary.name) / "hactool.tar.gz"
        prefix = "hactool-" + sky_tools.LOCK["hactool"]["version"]
        with tarfile.open(archive, "w:gz") as stream:
            contents = b"CC = gcc\nLDFLAGS = -lmbedtls\n"
            info = tarfile.TarInfo(prefix + "/config.mk.template")
            info.size = len(contents)
            stream.addfile(info, io.BytesIO(contents))
        paths = sky_tools._paths(self.work)
        paths["root"].mkdir(parents=True)
        def build(command, cwd, **kwargs):
            self.assertEqual(command[0], "/usr/bin/make")
            self.assertIn("CC=/usr/bin/clang", command)
            binary = cwd / "hactool"
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o755)
        with patch.object(sky_tools, "_download", return_value=archive), \
             patch.object(sky_tools, "_run", side_effect=build) as run:
            sky_tools._install_archive("hactool", paths)
            sky_tools._install_archive("hactool", paths)
            self.assertEqual(run.call_count, 1)
        self.assertTrue(sky_tools._installed(paths["hactool"], "hactool", "hactool"))

    def test_spirv_cross_builds_with_apple_compiler_and_archiver(self):
        archive = Path(self.temporary.name) / "spirv-cross.tar.gz"
        prefix = "SPIRV-Cross-vulkan-sdk-" + sky_tools.LOCK["spirv-cross"]["version"]
        with tarfile.open(archive, "w:gz") as stream:
            contents = b"all: spirv-cross\n"
            info = tarfile.TarInfo(prefix + "/Makefile")
            info.size = len(contents)
            stream.addfile(info, io.BytesIO(contents))
        paths = sky_tools._paths(self.work)
        paths["root"].mkdir(parents=True)
        def build(command, cwd, **kwargs):
            self.assertEqual(command[0], "/usr/bin/make")
            self.assertIn("CXX=/usr/bin/clang++", command)
            self.assertIn("AR=/usr/bin/ar", command)
            binary = cwd / "spirv-cross"
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o755)
        with patch.object(sky_tools, "_download", return_value=archive), \
             patch.object(sky_tools, "_run", side_effect=build) as run:
            sky_tools._install_archive("spirv-cross", paths)
            sky_tools._install_archive("spirv-cross", paths)
            self.assertEqual(run.call_count, 1)
        self.assertTrue(sky_tools._installed(paths["spirv-cross"], "spirv-cross", "spirv-cross"))

    def test_default_and_ios_bootstrap_include_shader_translator(self):
        for ios in (False, True):
            with self.subTest(ios=ios):
                installed = []
                with patch.object(sky_tools, "_prerequisites", return_value=[]), \
                     patch.object(sky_tools, "_install_archive", side_effect=lambda name, paths: installed.append(name)), \
                     patch.object(sky_tools, "_install_python", side_effect=lambda paths: installed.append("python")):
                    env = sky_tools.ensure_tools(self.work, ios)
                self.assertEqual(installed, ["dotnet", "hactool", "spirv-cross"] + (["xcodegen"] if ios else []) + ["python"])
                self.assertIn(str(sky_tools._paths(self.work)["spirv-cross"]), env["PATH"].split(os.pathsep))

    def test_failed_python_install_is_removed(self):
        paths = sky_tools._paths(self.work)
        paths["root"].mkdir(parents=True)
        def fail(command, **kwargs):
            paths["python"].mkdir()
            raise subprocess.CalledProcessError(1, command)
        with patch.object(sky_tools, "_run", side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                sky_tools._install_python(paths)
        self.assertFalse(paths["python"].exists())

    def test_ensure_rejects_prerequisite_failure_without_writes_or_network(self):
        with patch.object(sky_tools, "_prerequisites", return_value=["Xcode missing"]), \
             patch.object(sky_tools, "_download", side_effect=AssertionError("network used")):
            with self.assertRaisesRegex(RuntimeError, "Xcode missing"):
                sky_tools.ensure_tools(self.work)
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()
