#!/usr/bin/env python3
"""Extract the original Skyrim cartridge without an update package or layering."""

import csv
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from sky_dump import materialize_xci

TITLE_ID = "01000a10041ea000"
CONTENT_VERSION = 0
COLUMNS = ("package_sha256", "nca_sha256", "title_id", "content_version", "role", "source_package")


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _hactool(env, keys, description, *arguments):
    command = [env["HACTOOL"], "-k", str(keys), "--disablekeywarns", "--suppresskeys", *map(str, arguments)]
    # Never print commands or tool diagnostics: either can expose private keys.
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    _require(result.returncode == 0, description + " failed; verify the dump and its console's keys.")
    return result.stdout


def _nca_info(text):
    fields = {}
    for name, pattern in (("magic", r"NCA[023]"), ("title", r"[0-9a-fA-F]{16}"),
                          ("type", r"[A-Za-z]+")):
        label = {"magic": "Magic", "title": "Title ID", "type": "Content Type"}[name]
        # Nested NPDM information also has a Title ID; inspect only NCA fields.
        values = re.findall(r"^" + label + r":[ \t]*(.*?)[ \t]*$", text, re.MULTILINE)
        _require(len(values) == 1 and re.fullmatch(pattern, values[0]),
                 "Invalid NCA metadata; verify the dump and its console's keys.")
        fields[name] = values[0].lower() if name == "title" else values[0]
    return fields


def _game_files(game):
    return {str(path.relative_to(game)): {"bytes": path.stat().st_size, "sha256": _sha256(path)}
            for path in sorted(game.rglob("*")) if path.is_file() and path.name != ".DS_Store"}


def _control_capacities(path):
    _require(path.is_file(), "Missing extracted Control/control.nacp")
    with path.open("rb") as stream:
        nacp = stream.read(0x3158)
    _require(len(nacp) == 0x3158, "The cartridge control.nacp is incomplete.")
    initial = struct.unpack_from("<qq", nacp, 0x3080)
    maximum = struct.unpack_from("<qq", nacp, 0x3148)
    _require(all(value >= 0 for value in initial + maximum) and initial[0] > 0,
             "Invalid save capacities in cartridge control.nacp.")
    maximum = tuple(limit or size for size, limit in zip(initial, maximum))
    _require(all(limit >= size for size, limit in zip(initial, maximum)),
             "Cartridge control.nacp save maxima are smaller than their initial sizes.")
    return initial + maximum


def _validate_game(game):
    modules = {}
    for name in ("main", "sdk"):
        path = game / "ExeFS" / name
        _require(path.is_file(), "Missing extracted ExeFS/" + name)
        with path.open("rb") as stream:
            header = stream.read(0x100)
        _require(len(header) == 0x100 and header[:4] == b"NSO0", "Invalid extracted NSO: " + name)
        modules[name] = {"sha256": _sha256(path), "build_id": header[0x40:0x60].hex()}
    ini, data = game / "RomFS/Skyrim.ini", game / "RomFS/Data"
    _require(ini.is_file() and ini.stat().st_size > 0 and data.is_dir()
             and any(path.is_file() and path.stat().st_size > 0 for path in data.rglob("*")),
             "Extracted RomFS must contain Skyrim.ini and nonempty Data.")
    _control_capacities(game / "Control/control.nacp")
    return modules


def _reuse(destination, package_hash):
    game, metadata = destination / "Update", destination / "meta"
    try:
        state = json.loads((metadata / "extraction.json").read_text())
        with (metadata / "packages.tsv").open() as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        _require(state["package_sha256"] == package_hash, "Existing extraction is from another XCI.")
        _require(state["title_id"] == TITLE_ID and state["content_version"] == CONTENT_VERSION
                 and len(rows) == 1 and rows[0]["role"] == "base"
                 and rows[0]["package_sha256"] == package_hash and rows[0]["title_id"] == TITLE_ID
                 and rows[0]["content_version"] == str(CONTENT_VERSION), "Existing extraction metadata is invalid.")
        _require(_validate_game(game) == state["modules"] and _game_files(game) == state["files"],
                 "Existing extracted game files have changed.")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError("Refusing to overwrite an incomplete or mismatched base extraction; "
                         "use another --work-dir. " + str(error)) from None
    return game, metadata


def extract_xci(source: Path, keys: Path, work_dir: Path, env: dict) -> tuple[Path, Path]:
    """Publish base/Update and base/meta atomically, or verify and reuse them."""
    source, keys, work = (Path(path).expanduser().resolve() for path in (source, keys, work_dir))
    _require(not source.is_dir() or not work.is_relative_to(source),
             "Keep --work-dir outside the original split dump.")
    _require(keys.is_file() and keys.stat().st_size > 0, "Supply your console's nonempty prod.keys file.")
    tool = Path(env.get("HACTOOL", ""))
    _require(tool.is_file() and os.access(tool, os.X_OK), "The pinned local HACTOOL executable is missing.")
    destination = work / "data/switch/skyrim/base"
    _require(not source.is_dir() or not destination.resolve().is_relative_to(source),
             "Keep extraction outputs outside the original split dump.")
    cartridge = materialize_xci(source, work)
    package_hash = _sha256(cartridge)
    if destination.exists() or destination.is_symlink():
        _require(not destination.is_symlink(), "Refusing a symlink at the base extraction path.")
        return _reuse(destination, package_hash)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".base-extract-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        ncas, cnmt, published = staging / "nca", staging / "cnmt", staging / "published"
        ncas.mkdir()
        _hactool(env, keys, "XCI unpacking", "-x", "-t", "xci", "--securedir=" + str(ncas), cartridge)
        matches = {"Program": [], "Meta": [], "Control": []}
        for nca in sorted(ncas.glob("*.nca")):
            info = _hactool(env, keys, "NCA inspection", "-i", "-t", "nca", nca)
            fields = _nca_info(info)
            if fields["title"] == TITLE_ID and fields["type"] in matches:
                _require("BKTR" not in info and not re.search(r"Partition Type:\s*Patch RomFS", info),
                         "The original cartridge must not contain a BKTR update Program.")
                matches[fields["type"]].append(nca)
        for kind, files in matches.items():
            _require(len(files) == 1, "Expected exactly one Skyrim " + kind + " NCA for title " + TITLE_ID)
        program, meta, control = (matches[kind][0] for kind in ("Program", "Meta", "Control"))
        cnmt.mkdir()
        _hactool(env, keys, "CNMT extraction", "-x", "-t", "nca", "--section0dir=" + str(cnmt), meta)
        headers = list(cnmt.glob("*.cnmt"))
        _require(len(headers) == 1, "Expected exactly one cartridge CNMT.")
        with headers[0].open("rb") as stream:
            header = stream.read(0x20)
        _require(len(header) == 0x20, "The cartridge CNMT header is incomplete.")
        title, version = struct.unpack_from("<QI", header)
        _require(title == int(TITLE_ID, 16) and header[0xC] == 0x80, "The cartridge CNMT title/type is invalid.")
        _require(version == CONTENT_VERSION, "Expected cartridge content version 0; found " + str(version))
        game, metadata = published / "Update", published / "meta"
        game.mkdir(parents=True)
        _hactool(env, keys, "Base game extraction", "-x", "-t", "nca",
                 "--exefsdir=" + str(game / "ExeFS"), "--romfsdir=" + str(game / "RomFS"), program)
        _hactool(env, keys, "Control extraction", "-x", "-t", "nca",
                 "--romfsdir=" + str(game / "Control"), control)
        modules = _validate_game(game)
        metadata.mkdir()
        with (metadata / "packages.tsv").open("w") as stream:
            writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
            writer.writerow(COLUMNS)
            writer.writerow((package_hash, _sha256(program), TITLE_ID, version, "base", str(source)))
        state = {"package_sha256": package_hash, "title_id": TITLE_ID, "content_version": version,
                 "modules": modules, "files": _game_files(game)}
        (metadata / "extraction.json").write_text(json.dumps(state, indent=2) + "\n")
        _require(not destination.exists() and not destination.is_symlink(), "Base extraction path appeared during extraction.")
        published.rename(destination)
    return destination / "Update", destination / "meta"
