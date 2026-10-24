#!/usr/bin/env python3
"""Recover embedded NVN shader inputs from an owned Skyrim main module.

Accepts the original NSO or an existing flat rewritten image. The shader package
headers and shader code are read directly; the guest is never executed.
"""

import argparse
import json
from pathlib import Path
import sys

import shader_archive


ROOT = Path(__file__).resolve().parents[2]
CONTROL_SIZES = (0x7D1, 0x838, 0x848)


def module_image(path):
    data = path.read_bytes()
    if data[:4] != b"NSO0":
        return data
    # Reuse the checked NSO/LZ4 reader instead of adding another decoder.
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        from sky_inventory import parse_module
    finally:
        sys.path.pop(0)
    return parse_module(path).image


def extract_embedded(module, output):
    """Validate all packages before writing unique captures to a new directory."""
    module, output = Path(module), Path(output)
    shader_archive.require(not output.exists(),
                           "Capture output directory already exists; choose a new directory")
    captures = {}
    for metadata, code in shader_archive.fxp_captures(
            module_image(module), module.name, control_sizes=CONTROL_SIZES):
        captures.setdefault(metadata["shader_key"], (metadata, code))
    shader_archive.require(captures, "Module contains no supported shader records")
    output.mkdir(parents=True)
    for key in sorted(captures):
        metadata, code = captures[key]
        (output / metadata["code_file"]).write_bytes(code)
        (output / (key + ".json")).write_text(json.dumps(metadata, indent=2) + "\n")
    return sorted(captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path, help="Original main NSO or flat main.patched.bin")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        keys = extract_embedded(args.module, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Embedded shader extraction failed: {error}\n")
    print(f"SHADER_EMBEDDED unique={len(keys)} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
