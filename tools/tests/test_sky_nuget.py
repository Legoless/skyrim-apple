#!/usr/bin/env python3
"""Verify NuGet configuration precedence with the pinned SDK and no packages."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sky_tools import tool_environment

REPO = Path(__file__).resolve().parents[2]


class NuGetConfigurationTests(unittest.TestCase):
    def test_restore_ignores_inherited_private_sources_disabled_feed_and_fallback(self):
        work = Path(os.environ.get("SKY_WORK_DIR", REPO / "skyrim-work")).expanduser().resolve()
        env = tool_environment(work)
        dotnet = Path(env["DOTNET_ROOT"]) / "dotnet"
        self.assertTrue(dotnet.is_file(), "Run ./setup-skyrim.sh --tools-only before this integration check")
        with tempfile.TemporaryDirectory(prefix="sky nuget '") as temporary:
            root = Path(temporary).resolve()
            checkout = root / "relocated checkout"
            project = checkout / "project"
            project.mkdir(parents=True)
            private_fallback = root / "unavailable private fallback"
            (root / "NuGet.Config").write_text(
                '<configuration><packageSources><clear />'
                '<add key="private" value="https://private.invalid/v3/index.json" />'
                '<add key="nuget.org" value="https://api.nuget.org/v3/index.json" />'
                '</packageSources><disabledPackageSources><add key="nuget.org" value="true" />'
                '</disabledPackageSources><fallbackPackageFolders><add key="private" value="'
                + escape(str(private_fallback), {'"': '&quot;'})
                + '" /></fallbackPackageFolders><packageSourceMapping>'
                '<packageSource key="private"><package pattern="*" /></packageSource>'
                '</packageSourceMapping></configuration>'
            )
            shutil.copy2(REPO / "NuGet.Config", checkout / "NuGet.Config")
            shutil.copy2(REPO / "global.json", checkout / "global.json")
            (project / "Probe.csproj").write_text(
                '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
                '<TargetFramework>net10.0</TargetFramework><NuGetAudit>false</NuGetAudit>'
                '</PropertyGroup></Project>'
            )
            for name in ("RestoreConfigFile", "RestoreSources", "RestoreAdditionalProjectSources", "RestorePackagesPath",
                         "RestoreFallbackFolders", "RestoreAdditionalProjectFallbackFolders",
                         "NUGET_FALLBACK_PACKAGES"):
                env.pop(name, None)
            env.update(DOTNET_CLI_HOME=str(root / "dotnet-home"),
                       NUGET_PACKAGES=str(root / "packages"),
                       NUGET_HTTP_CACHE_PATH=str(root / "http-cache"))
            result = subprocess.run([str(dotnet), "restore", str(project / "Probe.csproj"),
                                     "--verbosity", "quiet"], cwd=checkout, env=env,
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            assets = json.loads((project / "obj/project.assets.json").read_text())
            restore = assets["project"]["restore"]
            self.assertEqual(list(restore["sources"]), ["https://api.nuget.org/v3/index.json"])
            self.assertEqual(Path(restore["packagesPath"]), root / "packages")
            self.assertEqual(restore.get("fallbackFolders", []), [])
            self.assertIn(str(root / "NuGet.Config"), restore["configFilePaths"])
            self.assertIn(str(checkout / "NuGet.Config"), restore["configFilePaths"])
            self.assertFalse(private_fallback.exists())
            self.assertFalse(any((root / "packages").glob("*")), "The probe must not download packages")

    def test_production_environment_overrides_ambient_restore_sources_paths_and_fallbacks(self):
        installed_work = Path(os.environ.get("SKY_WORK_DIR", REPO / "skyrim-work")).expanduser().resolve()
        sdk = Path(tool_environment(installed_work)["DOTNET_ROOT"])
        self.assertTrue((sdk / "dotnet").is_file(), "Run ./setup-skyrim.sh --tools-only before this integration check")
        with tempfile.TemporaryDirectory(prefix="sky restore environment '") as temporary:
            root = Path(temporary).resolve()
            work = root / "private workspace"
            local_sdk = work / "build/tools" / sdk.name
            local_sdk.parent.mkdir(parents=True)
            local_sdk.symlink_to(sdk, target_is_directory=True)
            project = root / "relocated checkout"
            project.mkdir()
            shutil.copy2(REPO / "global.json", project / "global.json")
            (project / "Probe.csproj").write_text(
                '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
                '<TargetFramework>net10.0</TargetFramework><NuGetAudit>false</NuGetAudit>'
                '</PropertyGroup></Project>'
            )
            hostile = {
                "RestoreSources": "https://private.invalid/v3/index.json",
                "RestoreAdditionalProjectSources": "https://additional.private.invalid/v3/index.json",
                "RestorePackagesPath": str(root / "ambient restore packages"),
                "RestoreFallbackFolders": str(root / "unavailable restore fallback"),
                "RestoreAdditionalProjectFallbackFolders": str(root / "unavailable additional fallback"),
                "NUGET_FALLBACK_PACKAGES": str(root / "unavailable NuGet fallback"),
                "NUGET_PACKAGES": str(root / "ambient global packages"),
            }
            with patch.dict(os.environ, hostile):
                env = tool_environment(work)
            result = subprocess.run([str(local_sdk / "dotnet"), "restore", str(project / "Probe.csproj"),
                                     "--verbosity", "quiet"], cwd=project, env=env,
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            restore = json.loads((project / "obj/project.assets.json").read_text())["project"]["restore"]
            self.assertEqual(list(restore["sources"]), ["https://api.nuget.org/v3/index.json"])
            self.assertEqual(Path(restore["packagesPath"]), work / "build/tools/nuget/packages")
            self.assertEqual(restore.get("fallbackFolders", []), [])
            self.assertFalse(any((work / "build/tools/nuget/packages").glob("*")))
            for name, value in hostile.items():
                if name not in ("RestoreSources", "RestoreAdditionalProjectSources"):
                    self.assertFalse(Path(value).exists(), f"Ambient {name} must not be used")


if __name__ == "__main__":
    unittest.main()
