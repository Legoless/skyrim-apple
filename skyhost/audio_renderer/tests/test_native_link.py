"""Execute the production MSBuild hook across a real shell argument boundary."""

import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
from sky_tools import tool_environment

PROJECT = REPO / "skyhost/audio_renderer/SkyAudioRenderer.csproj"


class NativeLibraryPathTests(unittest.TestCase):
    def run_target(self, rid, kind):
        with tempfile.TemporaryDirectory(prefix="sky native link ") as directory:
            root = Path(directory).resolve()
            libraries = [root / "NuGet package's native files" / name
                         for name in ("libbootstrapperdll.o", "libRuntime.WorkstationGC.a")]
            for library in libraries:
                library.parent.mkdir(exist_ok=True)
                library.write_bytes(b"synthetic native input")
            output = root / "linker arguments.json"
            capture = root / "capture arguments.py"
            capture.write_text("import json,sys\nfrom pathlib import Path\n"
                               "Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]))\n")
            project = ET.Element("Project")
            setup = ET.SubElement(project, "Target", Name="SetupOSSpecificProps")
            items = ET.SubElement(setup, "ItemGroup")
            for library in libraries:
                ET.SubElement(items, "NativeLibrary", Include=str(library))
                ET.SubElement(items, "LinkerArg", Include=str(library))
            ET.SubElement(items, "LinkerArg", Include="-dynamiclib")
            hook = ET.parse(PROJECT).getroot().find("./Target[@Name='SkyQuoteNativeSharedLibraries']")
            self.assertIsNotNone(hook)
            project.append(hook)
            link = ET.SubElement(project, "Target", Name="LinkNative", DependsOnTargets="SetupOSSpecificProps")
            if kind == "Shared":
                command = shlex.join([sys.executable, str(capture), str(output)]) + " @(LinkerArg, ' ')"
                ET.SubElement(link, "Exec", Command=command)
            else:
                ET.SubElement(link, "WriteLinesToFile", File=str(output), Lines="@(LinkerArg)",
                              Overwrite="true", Encoding="utf-8")
            fixture = root / "native link.proj"
            ET.ElementTree(project).write(fixture, encoding="unicode")
            environment = tool_environment(REPO / "skyrim-work")
            dotnet = shutil.which("dotnet", path=environment["PATH"])
            self.assertIsNotNone(dotnet, "Run setup-skyrim.sh --tools-only to install the pinned SDK")
            result = subprocess.run([dotnet, "msbuild", str(fixture), "-nologo", "-verbosity:quiet",
                                     "-target:LinkNative", f"-p:RuntimeIdentifier={rid}", f"-p:NativeLib={kind}"],
                                    cwd=REPO, env=environment, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = output.read_text(encoding="utf-8-sig")
            return [str(path) for path in libraries], json.loads(text) if kind == "Shared" else text.splitlines()

    def test_macos_shared_link_receives_each_library_path_as_one_argument(self):
        libraries, arguments = self.run_target("osx-arm64", "Shared")
        self.assertCountEqual(arguments, ["-dynamiclib", *libraries])

    def test_ios_static_manifest_paths_remain_unquoted(self):
        libraries, arguments = self.run_target("ios-arm64", "Static")
        self.assertEqual(arguments, [*libraries, "-dynamiclib"])


if __name__ == "__main__":
    unittest.main()
