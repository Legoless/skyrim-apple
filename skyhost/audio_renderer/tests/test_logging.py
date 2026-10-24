"""The audio logger must not enter Ryujinx's unsupported iOS color target."""
from pathlib import Path
import subprocess
import unittest


PROJECT = Path(__file__).resolve().parent / "LoggingRegression/LoggingRegression.csproj"


class AudioLoggingTests(unittest.TestCase):
    def test_actual_bridge_initialization_preserves_error_notice_and_native_stderr(self):
        # Common is referenced through two configurations sharing its output.
        result = subprocess.run(
            ["dotnet", "run", "--project", str(PROJECT), "-c", "Release", "--no-launch-profile",
             "-p:GeneratePackageOnBuild=false", "-p:BuildInParallel=false"],
            capture_output=True, text=True, timeout=120, check=False,
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("AUDIO_LOGGING_OK actual-bridge-init error-notice-data color-free-stderr no-console-target native-fd2-preserved", output)
        self.assertIn("NATIVE_STDERR_PRESERVED", result.stderr)
        self.assertNotIn("Unhandled exception", output)


if __name__ == "__main__":
    unittest.main()
