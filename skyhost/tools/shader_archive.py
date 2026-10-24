#!/usr/bin/env python3
"""Recover NVN shader captures from an owned Skyrim Switch shader BSA.

Supports the uncompressed version-105 BSA and FXP layout observed in the game.
Code and layout validation mirror nvn_program.c; outputs are private inputs for
shaders.py, not project-authored shader assets.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct

import shaders


CONTROL_MAGIC = b"\x34\x12\x76\x98"
CONTROL_SIZE = 0x880
CODE_OFFSET = 16 + CONTROL_SIZE
MAX_CODE_SIZE = 64 << 20
STAGE_NAMES = {0: "vertex", 1: "fragment", 5: "compute"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unpack(data, offset, layout):
    size = struct.calcsize(layout)
    require(0 <= offset <= len(data) - size, "Truncated shader archive structure")
    return struct.unpack_from(layout, data, offset)


def bsa_members(data):
    """Read bounded, uncompressed BSA members without using names as paths."""
    magic, version, offset, flags, folder_count, file_count, folder_bytes, name_bytes, _ = unpack(
        data, 0, "<4s8I")
    require(magic == b"BSA\0" and version == 105 and offset == 36,
            "Expected a version-105 BSA shader archive")
    require(flags == 3, "Only uncompressed BSA archives with folder and file names are supported")
    require(0 < folder_count <= file_count and file_count <= len(data) // 16,
            "Invalid BSA folder/file count")
    cursor = offset + folder_count * 24
    require(cursor <= len(data), "Truncated BSA folder table")
    records = []
    actual_folder_bytes = 0
    for index in range(folder_count):
        _, count, reserved, stored_offset = unpack(data, offset + index * 24, "<QIIQ")
        require(reserved == 0 and 0 < count <= file_count - len(records),
                "Invalid BSA folder record")
        require(stored_offset == cursor + name_bytes, "Unexpected BSA folder offset")
        require(cursor < len(data), "Truncated BSA folder name")
        length = data[cursor]
        cursor += 1
        require(length > 0 and cursor + length <= len(data), "Invalid BSA folder name length")
        folder = data[cursor:cursor + length]
        require(folder[-1:] == b"\0" and b"\0" not in folder[:-1], "Invalid BSA folder name")
        try:
            folder = folder[:-1].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("Invalid BSA folder name encoding") from error
        actual_folder_bytes += length
        cursor += length
        for _ in range(count):
            _, size, start = unpack(data, cursor, "<QII")
            require(size > 0 and not size & 0xc0000000, "Compressed or empty BSA member is unsupported")
            records.append((folder, size, start))
            cursor += 16
    require(len(records) == file_count and actual_folder_bytes == folder_bytes,
            "BSA folder inventory does not match header")
    end_names = cursor + name_bytes
    require(end_names <= len(data), "Truncated BSA file names")
    names = data[cursor:end_names].split(b"\0")
    require(names[-1:] == [b""] and len(names) == file_count + 1 and all(names[:-1]),
            "BSA filename inventory does not match header")
    ranges = []
    seen = set()
    members = []
    for raw_name, (folder, size, start) in zip(names[:-1], records):
        try:
            name = folder + "/" + raw_name.decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("Invalid BSA file name encoding") from error
        require(name.casefold() not in seen, "Duplicate BSA member name")
        seen.add(name.casefold())
        require(end_names <= start <= len(data) - size, "BSA member extends outside archive")
        ranges.append((start, start + size))
        members.append((name, start, size))
    ranges.sort()
    require(all(first[1] <= second[0] for first, second in zip(ranges, ranges[1:])),
            "Overlapping BSA members")
    for name, start, size in members:
        yield name, data[start:start + size]


def fxp_captures(data, member, *, control_sizes=(CONTROL_SIZE,)):
    """Validate shader packages and recover the same identity as runtime capture."""
    require(len(data) >= 8, "Truncated FXP shader package")
    cursor = 0
    count = 0
    previous_end = 0
    while True:
        control_start = data.find(CONTROL_MAGIC, cursor)
        if control_start < 0:
            break
        cursor = control_start + len(CONTROL_MAGIC)
        record = control_start - 16
        control_offset, control_size, code_offset, code_size = unpack(data, record, "<4I")
        require(control_offset == 16 and control_size in control_sizes and control_size >= 0x754
                and code_offset == 16 + control_size,
                "Unsupported FXP shader package layout")
        require(4 <= code_size <= MAX_CODE_SIZE and record + code_offset <= len(data) - code_size,
                "Truncated FXP shader code")
        require(record >= previous_end, "Overlapping FXP shader records")
        program_length, constant_size, constant_offset, declared_size, program_start = unpack(
            data, control_start + 0x6f8, "<5I")
        workgroup = list(unpack(data, control_start + 0x748, "<3I"))
        require(declared_size == code_size, "FXP shader code size mismatch")
        require(program_length > 0 and program_length % 8 == 0
                and program_start + program_length <= code_size, "Invalid FXP program range")
        require(constant_offset == (program_start + program_length + 255) & ~255,
                "FXP constant offset mismatch")
        require(code_size == (constant_offset + constant_size + 255) & ~255,
                "FXP code block layout mismatch")
        start = record + code_offset
        code = data[start:start + code_size]
        magic, = unpack(code, 0, "<I")
        if magic == 0x12345678:
            require(program_start == 0x30 and code[4:0x30] == bytes(0x2c),
                    "Invalid FXP graphics header")
            sph, = unpack(code, 0x30, "<I")
            stage = {1: 0, 2: 3, 3: 4, 4: 2, 5: 1}.get((sph >> 10) & 15)
        elif magic == 0x12345679:
            require(program_start == 0x100 and code[4:0x100] == bytes(0xfc),
                    "Invalid FXP compute header")
            stage = 5
        elif magic not in (0x07230203, 0x43425844, 0x44435442, 0x44414853) and program_start == 0 and all(workgroup):
            stage = 5
        else:
            raise ValueError("Unsupported FXP shader code magic")
        require(stage in STAGE_NAMES, "Unsupported FXP shader stage")
        require(stage != 5 or all(workgroup), "Invalid FXP compute workgroup")
        metadata = {"accepted": True, "stage": STAGE_NAMES[stage], "stage_ordinal": stage,
                    "program_start": program_start, "program_length": program_length,
                    "constant_offset": constant_offset, "constant_size": constant_size,
                    "workgroup": workgroup, "code_size": code_size,
                    "code_sha256": hashlib.sha256(code).hexdigest(),
                    "archive_member": member, "archive_record_offset": record}
        key = shaders.shader_key(metadata, code)
        metadata.update(shader_key=key, code_file=key + ".bin")
        previous_end = start + code_size
        cursor = previous_end
        count += 1
        yield metadata, code
    require(count > 0, "FXP package contains no supported shader records")


def extract_archive(archive, output):
    """Extract all unique inputs into a new directory; never replace a cache."""
    archive, output = Path(archive), Path(output)
    require(not output.exists(), "Capture output directory already exists; choose a new directory")
    captures = {}
    for member, data in bsa_members(archive.read_bytes()):
        if not member.lower().endswith(".fxp"):
            continue
        for metadata, code in fxp_captures(data, member):
            captures.setdefault(metadata["shader_key"], (metadata, code))
    require(captures, "Shader archive contains no FXP shader captures")
    # Do not create output until the complete source archive has been validated.
    output.mkdir(parents=True)
    for key in sorted(captures):
        metadata, code = captures[key]
        (output / metadata["code_file"]).write_bytes(code)
        (output / (key + ".json")).write_text(json.dumps(metadata, indent=2) + "\n")
    return sorted(captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        keys = extract_archive(args.archive, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Shader extraction failed: {error}\n")
    print(f"SHADER_ARCHIVE unique={len(keys)} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
