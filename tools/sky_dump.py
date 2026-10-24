#!/usr/bin/env python3
"""Read an owned XCI file or assemble the numbered parts of a split XCI dump."""

import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

BLOCK_SIZE = 8 * 1024 * 1024


def _numeric_entries(directory):
    return [path for path in directory.iterdir() if re.fullmatch(r"[0-9]+", path.name)]


def _parts_directory(source):
    candidates = ([source] if _numeric_entries(source) else [])
    candidates.extend(path for path in source.iterdir()
                      if path.is_dir() and path.suffix.lower() == ".xci")
    if len(candidates) != 1:
        detail = "Ambiguous split XCI directories" if candidates else "No numbered split XCI parts found"
        raise ValueError(detail + ": " + str(source))
    return candidates[0]


def _parts_metadata(directory):
    parts = sorted(_numeric_entries(directory), key=lambda path: (int(path.name), path.name))
    numbers = [int(path.name) for path in parts]
    if not parts or numbers != list(range(len(parts))):
        raise ValueError("Split XCI parts must be continuous, unique, and start at 00: " + str(directory))
    metadata = []
    for path in parts:
        if not path.is_file():
            raise ValueError("Split XCI part is not a file: " + str(path))
        stat = path.stat()
        if stat.st_size == 0:
            raise ValueError("Split XCI part is empty: " + str(path))
        metadata.append({"name": path.name, "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns})
    size = metadata[0]["bytes"]
    if any(part["bytes"] != size for part in metadata[:-1]) or metadata[-1]["bytes"] > size:
        raise ValueError("Split XCI parts have inconsistent sizes: " + str(directory))
    return parts, metadata


def _validate_header(source):
    with source.open("rb") as stream:
        stream.seek(0x100)
        if stream.read(4) != b"HEAD":
            raise ValueError("Missing XCI HEAD header at offset 0x100: " + str(source))


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(BLOCK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def _cache_valid(output, stamp, identity):
    try:
        saved = json.loads(stamp.read_text())
        return (saved["input"] == identity and output.is_file()
                and output.stat().st_size == identity["bytes"]
                and _sha256(output) == saved["sha256"])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _copy_parts(parts, destination, digest):
    for part in parts:
        with part.open("rb") as stream:
            for block in iter(lambda: stream.read(BLOCK_SIZE), b""):
                destination.write(block)
                digest.update(block)


def materialize_xci(source: Path, work_dir: Path) -> Path:
    """Return an XCI, caching split parts atomically without changing the dump.

    A split dump can contain numbered files directly or one nested .xci
    directory. Certificate and initial-data files beside those parts are ignored.
    """
    source = Path(source).expanduser().resolve()
    if source.is_file():
        if source.suffix.lower() != ".xci":
            raise ValueError("The game input must be an XCI file or split-dump directory: " + str(source))
        _validate_header(source)
        return source
    if not source.is_dir():
        raise ValueError("XCI file or split-dump directory not found: " + str(source))
    directory = _parts_directory(source)
    parts, metadata = _parts_metadata(directory)
    _validate_header(parts[0])
    cache = Path(work_dir).expanduser().resolve() / "build/cache/skyrim"
    if cache.resolve().is_relative_to(source):
        raise ValueError("The work directory must keep generated files outside the input dump.")
    output = cache / "input.xci"
    stamp = cache / "input.xci.json"
    identity = {"directory": str(directory.resolve()), "parts": metadata,
                "bytes": sum(part["bytes"] for part in metadata)}
    if _cache_valid(output, stamp, identity):
        return output
    cache.mkdir(parents=True, exist_ok=True)
    pending = []
    try:
        descriptor, temporary = tempfile.mkstemp(prefix=".input-", suffix=".xci", dir=cache)
        pending.append(Path(temporary))
        digest = hashlib.sha256()
        with os.fdopen(descriptor, "wb") as destination:
            _copy_parts(parts, destination, digest)
            destination.flush()
            os.fsync(destination.fileno())
        if pending[0].stat().st_size != identity["bytes"] or _parts_metadata(directory)[1] != metadata:
            raise RuntimeError("Split XCI input changed while it was being assembled; retry with a stable dump.")
        descriptor, temporary = tempfile.mkstemp(prefix=".input-", suffix=".json", dir=cache)
        pending.append(Path(temporary))
        with os.fdopen(descriptor, "w") as destination:
            json.dump({"input": identity, "sha256": digest.hexdigest()}, destination, indent=2)
            destination.write("\n")
            destination.flush()
            os.fsync(destination.fileno())
        # The published XCI is always complete. A crash between these two atomic
        # replacements leaves an invalid stamp, which causes a safe rebuild.
        pending[0].replace(output)
        pending[1].replace(stamp)
        return output
    finally:
        for path in pending:
            path.unlink(missing_ok=True)
