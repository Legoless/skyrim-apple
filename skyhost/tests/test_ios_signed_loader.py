#!/usr/bin/env python3
"""Exercise the production iOS signed-image mapper in a synthetic macOS host.

The fixture contains two tiny ARM64 functions, not game code. This verifies
remapping/protection/COW semantics on the host; device execution remains a
separate requirement.
"""
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import unittest


HOST_DIR = Path(__file__).resolve().parents[1]
GEN_EMBED = HOST_DIR / "ios/tools/gen_embed.py"


class SignedLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="skyhost-signed-loader-")
        cls.addClassCleanup(temporary.cleanup)
        cls.directory = Path(temporary.name)
        image = bytearray(0x10010)
        # mov x0, #0x1234 / #0x2345; ret, in original text and patch sections.
        struct.pack_into("<II", image, 0, 0xD2824680, 0xD65F03C0)
        struct.pack_into("<II", image, 0x10000, 0xD28468A0, 0xD65F03C0)
        image[0x8000:0x8008] = b"ORIGINAL"
        (cls.directory / "main.patched.bin").write_bytes(image)
        layout = {
            "PATCH_OFF": 0x10000, "KEY_PAGE_OFF": 0xC000,
            "KEY_PAGE_SIZE": 0x4000, "TEXT_END": 0x100,
            "RO_OFF": 0x4000, "DATA_OFF": 0x8000,
        }
        keys = ("K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI")
        layout.update((f"KEY_{name}_IDX", index) for index, name in enumerate(keys))
        (cls.directory / "main.fixups.tsv").write_text(
            "".join(f"# {name} {value:#x}\n" for name, value in layout.items()), encoding="utf-8",
        )
        (cls.directory / "sky_ios_embed_sizes.h").write_text(
            "#define SKY_IOS_MAIN_FILE_SIZE 0x10010ULL\n"
            "#define SKY_IOS_SDK_FILE_SIZE 0x10010ULL\n", encoding="utf-8",
        )
        assembly = cls.directory / "GuestImages.S"
        generated = subprocess.run(
            [sys.executable, str(GEN_EMBED), str(cls.directory), str(assembly), "main"],
            capture_output=True, text=True, timeout=30, check=False,
        )
        if generated.returncode:
            raise AssertionError(generated.stdout + generated.stderr)
        cls.executable = cls.directory / "t_signed_loader"
        sources = (
            "tests/t_ios_signed_loader.c", "gates.c", "svc.c", "support.c",
            "profile.c", "thread.c", "entry.S", "tests/crash_stub.c",
        )
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-D_DARWIN_C_SOURCE", "-DSKY_IOS_EMBEDDED_LOADER_TEST", "-UNDEBUG",
            "-O2", "-g", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(HOST_DIR), "-I", str(cls.directory), "-o", str(cls.executable),
        ]
        command += [str(HOST_DIR / source) for source in sources]
        command += [str(assembly)] + shlex.split(generated.stdout)
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_mode(self, mode):
        result = subprocess.run(
            [str(self.executable), mode, str(self.directory)],
            capture_output=True, text=True, timeout=15, check=False,
        )
        return result.returncode, result.stdout + result.stderr

    def assert_rejected(self, mode, diagnostic):
        returncode, output = self.run_mode(mode)
        self.assertEqual(returncode, -signal.SIGABRT, output)
        self.assertIn(f"FATAL main: {diagnostic}", output)
        self.assertNotIn("T_SIGNED_LOADER OK", output)

    def test_embedded_code_cow_data_keypage_and_relative_relocation(self):
        returncode, output = self.run_mode("success")
        self.assertEqual(returncode, 0, output)
        self.assertIn("SIGNED_IMAGE module=main code=embedded-shared-rx data=copy-on-write-rw", output)
        self.assertIn("RELOC main relative=1", output)
        self.assertIn("T_SIGNED_LOADER OK embedded-rx-execution cow-data ro-keypage relative-relocation", output)

    def test_overflowing_keypage_layout(self):
        self.assert_rejected("overflow-layout", "invalid key-page/patch layout")

    def test_mismatched_embedded_section_layout(self):
        self.assert_rejected("section-mismatch", "signed embedded section layout mismatch")

    def test_text_relocation(self):
        self.assert_rejected("text-reloc", "relocation target is not writable guest data 0")

    def test_readonly_data_relocation(self):
        self.assert_rejected("ro-reloc", "relocation target is not writable guest data 0x4000")

    def test_keypage_relocation(self):
        self.assert_rejected("key-reloc", "relocation target is not writable guest data 0xc000")

    def test_patch_relocation(self):
        self.assert_rejected("patch-reloc", "relocation target is not writable guest data 0x10000")


if __name__ == "__main__":
    unittest.main()
