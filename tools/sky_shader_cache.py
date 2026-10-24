"""Read-only identity checks for the recorded private Skyrim shader cache."""

import hashlib
from pathlib import Path
import re


# Hashes only: no game programs or translated shader contents are distributed.
# The reference is the complete local v327680 cache recorded on 2026-10-22.
REFERENCE = {
    "pairs": 13278,
    "files": 26556,
    "bytes": 323194827,
    "sha256": "2a46f1c550ea2397410a49e2e6b64841250fc553d91bf1930aa7db7e4e04ff2d",
}
KEY = re.compile(r"[0-9a-f]{64}")


def fingerprint(directory):
    """Hash the exact pair inventory, filenames and contents in stable order."""
    directory = Path(directory)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("Shader cache must be a regular directory")
    sources = {path.stem: path for path in directory.glob("*.metal")}
    metadata = {path.stem: path for path in directory.glob("*.json")
                if KEY.fullmatch(path.stem)}
    if not sources or sources.keys() != metadata.keys():
        raise ValueError("Shader cache is empty or contains incomplete shader pairs")
    if any(not KEY.fullmatch(key) for key in sources):
        raise ValueError("Shader cache contains an unexpected source filename")
    digest = hashlib.sha256()
    size = 0
    paths = sorted([*sources.values(), *metadata.values()], key=lambda path: path.name)
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise ValueError("Shader cache must contain regular files, not symbolic links")
        data = path.read_bytes()
        if not data:
            raise ValueError("Shader cache contains an empty file")
        size += len(data)
        content = hashlib.sha256(data).hexdigest()
        digest.update((path.name + "\0" + content + "\n").encode("ascii"))
    return {"pairs": len(sources), "files": len(paths), "bytes": size,
            "sha256": digest.hexdigest()}
