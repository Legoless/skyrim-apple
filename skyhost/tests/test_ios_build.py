"""Portable iOS helper invocation without compiling private game inputs."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


HELPER = Path(__file__).resolve().parents[1] / "ios/build.py"
SPEC = importlib.util.spec_from_file_location("sky_ios_build_helper", HELPER)
BUILD_IOS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_IOS)


class BuildHelperPortabilityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sky ios helper ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "clone with spaces"
        self.source = self.repo / "skyhost"
        self.build = self.source / "build/ios"
        self.build.mkdir(parents=True)
        (self.repo / "global.json").write_text('{"sdk":{"version":"10.0.104"}}')
        self.foreign = self.root / "foreign current directory"
        self.foreign.mkdir()
        self.work = self.foreign / "custom workspace"
        env = BUILD_IOS.tool_environment(self.work)
        pinned_bin = next(Path(part) for part in env["PATH"].split(os.pathsep)
                          if "xcodegen-" in part)
        pinned_bin.mkdir(parents=True)
        self.xcodegen = pinned_bin / "xcodegen"
        self.xcodegen.write_text("#!/bin/sh\nexit 0\n")
        self.xcodegen.chmod(0o755)

    def invoke(self, arguments, environment=None):
        calls = []

        def child(command, **kwargs):
            calls.append((command, kwargs))
            self.assertEqual(kwargs["cwd"], self.repo)
            self.assertTrue((kwargs["cwd"] / "global.json").is_file())
            self.assertEqual(kwargs["env"]["SKY_WORK_DIR"], str(self.work))
            if len(command) > 1 and command[1] == str(self.source / "audio_renderer/build_ios.py"):
                response = self.source / "build/audio-renderer-ios/ios-arm64"
                response.mkdir(parents=True)
                (response / "SkyAudioRenderer.a").write_bytes(b"mock publish")
                (response / "SkyAudioRenderer.link.rsp").write_text("mock link inputs")
            return subprocess.CompletedProcess(command, 0)

        previous = Path.cwd()
        try:
            os.chdir(self.foreign)
            with mock.patch.dict(os.environ, {"PATH": "", **(environment or {})}, clear=True), \
                 mock.patch.object(BUILD_IOS, "REPO", self.repo), \
                 mock.patch.object(BUILD_IOS, "SOURCE", self.source), \
                 mock.patch.object(BUILD_IOS, "BUILD", self.build), \
                 mock.patch.object(sys, "argv", [str(HELPER), "--sdk", "device", *arguments]), \
                 mock.patch.object(BUILD_IOS, "prepare") as prepare, \
                 mock.patch.object(BUILD_IOS.subprocess, "run", side_effect=child), \
                 contextlib.redirect_stdout(io.StringIO()):
                BUILD_IOS.main()
                prepared = prepare.call_args.args
        finally:
            os.chdir(previous)
        return calls, prepared

    def test_custom_workspace_and_foreign_cwd_use_pinned_tools_and_repo_sdk(self):
        calls, prepared = self.invoke(["--work-dir", "custom workspace", "--derived-data", "derived data"])
        self.assertEqual(prepared[0], self.work / "build/rewrite-native")
        self.assertEqual(prepared[1], self.source.parent / "skyrim-work/build/shaders")
        self.assertEqual(calls[0][0][-2:], ["--work-dir", str(self.work)])
        self.assertEqual(calls[1][0][0], str(self.xcodegen))
        self.assertEqual(calls[2][0][0], "xcodebuild")
        command = calls[2][0]
        self.assertEqual(command[command.index("-derivedDataPath") + 1], str(self.foreign / "derived data"))

    def test_environment_workspace_and_rewrite_override_survive_prepare_only(self):
        rewrite = self.foreign / "explicit rewritten inputs"
        calls, prepared = self.invoke(["--prepare-only"], {
            "SKY_WORK_DIR": str(self.work), "SKY_REWRITE_DIR": str(rewrite),
            "SKY_SHADER_CACHE_DIR": str(self.root / "shader pairs"),
        })
        self.assertEqual(prepared[0], rewrite)
        self.assertEqual(prepared[1], self.root / "shader pairs")
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[-1][0][0], str(self.xcodegen))

    def test_missing_tool_reports_tools_only_setup_before_game_preparation(self):
        error = io.StringIO()
        with mock.patch.dict(os.environ, {"PATH": ""}, clear=True), \
             mock.patch.object(sys, "argv", [str(HELPER), "--work-dir", str(self.root / "uninitialized work")]), \
             mock.patch.object(BUILD_IOS, "prepare") as prepare, \
             contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
            BUILD_IOS.main()
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("--tools-only --ios --work-dir", error.getvalue())
        prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
