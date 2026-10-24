#!/usr/bin/env python3
"""Translate captured NVN shaders, preserving their original Maxwell programs.

The compiler is the vendored Ryujinx translator in skyhost/tools/dmsh_xlate. Its SPIR-V output
goes through spirv-cross; the resulting MSL and route B reflection are consumed
by skyhost's Metal backend. No game-specific replacement shader is generated.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STAGES = {"vertex": "vs", "fragment": "ps", "compute": "cs"}
ORDINALS = {"vertex": 0, "fragment": 1, "compute": 5}
VERSION = 1


def shader_key(metadata, code):
    """Same explicit little-endian identity as nvn_program.c.

    Control contains mutable runtime state and pointers. Only the validated
    program/constant ranges and compute dimensions affect translation.
    """
    stage = metadata["stage_ordinal"]
    group = metadata["workgroup"] if stage == 5 else [0, 0, 0]
    layout = [stage, metadata["program_start"], metadata["program_length"],
              metadata["constant_offset"], metadata["constant_size"], *group]
    return hashlib.sha256(code + struct.pack("<8I", *layout)).hexdigest()


def resource_slots(handle, cbuf_slot, image=False):
    """Decode Ryujinx TextureHandle offsets into NVN API slots.

    Skyrim uses combined handles in c2 at 0x20 + slot*8. NVN's separate
    texture/sampler and image tables use 0x168/0x568/0x120 respectively.
    The raw handle and cbuf are also retained in reflection for validation.
    """
    a, b, kind = handle & 0x3fff, (handle >> 14) & 0x3fff, handle >> 28
    bank = cbuf_slot & 0xffff if cbuf_slot >= 0 else 2
    sampler_bank = (cbuf_slot >> 16) - 1 if cbuf_slot >= 0 and cbuf_slot >> 16 else bank
    def slot(word, base, actual_bank):
        offset = word * 4 - base
        return offset // 8 if actual_bank == 2 and offset >= 0 and offset % 8 == 0 else None
    if image:
        return slot(a, 0x120, bank), None, kind
    if not kind:
        return slot(a, 0x20, bank), None, kind
    texture = next((slot(x, 0x168, bank) for x in (a, b)
                    if 0x168 <= x * 4 < 0x568), None)
    sampler = next((slot(x, 0x568, sampler_bank) for x in (a, b)
                    if x * 4 >= 0x568), None)
    return texture, sampler, kind


def reflection(metadata, source, resources, constants):
    """Reflect the actual argument-buffer members emitted by spirv-cross."""
    body = re.search(r"struct spvDescriptorSetBuffer0\s*\{(.*?)\};", source, re.S)
    bindings = []
    storage = sorted(resources["sbufs"], key=lambda value: value["binding"])
    stage = STAGES[metadata["stage"]]
    for match in re.finditer(r"^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];",
                             body.group(1) if body else "", re.M):
        msl_type, name, argument = match.groups()
        value = {"msl_name": name, "msl_type": msl_type.strip(), "arg_id": int(argument)}
        cbuf = re.fullmatch(r"[a-z]+_c(\d+)", name)
        sbuf = re.fullmatch(r"[a-z]+_s(\d+)", name)
        texture = re.match(r"[a-z]+_([ti])\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)", name)
        if name == "support_buffer":
            value["kind"] = "support"
        elif cbuf:
            bank = int(cbuf[1])
            if 3 <= bank <= 16:
                value.update(kind="cbuf", reg=bank - 3)
            else:
                value.update(kind="c1" if bank == 1 else "drvcb", bank=bank)
        elif sbuf:
            item = storage[int(sbuf[1])]
            offset = item["cb_offset"] * 4
            base = {"vs": 0x110, "cs": 0x310, "ps": 0x510}[stage]
            if stage == "cs" and item["cb_slot"] == 0 and 0x210 <= offset < 0x310:
                value.update(kind="ubo_ptr", reg=(offset - 0x210) // 16)
            else:
                value.update(kind="sbuf", reg=(offset - base) // 16,
                             rw=bool(item["flags"] & 2))
            value.update(cb_slot=item["cb_slot"], cb_offset=item["cb_offset"])
        elif texture:
            bank = int(texture[2]) if texture[2] else -1
            handle = int(texture[3], 16)
            image = texture[1] == "i"
            tex, samp, kind = resource_slots(handle, bank, image)
            value.update(handle=handle, cbuf_slot=bank)
            if name.endswith("Smplr"):
                value.update(kind="samp" if kind else "samp_tex", reg=samp if kind else tex)
            else:
                value.update(kind="img" if image else "tbuf" if "texture_buffer" in msl_type else "tex", reg=tex)
        else:
            raise ValueError(f"unrecognized MSL resource {name}")
        if "reg" in value and (value["reg"] is None or not 0 <= value["reg"] < 128):
            raise ValueError(f"unresolved NVN slot: {value}")
        bindings.append(value)
    entry = re.search(r"\b(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(", source)
    if not entry:
        raise ValueError("MSL has no vertex/fragment/kernel entry point")
    vertex = [{"location": int(m[3]), "msl_type": m[1]}
              for m in re.finditer(r"(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]", source)]
    return {"schema": VERSION, "route": "B", "stage": stage,
            "key": metadata["shader_key"], "entry": entry[2],
            "driver_texture_base": 32,
            "msl_entry_kind": entry[1], "bindings": bindings,
            "vertex_inputs": vertex, "c1": constants.hex(),
            "threadgroup_size": metadata["workgroup"] if stage == "cs" else None,
            "notes": resources["notes"], "resources": resources}


def load_capture(path):
    metadata = json.loads(path.read_text())
    if not metadata.get("accepted") or metadata["stage"] not in STAGES:
        return None
    code = (path.parent / metadata["code_file"]).read_bytes()
    if len(code) != metadata["code_size"] or hashlib.sha256(code).hexdigest() != metadata["code_sha256"]:
        raise ValueError(f"capture code checksum mismatch: {path}")
    key = shader_key(metadata, code)
    if metadata.get("shader_key") and metadata["shader_key"] != key:
        raise ValueError(f"capture semantic identity mismatch: {path}")
    metadata["shader_key"] = key
    return metadata, code


def compiler_command(args):
    if args.translator:
        path = Path(args.translator).expanduser().resolve()
        return ["dotnet", str(path)] if path.suffix == ".dll" else [str(path)]
    project = ROOT / "skyhost/tools/dmsh_xlate/dmsh_xlate.csproj"
    target = args.output / "compiler"
    subprocess.run(["dotnet", "build", str(project), "-c", "Release", "-o", str(target),
                    "--nologo", "-v", "quiet"], check=True)
    return ["dotnet", str(target / "dmsh_xlate.dll")]


def validate_metal(source, output):
    subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-c", str(source), "-o", str(output)],
                   capture_output=True, text=True, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--translator", help="Existing dmsh_xlate binary or .dll (otherwise build vendored source)")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--limit", type=int, help="Translate the first N unique captures")
    parser.add_argument("--validate-metal", action="store_true", help="Also compile every MSL shader using Apple's compiler")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    unique = {}
    for path in sorted(args.captures.glob("*.json")):
        capture = load_capture(path)
        if capture:
            unique.setdefault(capture[0]["shader_key"], capture)
    captures = list(unique.values())[:args.limit]
    pending = [(m, c) for m, c in captures
               if not (args.output / (m["shader_key"] + ".metal")).exists()
               or not (args.output / (m["shader_key"] + ".json")).exists()]
    print(f"SHADER_BUILD unique={len(unique)} selected={len(captures)} cached={len(captures)-len(pending)} pending={len(pending)}", flush=True)
    if args.validate_metal:
        pending_keys = {m["shader_key"] for m, _ in pending}
        cached = [m for m, _ in captures if m["shader_key"] not in pending_keys]
        with tempfile.TemporaryDirectory(prefix="sky-metal-validation-") as temporary:
            def validate_cached(metadata):
                key = metadata["shader_key"]
                validate_metal(args.output / (key + ".metal"), Path(temporary) / (key + ".air"))
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
                for _ in executor.map(validate_cached, cached):
                    pass
        print(f"SHADER_BUILD metal_validated_cached={len(cached)}", flush=True)
    if not pending:
        return 0
    compiler = compiler_command(args)
    failures = []
    def batch(items):
        completed, failed = 0, []
        with tempfile.TemporaryDirectory(prefix="sky-shaders-") as temporary:
            directory = Path(temporary)
            jobs = []
            for metadata, code in items:
                key = metadata["shader_key"]
                path = directory / (key + ".bin")
                start = metadata["program_start"]
                path.write_bytes(code[start:start + metadata["program_length"]])
                constant = metadata["constant_offset"]
                jobs.append({"key": key, "stage": STAGES[metadata["stage"]], "code": str(path),
                             "c1": code[constant:constant + metadata["constant_size"]].hex(),
                             "local": metadata["workgroup"], "debug": True, "reduced": False})
            job_path = directory / "jobs.json"
            job_path.write_text(json.dumps(jobs))
            translation = subprocess.run([*compiler, str(job_path), str(directory)], capture_output=True, text=True)
            for metadata, code in items:
                key = metadata["shader_key"]
                try:
                    spv = directory / (key + ".spv")
                    if not spv.exists():
                        raise ValueError(translation.stderr.strip() or translation.stdout.strip())
                    msl = directory / (key + ".metal")
                    subprocess.run(["spirv-cross", str(spv), "--msl", "--msl-version", "20300",
                                    "--msl-argument-buffers", "--msl-texture-buffer-native",
                                    "--output", str(msl)], capture_output=True, text=True, check=True)
                    constant = metadata["constant_offset"]
                    refl = reflection(metadata, msl.read_text(), json.loads((directory / (key + ".res.json")).read_text()),
                                      code[constant:constant + metadata["constant_size"]])
                    if args.validate_metal:
                        validate_metal(msl, directory / (key + ".air"))
                    shutil.copyfile(msl, args.output / (key + ".metal"))
                    # Metadata is the completion marker; publish it after the source.
                    temporary_json = args.output / (key + ".json.tmp")
                    temporary_json.write_text(json.dumps(refl, indent=2) + "\n")
                    temporary_json.replace(args.output / (key + ".json"))
                    completed += 1
                except (ValueError, OSError, subprocess.CalledProcessError) as error:
                    detail = error.stderr if isinstance(error, subprocess.CalledProcessError) else str(error)
                    failed.append({"key": key, "stage": metadata["stage"], "error": detail})
        return completed, failed
    batches = [pending[i:i + 32] for i in range(0, len(pending), 32)]
    count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for completed, failed in executor.map(batch, batches):
            count += completed
            failures.extend(failed)
            print(f"SHADER_BUILD translated={count} failed={len(failures)} total={len(pending)}", flush=True)
    (args.output / "failures.json").write_text(json.dumps(failures, indent=2) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
