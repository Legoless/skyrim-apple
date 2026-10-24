"""Packaging regressions for the native consumer of the iOS DSP archive."""

import importlib.util
import contextlib
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


HELPER = Path(__file__).resolve().parents[1] / "build_ios.py"
SPEC = importlib.util.spec_from_file_location("build_ios", HELPER)
BUILD_IOS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_IOS)


class NativeLinkPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sky audio 'pack ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "published DSP"
        self.output.mkdir()
        (self.output / "SkyAudioRenderer.a").write_bytes(b"managed archive")

    def manifest(self, libraries, arguments):
        (self.output / "native-link-inputs.libraries").write_text("\n".join(map(str, libraries)) + "\n")
        (self.output / "native-link-inputs.arguments").write_text("\n".join(arguments) + "\n")

    def test_bootstrapper_and_archives_remain_independent_link_inputs(self):
        runtime = self.root / "runtime package"
        runtime.mkdir()
        bootstrapper = runtime / "libbootstrapperdll.o"
        archive = runtime / "libRuntime.WorkstationGC.a"
        bootstrapper.write_bytes(b"bootstrap constructor")
        archive.write_bytes(b"runtime")
        self.manifest([bootstrapper, archive], [str(bootstrapper), str(archive),
                      '-isysroot "/sdk with spaces"', "-framework Foundation", "-lobjc"])
        response = BUILD_IOS.write_link_response(self.output)
        arguments = shlex.split(response.read_text())
        self.assertEqual(arguments, [str(self.output / "SkyAudioRenderer.a"),
            str(self.output / "native/libbootstrapperdll.o"),
            str(self.output / "native/libRuntime.WorkstationGC.a"),
            "-isysroot", "/sdk with spaces", "-framework", "Foundation", "-lobjc"])
        self.assertEqual((self.output / "native/libbootstrapperdll.o").read_bytes(), b"bootstrap constructor")

    def test_conflicting_native_filenames_fail_instead_of_overwriting_runtime(self):
        libraries = []
        for directory in ("runtime-one", "runtime-two"):
            source_dir = self.root / directory
            source_dir.mkdir()
            source = source_dir / "same.a"
            source.write_bytes(directory.encode())
            libraries.append(source)
        self.manifest(libraries, list(map(str, libraries)))
        with self.assertRaisesRegex(RuntimeError, "names collide"):
            BUILD_IOS.write_link_response(self.output)

    def test_missing_archive_or_runtime_is_a_hard_failure(self):
        self.manifest([self.root / "missing.o"], [])
        with self.assertRaisesRegex(RuntimeError, "native dependency is missing"):
            BUILD_IOS.write_link_response(self.output)
        (self.output / "SkyAudioRenderer.a").unlink()
        with self.assertRaisesRegex(RuntimeError, "static archive was not produced"):
            BUILD_IOS.write_link_response(self.output)

    def test_non_ios_rid_is_rejected_before_publishing(self):
        result = subprocess.run([sys.executable, str(HELPER), "osx-arm64"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice", result.stderr)

    def test_publish_from_another_directory_uses_local_tools_and_absolute_output(self):
        repo = (self.root / "relocated checkout").resolve()
        caller = self.root / "foreign working directory"
        caller.mkdir()
        work = (self.root / "private workspace").resolve()
        output = (caller / "published audio").resolve()
        environment = BUILD_IOS.tool_environment(work)
        dotnet = str(Path(environment["DOTNET_ROOT"]) / "dotnet")

        def publish(command, **kwargs):
            self.assertEqual(command[0], dotnet)
            self.assertEqual(command[2], str(repo / "skyhost/audio_renderer/SkyAudioRenderer.csproj"))
            self.assertEqual(command[command.index("-o") + 1], str(output))
            self.assertEqual(kwargs["cwd"], repo)
            self.assertEqual(kwargs["env"]["SKY_WORK_DIR"], str(work))
            self.assertEqual(kwargs["env"]["DOTNET_ROOT"], environment["DOTNET_ROOT"])
            self.assertEqual(kwargs["env"]["PATH"].split(os.pathsep)[0],
                             str(work / "build/tools/python/bin"))
            (output / "SkyAudioRenderer.a").write_bytes(b"managed fixture")
            (output / "native-link-inputs.libraries").write_text("")
            (output / "native-link-inputs.arguments").write_text("-framework Foundation\n")
            return subprocess.CompletedProcess(command, 0)

        previous = Path.cwd()
        try:
            os.chdir(caller)
            with patch.object(BUILD_IOS, "REPO", repo), \
                 patch.object(BUILD_IOS.shutil, "which", return_value=dotnet) as locate, \
                 patch.object(BUILD_IOS.subprocess, "run", side_effect=publish) as invoked, \
                 contextlib.redirect_stdout(io.StringIO()):
                BUILD_IOS.main(["ios-arm64", "--work-dir", "../private workspace",
                                "--output", "published audio"])
                locate.assert_called_once_with("dotnet", path=environment["PATH"])
                invoked.assert_called_once()
        finally:
            os.chdir(previous)
        self.assertTrue((output / "SkyAudioRenderer.link.rsp").is_file())

    def test_workspace_defaults_to_environment_then_checkout(self):
        repo = (self.root / "relocated checkout").resolve()
        for configured in (None, str(self.root / "environment workspace")):
            with self.subTest(configured=configured):
                env = {"PATH": "/ambient/bin"}
                if configured:
                    env["SKY_WORK_DIR"] = configured
                expected = Path(configured).resolve() if configured else repo / "skyrim-work"
                with patch.dict(os.environ, env, clear=True), patch.object(BUILD_IOS, "REPO", repo), \
                     patch.object(BUILD_IOS, "tool_environment", return_value={"PATH": "/pinned/bin"}) as tools, \
                     patch.object(BUILD_IOS.shutil, "which", return_value="/pinned/bin/dotnet"), \
                     patch.object(BUILD_IOS.subprocess, "run") as invoked, \
                     patch.object(BUILD_IOS, "write_link_response", return_value=self.output / "link.rsp"), \
                     contextlib.redirect_stdout(io.StringIO()):
                    BUILD_IOS.main(["iossimulator-arm64", "--output", str(self.output)])
                    tools.assert_called_once_with(expected)
                    self.assertEqual(invoked.call_args.kwargs["env"]["SKY_WORK_DIR"], str(expected))

    def test_missing_dotnet_reports_local_install_command_without_publishing(self):
        work = self.root / "unprepared workspace"
        output = self.root / "must not be created"
        errors = io.StringIO()
        with patch.object(BUILD_IOS.shutil, "which", return_value=None), \
             patch.object(BUILD_IOS.subprocess, "run") as invoked, \
             contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
            BUILD_IOS.main(["ios-arm64", "--work-dir", str(work), "--output", str(output)])
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("The .NET SDK is missing", errors.getvalue())
        self.assertIn("--tools-only --ios --work-dir", errors.getvalue())
        install_command = shlex.split(errors.getvalue().split("Run ", 1)[1].strip())
        self.assertEqual(install_command[-1], str(work.resolve()))
        invoked.assert_not_called()
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
