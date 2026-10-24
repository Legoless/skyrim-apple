#!/usr/bin/env python3
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


TITLE_ID = "01000a10041ea000"
UPDATE_META_TITLE_ID = "01000a10041ea800"
SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "sky_extract.sh"
FAKE_HACTOOL = r'''#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import struct
import sys


def option(name):
    prefix = name + "="
    for arg in args:
        if arg.startswith(prefix):
            return arg[len(prefix):]
    raise SystemExit(92)


def record(event):
    with open(os.environ["FAKE_HACTOOL_LOG"], "a", encoding="utf-8") as stream:
        stream.write(json.dumps(event, sort_keys=True) + "\n")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


args = sys.argv[1:]
if "-k" not in args or "--disablekeywarns" not in args or "--suppresskeys" not in args:
    raise SystemExit(90)
target = Path(args[-1])
intype = args[args.index("-t") + 1]
if intype in ("xci", "pfs0"):
    outdir = Path(option("--securedir" if intype == "xci" else "--outdir"))
    outdir.mkdir(parents=True, exist_ok=True)
    package = json.loads(target.read_text(encoding="utf-8"))
    event = {
        "event": "unpack",
        "package_type": intype,
        "source": str(target),
        "outdir": str(outdir),
        "ncas": [],
    }
    for spec in package["ncas"]:
        nca = outdir / spec["name"]
        data = json.dumps(spec, sort_keys=True, separators=(",", ":")).encode("utf-8")
        nca.write_bytes(data)
        event["ncas"].append({"name": spec["name"], "sha256": hashlib.sha256(data).hexdigest()})
    record(event)
    raise SystemExit(0)

spec = json.loads(target.read_text(encoding="utf-8"))
if any(arg.startswith("--section0dir=") for arg in args):
    outdir = Path(option("--section0dir"))
    outdir.mkdir(parents=True, exist_ok=True)
    cnmt = struct.pack("<QI", int(spec["title_id"], 16), spec["version"]) + bytes(20)
    (outdir / "content.cnmt").write_bytes(cnmt)
    record({"event": "meta", "source": str(target), "outdir": str(outdir)})
    raise SystemExit(0)

if any(arg.startswith("--exefsdir=") for arg in args):
    layered = any(arg.startswith("--basenca=") for arg in args)
    has_romfsdir = any(arg.startswith("--romfsdir=") for arg in args)
    if not spec.get("bktr") or (not layered and has_romfsdir):
        raise SystemExit(93)
    exefs = Path(option("--exefsdir"))
    exefs.mkdir(parents=True, exist_ok=True)
    if os.environ.get("FAKE_HACTOOL_EMPTY") != "ExeFS":
        (exefs / "program.bin").write_bytes(b"updated executable")
    if layered:
        base_nca = Path(option("--basenca"))
        base_spec = json.loads(base_nca.read_text(encoding="utf-8"))
        if base_spec.get("bktr"):
            raise SystemExit(93)
        romfs = Path(option("--romfsdir"))
        (romfs / "assets").mkdir(parents=True, exist_ok=True)
        if os.environ.get("FAKE_HACTOOL_EMPTY") != "RomFS":
            merged = b"base:" + base_spec["payload"].encode() + b";update:" + spec["payload"].encode()
            (romfs / "assets" / "merged.bin").write_bytes(merged)
        record({
            "event": "layer",
            "base_nca": str(base_nca),
            "update_nca": str(target),
            "base_nca_sha256": sha256(base_nca),
            "update_nca_sha256": sha256(target),
            "exefsdir": str(exefs),
            "romfsdir": str(romfs),
        })
    else:
        if os.environ.get("FAKE_HACTOOL_DELETE_AFTER_EXTRACT"):
            Path(os.environ["FAKE_HACTOOL_DELETE_AFTER_EXTRACT"]).unlink()
        record({
            "event": "program",
            "update_nca": str(target),
            "update_nca_sha256": sha256(target),
            "exefsdir": str(exefs),
            "has_basenca": False,
            "has_romfsdir": False,
        })
    raise SystemExit(0)

if "-i" in args:
    print("NCA:")
    print("Title ID: " + spec["title_id"])
    print("Content Type: " + spec["content_type"])
    if spec["content_type"] == "Program":
        print("Sections:")
        print(" Partition Type: " + ("Patch RomFS" if spec.get("bktr") else "RomFS"))
        if spec.get("bktr"):
            print(" BKTR relocation data")
    raise SystemExit(0)
raise SystemExit(91)
'''


class SkyExtractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sk_root = self.root / "work"
        self.base_pkg = self.root / "base.xci"
        self.update_pkg = self.root / "update.nsp"
        self.keys = self.root / "prod.keys"
        self.keys.write_text("TEST-ONLY-NOT-A-REAL-KEY\n", encoding="utf-8")
        self.fake_hactool = self.root / "fake_hactool.py"
        self.fake_hactool.write_text(FAKE_HACTOOL, encoding="utf-8")
        self.fake_hactool.chmod(0o755)
        self.hactool_log = self.root / "hactool.jsonl"
        self.set_pin("v327680")

    def tearDown(self):
        self.temporary.cleanup()

    def set_pin(self, pin):
        self.pin = pin
        self.dest = self.sk_root / "data" / "switch" / "skyrim" / pin
        self.out = self.dest / "Update"
        self.program_out = self.dest / "Program"

    @staticmethod
    def program(name, payload, title_id=TITLE_ID, bktr=False):
        return {
            "name": name,
            "content_type": "Program",
            "title_id": title_id,
            "bktr": bktr,
            "payload": payload,
        }

    @staticmethod
    def meta(name, title_id, version):
        return {"name": name, "content_type": "Meta", "title_id": title_id, "version": version}

    @staticmethod
    def control(name):
        return {"name": name, "content_type": "Control", "title_id": TITLE_ID}

    def write_packages(self, base_ncas=None, update_ncas=None):
        if base_ncas is None:
            base_ncas = [
                self.program("misleading-update-name.nca", "base assets", TITLE_ID.upper()),
                self.meta("base-meta.nca", TITLE_ID, 0),
                self.control("base-control.nca"),
            ]
        if update_ncas is None:
            update_ncas = [
                self.program("misleading-base-name.nca", "update assets", bktr=True),
                self.meta("update-meta.nca", UPDATE_META_TITLE_ID.upper(), 327680),
                self.control("update-control.nca"),
            ]
        self.base_pkg.write_text(json.dumps({"ncas": base_ncas}), encoding="utf-8")
        self.update_pkg.write_text(json.dumps({"ncas": update_ncas}), encoding="utf-8")

    def run_extract(self, program_only=False, extra_env=None):
        env = os.environ.copy()
        env.update({
            "SK_ROOT": str(self.sk_root),
            "HACTOOL": str(self.fake_hactool),
            "FAKE_HACTOOL_LOG": str(self.hactool_log),
        })
        if extra_env:
            env.update(extra_env)
        command = ["bash", str(SCRIPT)]
        if program_only:
            command.append("--program-only")
        command.extend([str(self.base_pkg), str(self.update_pkg), str(self.keys), self.pin])
        return subprocess.run(
            command,
            text=True,
            capture_output=True,
            env=env,
            timeout=30,
            check=False,
        )

    def events(self):
        if not self.hactool_log.exists():
            return []
        return [json.loads(line) for line in self.hactool_log.read_text(encoding="utf-8").splitlines()]

    def manifest_rows(self):
        with (self.dest / "meta" / "packages.tsv").open(newline="", encoding="utf-8") as stream:
            return list(csv.DictReader(stream, delimiter="\t"))

    def program_rows(self):
        with (self.dest / "meta" / "program.tsv").open(newline="", encoding="utf-8") as stream:
            return list(csv.DictReader(stream, delimiter="\t"))

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        exposed = result.stdout + result.stderr + self.hactool_log.read_text(encoding="utf-8")
        exposed += (self.dest / "meta" / "packages.tsv").read_text(encoding="utf-8")
        self.assertNotIn("TEST-ONLY-NOT-A-REAL-KEY", exposed)
        self.assertFalse(Path(str(self.out) + ".part").exists())
        self.assertFalse((self.dest / "meta.part").exists())
        self.assert_temp_workspace_clean()

    def assert_program_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        exposed = result.stdout + result.stderr + self.hactool_log.read_text(encoding="utf-8")
        exposed += (self.dest / "meta" / "program.tsv").read_text(encoding="utf-8")
        self.assertNotIn("TEST-ONLY-NOT-A-REAL-KEY", exposed)
        self.assertFalse(Path(str(self.program_out) + ".part").exists())
        self.assertFalse((self.dest / "meta.part").exists())
        self.assert_temp_workspace_clean()

    def assert_temp_workspace_clean(self):
        temporary = self.sk_root / "build" / "cache" / "skyrim"
        leftovers = list(temporary.glob("extract.*")) if temporary.exists() else []
        self.assertEqual(leftovers, [])

    def assert_failed_without_output(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.out.exists())
        self.assertFalse(self.program_out.exists())
        self.assertFalse((self.dest / "meta").exists())
        self.assertFalse(Path(str(self.out) + ".part").exists())
        self.assertFalse(Path(str(self.program_out) + ".part").exists())
        self.assertFalse((self.dest / "meta.part").exists())
        self.assert_temp_workspace_clean()

    def test_separate_base_update_flow_and_basenca_layering(self):
        self.write_packages()

        result = self.run_extract()

        self.assert_success(result)
        events = self.events()
        unpacks = [event for event in events if event["event"] == "unpack"]
        self.assertEqual([event["package_type"] for event in unpacks], ["xci", "pfs0"])
        self.assertNotEqual(unpacks[0]["outdir"], unpacks[1]["outdir"])
        layer = next(event for event in events if event["event"] == "layer")
        self.assertTrue(layer["base_nca"].startswith(unpacks[0]["outdir"] + os.sep))
        self.assertTrue(layer["update_nca"].startswith(unpacks[1]["outdir"] + os.sep))
        self.assertEqual(layer["exefsdir"], str(self.out) + ".part/ExeFS")
        self.assertEqual(layer["romfsdir"], str(self.out) + ".part/RomFS")
        self.assertEqual((self.out / "ExeFS" / "program.bin").read_bytes(), b"updated executable")
        self.assertEqual(
            (self.out / "RomFS" / "assets" / "merged.bin").read_bytes(),
            b"base:base assets;update:update assets",
        )

    def test_packages_tsv_hashes_versions_and_sources(self):
        self.write_packages()

        result = self.run_extract()

        self.assert_success(result)
        rows = self.manifest_rows()
        self.assertEqual([row["role"] for row in rows], ["base", "update"])
        layer = next(event for event in self.events() if event["event"] == "layer")
        for row, package, version in zip(rows, (self.base_pkg, self.update_pkg), ("0", "327680")):
            self.assertEqual(row["package_sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
            self.assertEqual(row["title_id"], TITLE_ID)
            self.assertEqual(row["content_version"], version)
            self.assertEqual(row["source_package"], str(package))
        self.assertEqual(rows[0]["nca_sha256"], layer["base_nca_sha256"])
        self.assertEqual(rows[1]["nca_sha256"], layer["update_nca_sha256"])

    def test_update_program_requires_bktr(self):
        self.write_packages(update_ncas=[
            self.program("update.nca", "not a patch", bktr=False),
            self.meta("update-meta.nca", UPDATE_META_TITLE_ID, 327680),
        ])

        result = self.run_extract()

        self.assert_failed_without_output(result)
        self.assertIn("BKTR", result.stderr)
        self.assertNotIn("layer", [event["event"] for event in self.events()])

    def test_wrong_title_id_is_rejected(self):
        self.write_packages(base_ncas=[
            self.program("base.nca", "wrong game", title_id="0100000000000000"),
            self.meta("base-meta.nca", TITLE_ID, 0),
        ])

        result = self.run_extract()

        self.assert_failed_without_output(result)
        self.assertIn("title ID", result.stderr)

    def test_ambiguous_program_ncas_are_rejected(self):
        self.write_packages(base_ncas=[
            self.program("one.nca", "one"),
            self.program("two.nca", "two"),
            self.meta("base-meta.nca", TITLE_ID, 0),
        ])

        result = self.run_extract()

        self.assert_failed_without_output(result)
        self.assertIn("ambiguous base Program NCA", result.stderr)

    def test_existing_output_is_refused_without_changes(self):
        self.write_packages()
        self.out.mkdir(parents=True)
        sentinel = self.out / "keep.txt"
        sentinel.write_text("user data", encoding="utf-8")

        result = self.run_extract()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "user data")
        self.assertEqual(self.events(), [])

    def test_existing_part_is_refused_without_changes(self):
        self.write_packages()
        part = Path(str(self.out) + ".part")
        part.mkdir(parents=True)
        sentinel = part / "keep.txt"
        sentinel.write_text("user data", encoding="utf-8")

        result = self.run_extract()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "user data")
        self.assertFalse(self.out.exists())
        self.assertEqual(self.events(), [])
        self.assert_temp_workspace_clean()

    def test_empty_extracted_directory_is_rejected(self):
        self.write_packages()

        result = self.run_extract(extra_env={"FAKE_HACTOOL_EMPTY": "RomFS"})

        self.assert_failed_without_output(result)
        self.assertIn("RomFS", result.stderr)

    def test_program_only_skips_base_and_extracts_only_exefs(self):
        self.write_packages()

        result = self.run_extract(program_only=True)

        self.assert_program_success(result)
        events = self.events()
        unpacks = [event for event in events if event["event"] == "unpack"]
        self.assertEqual([event["package_type"] for event in unpacks], ["pfs0"])
        self.assertNotIn("layer", [event["event"] for event in events])
        program = next(event for event in events if event["event"] == "program")
        self.assertFalse(program["has_basenca"])
        self.assertFalse(program["has_romfsdir"])
        self.assertEqual(program["exefsdir"], str(self.program_out) + ".part/ExeFS")
        self.assertEqual((self.program_out / "ExeFS" / "program.bin").read_bytes(), b"updated executable")
        self.assertEqual([path.name for path in self.program_out.iterdir()], ["ExeFS"])
        self.assertEqual([path.name for path in (self.dest / "meta").iterdir()], ["program.tsv"])
        self.assertFalse(self.out.exists())
        self.assertNotIn("base version", result.stdout)

    def test_program_tsv_hash_version_and_source(self):
        self.write_packages()

        result = self.run_extract(program_only=True)

        self.assert_program_success(result)
        rows = self.program_rows()
        self.assertEqual(len(rows), 1)
        row = rows[0]
        program = next(event for event in self.events() if event["event"] == "program")
        self.assertEqual(row["package_sha256"], hashlib.sha256(self.update_pkg.read_bytes()).hexdigest())
        self.assertEqual(row["nca_sha256"], program["update_nca_sha256"])
        self.assertEqual(row["title_id"], TITLE_ID)
        self.assertEqual(row["content_version"], "327680")
        self.assertEqual(row["role"], "update")
        self.assertEqual(row["source_package"], str(self.update_pkg))

    def test_program_only_empty_exefs_is_rejected(self):
        self.write_packages()

        result = self.run_extract(program_only=True, extra_env={"FAKE_HACTOOL_EMPTY": "ExeFS"})

        self.assert_failed_without_output(result)
        self.assertIn("ExeFS", result.stderr)

    def test_program_only_existing_output_is_refused_without_changes(self):
        self.write_packages()
        self.program_out.mkdir(parents=True)
        sentinel = self.program_out / "keep.txt"
        sentinel.write_text("user data", encoding="utf-8")

        result = self.run_extract(program_only=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "user data")
        self.assertEqual(self.events(), [])

    def test_program_only_existing_part_is_refused_without_changes(self):
        self.write_packages()
        part = Path(str(self.program_out) + ".part")
        part.mkdir(parents=True)
        sentinel = part / "keep.txt"
        sentinel.write_text("user data", encoding="utf-8")

        result = self.run_extract(program_only=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "user data")
        self.assertFalse(self.program_out.exists())
        self.assertEqual(self.events(), [])
        self.assert_temp_workspace_clean()

    def test_program_only_validates_base_path_before_extraction(self):
        self.write_packages()
        self.base_pkg = self.root / "missing.xci"

        result = self.run_extract(program_only=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("base XCI not found", result.stderr)
        self.assertEqual(self.events(), [])

    def test_full_mode_rejects_numeric_pin_version_mismatch(self):
        self.write_packages()
        self.set_pin("v327681")

        result = self.run_extract()

        self.assert_failed_without_output(result)
        self.assertIn("content version 327680 does not match pin v327681", result.stderr)
        self.assertNotIn("layer", [event["event"] for event in self.events()])

    def test_program_only_rejects_numeric_pin_version_mismatch(self):
        self.write_packages()
        self.set_pin("v327681")

        result = self.run_extract(program_only=True)

        self.assert_failed_without_output(result)
        self.assertIn("content version 327680 does not match pin v327681", result.stderr)
        events = self.events()
        self.assertNotIn("program", [event["event"] for event in events])
        unpacks = [event for event in events if event["event"] == "unpack"]
        self.assertEqual([event["package_type"] for event in unpacks], ["pfs0"])

    def test_program_only_then_full_preserves_both_modes(self):
        self.write_packages()

        program_result = self.run_extract(program_only=True)

        self.assert_program_success(program_result)
        program_manifest = (self.dest / "meta" / "program.tsv").read_bytes()
        program_files = {
            str(path.relative_to(self.program_out)): path.read_bytes()
            for path in self.program_out.rglob("*") if path.is_file()
        }
        full_result = self.run_extract()
        self.assert_success(full_result)
        self.assertEqual((self.dest / "meta" / "program.tsv").read_bytes(), program_manifest)
        self.assertEqual({
            str(path.relative_to(self.program_out)): path.read_bytes()
            for path in self.program_out.rglob("*") if path.is_file()
        }, program_files)
        self.assertEqual((self.out / "ExeFS" / "program.bin").read_bytes(), b"updated executable")
        self.assertTrue((self.out / "RomFS" / "assets" / "merged.bin").is_file())
        self.assertEqual(
            {path.name for path in (self.dest / "meta").iterdir()},
            {"program.tsv", "packages.tsv"},
        )

    def test_full_then_program_only_preserves_both_modes(self):
        self.write_packages()

        full_result = self.run_extract()

        self.assert_success(full_result)
        packages_manifest = (self.dest / "meta" / "packages.tsv").read_bytes()
        update_files = {
            str(path.relative_to(self.out)): path.read_bytes()
            for path in self.out.rglob("*") if path.is_file()
        }
        program_result = self.run_extract(program_only=True)
        self.assert_program_success(program_result)
        self.assertEqual((self.dest / "meta" / "packages.tsv").read_bytes(), packages_manifest)
        self.assertEqual({
            str(path.relative_to(self.out)): path.read_bytes()
            for path in self.out.rglob("*") if path.is_file()
        }, update_files)
        self.assertEqual((self.program_out / "ExeFS" / "program.bin").read_bytes(), b"updated executable")
        self.assertEqual(
            {path.name for path in (self.dest / "meta").iterdir()},
            {"program.tsv", "packages.tsv"},
        )

    def test_failure_preserves_preexisting_shared_meta_and_other_mode(self):
        self.write_packages()
        full_result = self.run_extract()
        self.assert_success(full_result)
        packages_manifest = (self.dest / "meta" / "packages.tsv").read_bytes()
        update_files = {
            str(path.relative_to(self.out)): path.read_bytes()
            for path in self.out.rglob("*") if path.is_file()
        }

        result = self.run_extract(
            program_only=True,
            extra_env={"FAKE_HACTOOL_DELETE_AFTER_EXTRACT": str(self.update_pkg)},
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("could not hash update NSP", result.stderr)
        self.assertEqual((self.dest / "meta" / "packages.tsv").read_bytes(), packages_manifest)
        self.assertEqual({
            str(path.relative_to(self.out)): path.read_bytes()
            for path in self.out.rglob("*") if path.is_file()
        }, update_files)
        self.assertFalse(self.program_out.exists())
        self.assertFalse((self.dest / "meta" / "program.tsv").exists())
        self.assertFalse(Path(str(self.program_out) + ".part").exists())
        self.assertEqual([path.name for path in (self.dest / "meta").iterdir()], ["packages.tsv"])
        self.assert_temp_workspace_clean()

    def test_existing_own_manifest_is_refused_for_each_mode(self):
        self.write_packages()
        for program_only, manifest in ((False, "packages.tsv"), (True, "program.tsv")):
            with self.subTest(manifest=manifest):
                self.set_pin("existing-" + manifest)
                (self.dest / "meta").mkdir(parents=True)
                sentinel = self.dest / "meta" / manifest
                sentinel.write_text("user data", encoding="utf-8")
                result = self.run_extract(program_only=program_only)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("refusing to overwrite", result.stderr)
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "user data")
        self.assertEqual(self.events(), [])

    def test_non_directory_meta_is_rejected_without_changes(self):
        self.write_packages()
        self.dest.mkdir(parents=True)
        meta = self.dest / "meta"
        meta.write_text("user data", encoding="utf-8")

        result = self.run_extract(program_only=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a directory", result.stderr)
        self.assertEqual(meta.read_text(encoding="utf-8"), "user data")
        self.assertEqual(self.events(), [])

    def test_non_numeric_pins_remain_allowed_in_both_modes(self):
        self.write_packages()
        self.set_pin("full-analysis")

        full_result = self.run_extract()

        self.assert_success(full_result)
        self.set_pin("program-analysis")
        program_result = self.run_extract(program_only=True)
        self.assert_program_success(program_result)

    def test_unsafe_update_pins_are_rejected_before_extraction(self):
        self.write_packages()
        for pin in ("", ".", "..", "../v327680", "v/327680", "v 327680"):
            with self.subTest(pin=pin):
                self.pin = pin
                result = self.run_extract()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("update-pin", result.stderr)
        self.assertEqual(self.events(), [])


if __name__ == "__main__":
    unittest.main()
