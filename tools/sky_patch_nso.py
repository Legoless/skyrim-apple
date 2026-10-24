#!/usr/bin/env python3
"""Rewrite a Switch AArch64 NSO for native execution on Apple systems.

Usage: sky_patch_nso.py <input NSO> <output directory> <module-name> [--control-nacp PATH]

Only instruction rewrites discovered from the input are applied. There are no
per-game cuts, observation gates, parameter gates, build-ID checks, or rule
selection environment variables.
"""

import argparse
import bisect
import hashlib
from pathlib import Path
import re
import struct
import sys
from itertools import combinations

import lz4.block
from capstone import Cs, CsError, CS_ARCH_ARM64, CS_MODE_ARM


class RewriteError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise RewriteError(message)


def checked_range(data, offset, size, description):
    require(offset >= 0 and size >= 0 and offset + size <= len(data),
            f"{description} range {offset:#x}+{size:#x} exceeds {len(data):#x}")


def u32(data, offset, description="u32"):
    checked_range(data, offset, 4, description)
    return struct.unpack_from("<I", data, offset)[0]


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def decode_nso(data):
    require(len(data) >= 0x80 and data[:4] == b"NSO0", "input is not a complete NSO0 file")
    flags = struct.unpack_from("<I", data, 0xC)[0]
    segments = [struct.unpack_from("<III", data, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from("<I", data, 0x3C)[0]
    compressed_sizes = struct.unpack_from("<III", data, 0x60)
    image_size = segments[2][1] + segments[2][2] + bss
    require(image_size > 0, "NSO has an empty memory image")
    require(segments[0][1] == 0, f"unsupported text memory offset {segments[0][1]:#x}")
    require(segments[0][2] > 0 and segments[0][2] % 4 == 0,
            f"invalid text size {segments[0][2]:#x}")

    occupied = []
    for name, (_, memory_offset, size) in zip(("text", "rodata", "data"), segments):
        require(memory_offset + size <= image_size,
                f"{name} range {memory_offset:#x}+{size:#x} exceeds image {image_size:#x}")
        if size:
            occupied.append((memory_offset, memory_offset + size, name))
    for index, (start, end, name) in enumerate(occupied):
        for other_start, other_end, other_name in occupied[index + 1:]:
            require(end <= other_start or other_end <= start,
                    f"overlapping NSO segments {name} and {other_name}")

    image = bytearray(image_size)
    for index, (file_offset, memory_offset, size) in enumerate(segments):
        compressed = bool(flags & (1 << index))
        stored_size = compressed_sizes[index] if compressed else size
        checked_range(data, file_offset, stored_size, f"segment {index}")
        raw = data[file_offset:file_offset + stored_size]
        if compressed and size:
            try:
                raw = lz4.block.decompress(raw, uncompressed_size=size)
            except lz4.block.LZ4BlockError as error:
                raise RewriteError(f"segment {index} LZ4 decoding failed: {error}") from error
        elif compressed:
            require(not raw, f"compressed empty segment {index} has stored bytes")
        require(len(raw) == size,
                f"segment {index} decoded to {len(raw):#x} bytes, expected {size:#x}")
        image[memory_offset:memory_offset + size] = raw
    return image, segments


def function_starts(image, text_end):
    mod0 = u32(image, 4, "MOD0 pointer")
    checked_range(image, mod0, 24, "MOD0")
    require(image[mod0:mod0 + 4] == b"MOD0", f"MOD0 magic missing at {mod0:#x}")
    eh_header = mod0 + struct.unpack_from("<i", image, mod0 + 16)[0]
    checked_range(image, eh_header, 4, "eh_frame_hdr")
    version, pointer_encoding, count_encoding, table_encoding = image[eh_header:eh_header + 4]
    require(version == 1, f"unsupported eh_frame_hdr version {version}")

    def read_encoded(offset, encoding):
        require(encoding != 0xFF, f"omitted eh_frame_hdr pointer at {offset:#x}")
        fmt, application = encoding & 0xF, encoding & 0x70
        formats = {
            0x3: ("<I", 4),
            0xB: ("<i", 4),
            0x4: ("<Q", 8),
        }
        require(fmt in formats, f"unsupported eh_frame_hdr format {fmt:#x} at {offset:#x}")
        require(application in (0, 0x10, 0x30),
                f"unsupported eh_frame_hdr application {application:#x} at {offset:#x}")
        fmt_string, size = formats[fmt]
        checked_range(image, offset, size, "eh_frame_hdr value")
        value = struct.unpack_from(fmt_string, image, offset)[0]
        if application == 0x10:
            value += offset
        elif application == 0x30:
            value += eh_header
        return value, size

    offset = eh_header + 4
    _, size = read_encoded(offset, pointer_encoding)
    offset += size
    fde_count, size = read_encoded(offset, count_encoding)
    offset += size
    require(fde_count >= 0, "negative eh_frame_hdr entry count")
    starts = set()
    for index in range(fde_count):
        start, size = read_encoded(offset, table_encoding)
        offset += size
        _, size = read_encoded(offset, table_encoding)
        offset += size
        require(start >= 0 and start % 4 == 0,
                f"invalid eh_frame function start {start:#x} at entry {index}")
        if start < text_end:
            starts.add(start)
    return sorted(starts)


KEY_INDEX = {
    name: index for index, name in enumerate((
        "K18", "KTLS", "KTLR", "KGATE",
        "KRES0", "KRES1", "KASPECT", "KASPECTUI",
    ))
}


def read_control_nacp(path):
    data = Path(path).read_bytes()
    require(len(data) >= 0x4000, "control NACP must contain at least 0x4000 bytes")
    offsets = {
        "SAVE_DATA_SIZE": 0x3080,
        "SAVE_JOURNAL_SIZE": 0x3088,
        "SAVE_DATA_MAX": 0x3148,
        "SAVE_JOURNAL_MAX": 0x3150,
    }
    values = {key: struct.unpack_from("<q", data, offset)[0]
              for key, offset in offsets.items()}
    for key, value in values.items():
        require(value >= 0, f"control NACP {key} must not be negative")
    require(values["SAVE_DATA_SIZE"] > 0, "control NACP SAVE_DATA_SIZE must be positive")
    for initial, maximum in (("SAVE_DATA_SIZE", "SAVE_DATA_MAX"),
                             ("SAVE_JOURNAL_SIZE", "SAVE_JOURNAL_MAX")):
        if not values[maximum]:
            values[maximum] = values[initial]
        require(values[maximum] >= values[initial],
                f"control NACP {maximum} is smaller than {initial}")
    require(values["SAVE_DATA_MAX"] <= (1 << 63) - 1 - values["SAVE_JOURNAL_MAX"],
            "control NACP combined save maxima exceed INT64_MAX")
    return values


def rewrite(source, output_directory, name, control_nacp=None):
    require(re.fullmatch(r"[A-Za-z0-9_-]+", name or "") is not None,
            f"unsafe module name {name!r}")
    require(control_nacp is None or name == "main", "--control-nacp is only valid for main")
    data = Path(source).read_bytes()
    image, segments = decode_nso(data)
    save_metadata = read_control_nacp(control_nacp) if control_nacp is not None else {}
    text_end = segments[0][2]
    ro_offset = segments[1][1]
    data_offset = segments[2][1]
    key_page_offset = align(len(image), 0x4000)
    patch_offset = align(len(image) + 0x4000, 0x10000)

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    patch = bytearray()
    slot_references = []
    gates = []
    stats = {}

    def stat(kind):
        stats[kind] = stats.get(kind, 0) + 1

    def here():
        return patch_offset + len(patch)

    def emit(word, kind=None):
        require(isinstance(word, int) and 0 <= word <= 0xFFFFFFFF,
                f"invalid emitted instruction word {word!r}")
        if kind:
            slot_references.append((len(patch), kind))
        patch.extend(struct.pack("<I", word))

    def register(number):
        require(isinstance(number, int) and 0 <= number <= 31, f"invalid register {number!r}")
        return number

    def B(source_address, target_address):
        offset = target_address - source_address
        require(-(1 << 27) <= offset < (1 << 27) and offset % 4 == 0,
                f"branch out of range {source_address:#x} -> {target_address:#x}")
        return 0x14000000 | ((offset >> 2) & 0x3FFFFFF)

    def pair(opcode, first, second, base, offset):
        register(first)
        register(second)
        register(base)
        require(offset % 8 == 0 and -512 <= offset <= 504,
                f"invalid pair stack offset {offset}")
        return opcode | (((offset // 8) & 0x7F) << 15) | (second << 10) | (base << 5) | first

    def STP(first, second, base, offset):
        return pair(0xA9000000, first, second, base, offset)

    def LDP(first, second, base, offset):
        return pair(0xA9400000, first, second, base, offset)

    def STP_PRE(first, second, base, offset):
        return pair(0xA9800000, first, second, base, offset)

    def LDP_POST(first, second, base, offset):
        return pair(0xA8C00000, first, second, base, offset)

    def STUR(target, base, offset):
        register(target)
        register(base)
        require(-256 <= offset <= 255, f"invalid STUR offset {offset}")
        return 0xF8000000 | ((offset & 0x1FF) << 12) | (base << 5) | target

    def LDUR(target, base, offset):
        register(target)
        register(base)
        require(-256 <= offset <= 255, f"invalid LDUR offset {offset}")
        return 0xF8400000 | ((offset & 0x1FF) << 12) | (base << 5) | target

    def LDRX(target, base, index=0):
        register(target)
        register(base)
        require(0 <= index <= 0xFFF, f"invalid LDR index {index}")
        return 0xF9400000 | (index << 10) | (base << 5) | target

    def STRX(target, base, index=0):
        register(target)
        register(base)
        require(0 <= index <= 0xFFF, f"invalid STR index {index}")
        return 0xF9000000 | (index << 10) | (base << 5) | target

    def MRS_TPIDRRO(target):
        register(target)
        require(target != 31, "TPIDRRO read needs a real destination register")
        return 0xD53BD060 | target

    def ADRP(destination, pc, target):
        register(destination)
        require(destination != 31, "ADRP needs a real destination register")
        immediate = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
        require(-(1 << 20) <= immediate < (1 << 20),
                f"ADRP out of range {pc:#x} -> {target:#x}")
        return 0x90000000 | ((immediate & 3) << 29) | (((immediate >> 2) & 0x7FFFF) << 5) | destination

    def ADDI(destination, source_register, immediate, shift=0):
        register(destination)
        register(source_register)
        require(0 <= immediate <= 0xFFF and shift in (0, 1),
                f"invalid ADD immediate {immediate:#x} shift {shift}")
        return 0x91000000 | (shift << 22) | (immediate << 10) | (source_register << 5) | destination

    def ADDR(destination, source_register, other):
        register(destination)
        register(source_register)
        register(other)
        return 0x8B000000 | (other << 16) | (source_register << 5) | destination

    def SUBI(destination, source_register, immediate, shift=0):
        register(destination)
        register(source_register)
        require(0 <= immediate <= 0xFFF and shift in (0, 1),
                f"invalid SUB immediate {immediate:#x} shift {shift}")
        return 0xD1000000 | (shift << 22) | (immediate << 10) | (source_register << 5) | destination

    def BR(target_register):
        register(target_register)
        return 0xD61F0000 | (target_register << 5)

    def BLR(target_register):
        register(target_register)
        return 0xD63F0000 | (target_register << 5)

    NOP = 0xD503201F

    def dis(word, address):
        instructions = list(md.disasm(struct.pack("<I", word), address))
        require(len(instructions) == 1 and instructions[0].size == 4,
                f"cannot decode instruction {word:#010x} at {address:#x}")
        return instructions[0]

    def regset(instruction):
        try:
            read, written = instruction.regs_access()
        except CsError as error:
            raise RewriteError(f"cannot inspect registers at {instruction.address:#x}: {error}") from error
        return {instruction.reg_name(item) for item in read}, {instruction.reg_name(item) for item in written}

    def num(register_name):
        match = re.fullmatch(r"[xw](\d+)", register_name or "")
        return int(match.group(1)) if match else None

    def has18(instruction):
        return re.search(r"\b[xw]18\b", instruction.op_str) is not None

    def regs_in(instruction):
        result = {int(item) for item in re.findall(r"\b[xw](\d+)\b", instruction.op_str)}
        try:
            read, written = regset(instruction)
            result |= {num(item) for item in read | written} - {None}
        except RewriteError:
            pass
        return result

    def pick(avoid, count=2):
        result = [item for item in range(0, 16) if item not in avoid and item != 18][:count]
        require(len(result) == count, f"not enough scratch registers; avoid={sorted(avoid)}")
        return result

    def subst(instruction, substitute):
        word = struct.unpack("<I", instruction.bytes)[0]
        wanted = re.sub(r"\b([xw])18\b", lambda match: f"{match.group(1)}{substitute}", instruction.op_str)
        positions = [position for position in (0, 5, 10, 16) if (word >> position) & 31 == 18]
        require(positions, f"cannot locate x18 fields at {instruction.address:#x}")
        for count in range(len(positions), 0, -1):
            for candidates in combinations(positions, count):
                new_word = word
                for position in candidates:
                    new_word = (new_word & ~(31 << position)) | (substitute << position)
                try:
                    decoded = dis(new_word, instruction.address)
                except RewriteError:
                    continue
                if decoded.mnemonic == instruction.mnemonic and decoded.op_str == wanted:
                    return new_word
        raise RewriteError(
            f"unsupported x18 substitution at {instruction.address:#x}: "
            f"{instruction.mnemonic} {instruction.op_str}"
        )

    def emit_key_addr(scratch, kind):
        slot_references.append((len(patch), kind))
        emit(ADRP(scratch, here(), key_page_offset))
        emit(LDRX(scratch, scratch, KEY_INDEX[kind]))

    def emit_slot_addr(base, key_register, kind):
        emit_key_addr(key_register, kind)
        emit(ADDR(base, base, key_register))

    def sp_delta(instruction):
        match = re.search(r"\[sp, #(-?(?:0x)?[0-9a-f]+)\]!", instruction.op_str)
        if match:
            return int(match.group(1), 0)
        match = re.search(r"\[sp\], #(-?(?:0x)?[0-9a-f]+)$", instruction.op_str)
        if match:
            return int(match.group(1), 0)
        require(not re.match(r"(sp|wsp),", instruction.op_str),
                f"unsupported explicit sp write at {instruction.address:#x}: {instruction.op_str}")
        return 0

    def site_plain(instruction):
        substitute, temporary = pick(regs_in(instruction))
        delta = sp_delta(instruction)
        save = -16 if delta == 0 else -112
        require(delta == 0 or delta >= -96,
                f"unsupported sp delta {delta} at {instruction.address:#x}")
        start = here()
        emit(STP(substitute, temporary, 31, save))
        emit(MRS_TPIDRRO(temporary))
        emit_slot_addr(temporary, substitute, "K18")
        emit(LDRX(substitute, temporary))
        emit(subst(instruction, substitute))
        emit(STRX(substitute, temporary))
        emit(LDP(substitute, temporary, 31, save - delta))
        emit(B(here(), instruction.address + 4))
        return start

    def site_cond(instruction):
        word = struct.unpack("<I", instruction.bytes)[0]
        substitute, temporary = pick(regs_in(instruction))
        target = instruction.operands[-1].imm
        start = here()
        emit(STP(substitute, temporary, 31, -16))
        emit(MRS_TPIDRRO(temporary))
        emit_slot_addr(temporary, substitute, "K18")
        emit(LDRX(substitute, temporary))
        mnemonic = instruction.mnemonic
        taken = here() + 12
        offset = (taken - here()) >> 2
        if mnemonic in ("cbz", "cbnz"):
            new_word = (word & 0xFF000000) | ((offset & 0x7FFFF) << 5) | substitute
        else:
            new_word = (word & 0xFFF80000) | ((offset & 0x3FFF) << 5) | substitute
        pc = here()
        emit(new_word)
        decoded = dis(new_word, pc)
        require(decoded.mnemonic == mnemonic and decoded.operands[-1].imm == taken,
                f"conditional rewrite failed at {instruction.address:#x}")
        emit(LDP(substitute, temporary, 31, -16))
        emit(B(here(), instruction.address + 4))
        require(here() == taken, f"conditional trampoline layout mismatch at {instruction.address:#x}")
        emit(LDP(substitute, temporary, 31, -16))
        emit(B(here(), target))
        return start

    def site_pcrel(instruction):
        target = instruction.operands[1].imm
        substitute, temporary = pick(set())
        start = here()
        emit(STP(substitute, temporary, 31, -16))
        emit(MRS_TPIDRRO(temporary))
        emit_slot_addr(temporary, substitute, "K18")
        pc = here()
        emit(ADRP(substitute, pc, target))
        if instruction.mnemonic == "adr":
            emit(ADDI(substitute, substitute, target & 0xFFF))
        emit(STRX(substitute, temporary))
        emit(LDP(substitute, temporary, 31, -16))
        emit(B(here(), instruction.address + 4))
        return start

    def site_br(instruction, base, count):
        require(base % 4 == 0 and 0 <= base < text_end and count > 0,
                f"invalid jump table at {instruction.address:#x}: base={base:#x} count={count}")
        require(base + count * 4 <= text_end,
                f"jump table exceeds text at {instruction.address:#x}: base={base:#x} count={count}")
        start = here()
        emit(STUR(16, 31, -16))
        emit(STUR(17, 31, -24))
        emit(MRS_TPIDRRO(16))
        emit_slot_addr(16, 17, "K18")
        emit(LDRX(16, 16))
        emit(LDUR(17, 31, -24))
        pad_base = start + 4 * 24
        delta = pad_base - base
        operation = ADDI if delta >= 0 else SUBI
        remaining = abs(delta)
        operations = []
        high, low = remaining >> 12, remaining & 0xFFF
        while high:
            amount = min(high, 0xFFF)
            operations.append(operation(16, 16, amount, 1))
            high -= amount
        if low:
            operations.append(operation(16, 16, low, 0))
        require(len(operations) <= 12,
                f"jump-table trampoline too large at {instruction.address:#x}")
        for operation_word in operations:
            emit(operation_word)
        emit(BR(16))
        while here() < pad_base:
            emit(NOP)
        tails = pad_base + 4 * count
        for index in range(count):
            emit(B(here(), tails + 8 * index))
        for index in range(count):
            emit(LDUR(16, 31, -16))
            emit(B(here(), base + 4 * index))
        return start

    def site_sysreg_read(instruction, kind):
        destination = instruction.bytes[0] & 31
        require(destination != 31,
                f"unsupported system-register read to xzr at {instruction.address:#x}")
        temporary = 16 if destination != 16 else 17
        start = here()
        emit(STUR(temporary, 31, -16))
        emit(MRS_TPIDRRO(destination))
        emit_slot_addr(destination, temporary, kind)
        emit(LDRX(destination, destination))
        emit(LDUR(temporary, 31, -16))
        emit(B(here(), instruction.address + 4))
        return start

    def site_tpidr_write(instruction):
        source_register = struct.unpack("<I", instruction.bytes)[0] & 31
        temporary, = pick({source_register}, 1)
        start = here()
        emit(STUR(temporary, 31, -16))
        key_register = next(item for item in (16, 17) if item not in (source_register, temporary))
        emit(STUR(key_register, 31, -24))
        emit(MRS_TPIDRRO(temporary))
        emit_slot_addr(temporary, key_register, "KTLS")
        emit(STRX(source_register, temporary))
        emit(LDUR(key_register, 31, -24))
        emit(LDUR(temporary, 31, -16))
        emit(B(here(), instruction.address + 4))
        return start

    def emit_gate_call():
        emit(MRS_TPIDRRO(30))
        emit_slot_addr(30, 29, "KGATE")
        emit(LDRX(30, 30))
        emit(LDRX(29, 31))

    def site_gate(instruction, kind, argument):
        start = here()
        emit(STP_PRE(29, 30, 31, -16))
        emit_gate_call()
        emit(BLR(30))
        return_address = here()
        emit(LDP_POST(29, 30, 31, 16))
        emit(B(here(), instruction.address + 4))
        gates.append((return_address, kind, argument, instruction.address))
        return start

    EXCLUSIVE_LOADS = ("ldxr", "ldaxr", "ldxp", "ldaxp", "ldxrb", "ldaxrb", "ldxrh", "ldaxrh")
    EXCLUSIVE_STORES = ("stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh")

    def find_windows(instructions):
        result = []
        for index, instruction in enumerate(instructions):
            if instruction.mnemonic not in EXCLUSIVE_LOADS:
                continue
            end = index
            while end < len(instructions) and instructions[end].mnemonic not in EXCLUSIVE_STORES:
                end += 1
            if end >= len(instructions) or end - index >= 16:
                continue
            if end + 1 < len(instructions) and instructions[end + 1].mnemonic in ("cbz", "cbnz"):
                end += 1
            segment = instructions[index:end + 1]
            if any(has18(item) for item in segment):
                result.append(segment)
        return result

    def site_window(segment):
        used = set()
        for instruction in segment:
            used |= regs_in(instruction)
        substitute, temporary = pick(used)
        low, high = segment[0].address, segment[-1].address + 4
        start = here()
        emit(STP(substitute, temporary, 31, -16))
        emit(MRS_TPIDRRO(temporary))
        emit_slot_addr(temporary, substitute, "K18")
        emit(LDRX(substitute, temporary))
        copy_start = here()
        exits = copy_start + 4 * len(segment)
        external_targets = []
        words = []
        for index, instruction in enumerate(segment):
            word = struct.unpack("<I", instruction.bytes)[0]
            if has18(instruction):
                word = subst(instruction, substitute)
            mnemonic = instruction.mnemonic
            if mnemonic in ("cbz", "cbnz", "tbz", "tbnz") or mnemonic.startswith("b."):
                target = instruction.operands[-1].imm
                if low <= target < high:
                    new_target = copy_start + (target - low)
                else:
                    external_targets.append(target)
                    new_target = exits + 16 * len(external_targets)
                pc = copy_start + 4 * index
                delta = new_target - pc
                require(delta % 4 == 0, f"unaligned exclusive-window branch at {instruction.address:#x}")
                offset = delta >> 2
                if mnemonic in ("tbz", "tbnz"):
                    require(-(1 << 13) <= offset < (1 << 13),
                            f"exclusive-window TB branch out of range at {instruction.address:#x}")
                    word = (word & 0xFFF8001F) | ((offset & 0x3FFF) << 5)
                else:
                    require(-(1 << 18) <= offset < (1 << 18),
                            f"exclusive-window branch out of range at {instruction.address:#x}")
                    word = (word & 0xFF00001F) | ((offset & 0x7FFFF) << 5)
                decoded = dis(word, pc)
                require(decoded.operands[-1].imm == new_target and decoded.mnemonic == mnemonic,
                        f"exclusive-window branch rewrite failed at {instruction.address:#x}")
            else:
                require(mnemonic not in ("adr", "adrp", "b", "bl", "br", "blr", "ret")
                        and not (mnemonic == "ldr" and "[" not in instruction.op_str),
                        f"unsupported instruction in exclusive window at {instruction.address:#x}: "
                        f"{mnemonic} {instruction.op_str}")
            words.append(word)
        for word in words:
            emit(word)
        for target in [high] + external_targets:
            emit(STRX(substitute, temporary))
            emit(LDP(substitute, temporary, 31, -16))
            emit(B(here(), target))
            emit(NOP)
        return start, low, high

    starts = function_starts(image, text_end)
    kinds = {}
    for offset in range(0, text_end, 4):
        word = struct.unpack_from("<I", image, offset)[0]
        if word & 0xFFE0001F == 0xD4000001:
            kinds[offset] = ("svc", (word >> 5) & 0xFFFF)
            continue
        if word & 0xFFFFFFE0 == 0xD53BD040:
            kinds[offset] = ("tpidr_r", None)
            continue
        if word & 0xFFFFFFE0 == 0xD51BD040:
            kinds[offset] = ("tpidr_w", None)
            continue
        if word & 0xFFFFFFE0 == 0xD53BD060:
            kinds[offset] = ("tpidrro_r", None)
            continue
        if word & 0xFFFFFFE0 == 0xD53BE020:
            kinds[offset] = ("cntpct", word & 31)
            continue
        if word & 0xFFFFFFE0 == 0xD53BE040:
            kinds[offset] = ("cntvct", word & 31)
            continue
        for instruction in md.disasm(image[offset:offset + 4], offset):
            if has18(instruction):
                kinds[offset] = ("x18", None)

    def function_of(address):
        index = bisect.bisect_right(starts, address) - 1
        if index >= 0:
            return starts[index], starts[index + 1] if index + 1 < len(starts) else text_end
        return address, address + 4

    done = set()
    windows = []
    jumps = {}
    functions = sorted({function_of(address) for address, (kind, _) in kinds.items() if kind == "x18"})
    for function_start, function_end in functions:
        checked_range(image, function_start, function_end - function_start, "function")
        instructions = list(md.disasm(bytes(image[function_start:function_end]), function_start))
        function_windows = find_windows(instructions)
        for segment in function_windows:
            windows.append(segment)
            done |= {instruction.address for instruction in segment}
        for index, instruction in enumerate(instructions):
            if instruction.mnemonic == "br" and instruction.op_str == "x18":
                base = width = None
                context = instructions[max(0, index - 8):index]
                for previous in reversed(context):
                    if previous.mnemonic == "adr" and previous.op_str.startswith("x18,"):
                        base = previous.operands[1].imm
                        break
                for previous in reversed(context):
                    if (previous.mnemonic == "add" and previous.op_str.startswith("x18, x18,")
                            and "lsl #2" in previous.op_str):
                        index_register = num(previous.reg_name(previous.operands[2].reg))
                        for candidate in reversed(context):
                            if candidate.mnemonic in ("ldrb", "ldrsb") and num(candidate.reg_name(candidate.operands[0].reg)) == index_register:
                                width = 256
                            if candidate.mnemonic in ("ldrh", "ldrsh") and num(candidate.reg_name(candidate.operands[0].reg)) == index_register:
                                width = 65536
                            if width:
                                break
                        break
                require(base is not None and width is not None,
                        f"unsupported jump table at {instruction.address:#x}: {instruction.mnemonic} {instruction.op_str}")
                jumps[instruction.address] = (base, width)
        inside = {instruction.address for segment in function_windows for instruction in segment}
        heads = {segment[0].address for segment in function_windows}
        for instruction in instructions:
            if (instruction.mnemonic in ("b", "bl", "cbz", "cbnz", "tbz", "tbnz")
                    or instruction.mnemonic.startswith("b.")):
                target = instruction.operands[-1].imm
                require(target not in inside or target in heads,
                        f"branch into exclusive window at {instruction.address:#x} -> {target:#x}")

    for segment in windows:
        for instruction in segment:
            kind = kinds.get(instruction.address)
            require(kind is None or kind[0] == "x18",
                    f"exclusive window contains another rewrite site at {instruction.address:#x}: {kind}")
    for address, (kind, _) in kinds.items():
        if kind != "x18" or address in done:
            continue
        instruction = dis(struct.unpack_from("<I", image, address)[0], address)
        require(instruction.mnemonic not in EXCLUSIVE_LOADS + EXCLUSIVE_STORES,
                f"unhandled x18 exclusive instruction at {address:#x}: "
                f"{instruction.mnemonic} {instruction.op_str}")

    def put_branch(address, target):
        struct.pack_into("<I", image, address, B(address, target))

    for segment in windows:
        start, _, _ = site_window(segment)
        stat("window")
        put_branch(segment[0].address, start)
    for address in sorted(kinds):
        if address in done:
            continue
        kind, argument = kinds[address]
        instruction = dis(struct.unpack_from("<I", image, address)[0], address)
        if kind == "x18":
            mnemonic = instruction.mnemonic
            if mnemonic == "br":
                require(address in jumps, f"unsupported jump table at {address:#x}")
                start = site_br(instruction, *jumps[address])
                stat("br")
            elif mnemonic in ("cbz", "cbnz", "tbz", "tbnz"):
                start = site_cond(instruction)
                stat("cond")
            elif mnemonic in ("adr", "adrp"):
                start = site_pcrel(instruction)
                stat("pcrel")
            elif mnemonic in ("blr", "ret", "b", "bl") or (mnemonic == "ldr" and "[" not in instruction.op_str):
                raise RewriteError(
                    f"unsupported x18 instruction at {address:#x}: {mnemonic} {instruction.op_str}"
                )
            else:
                start = site_plain(instruction)
                stat("plain")
        elif kind == "tpidr_r":
            start = site_sysreg_read(instruction, "KTLS")
            stat(kind)
        elif kind == "tpidrro_r":
            start = site_sysreg_read(instruction, "KTLR")
            stat(kind)
        elif kind == "tpidr_w":
            start = site_tpidr_write(instruction)
            stat(kind)
        elif kind == "svc":
            start = site_gate(instruction, "svc", argument)
            stat(kind)
        elif kind in ("cntpct", "cntvct"):
            require(argument != 31, f"unsupported counter read to xzr at {address:#x}")
            start = site_gate(instruction, kind, argument)
            stat(kind)
        else:
            raise RewriteError(f"internal unsupported site kind {kind!r} at {address:#x}")
        put_branch(address, start)

    patched = image + bytearray(patch_offset - len(image)) + patch
    fixups = [
        f"# NSO_BUILD_ID\t{data[0x40:0x60].hex()}\n",
        f"# PATCH_OFF\t{patch_offset:#x}\n",
        f"# KEY_PAGE_OFF\t{key_page_offset:#x}\n",
        "# KEY_PAGE_SIZE\t0x4000\n",
        f"# TEXT_END\t{text_end:#x}\n",
        f"# RO_OFF\t{ro_offset:#x}\n",
        f"# DATA_OFF\t{data_offset:#x}\n",
    ]
    fixups.extend(f"# KEY_{key}_IDX\t{index}\n" for key, index in KEY_INDEX.items())
    fixups.extend(f"# {key}\t{value:#x}\n" for key, value in save_metadata.items())
    gate_lines = [f"{return_address:#x}\t{kind}\t{argument}\t{original:#x}\n"
                  for return_address, kind, argument, original in gates]
    site_lines = []
    for address in sorted(kinds):
        if address not in done:
            site_lines.append(f"{address:#x}\t{kinds[address][0]}\n")
    for segment in windows:
        site_lines.append(f"{segment[0].address:#x}\tW\t{segment[-1].address + 4:#x}\n")
    for address, (base, count) in jumps.items():
        site_lines.append(f"{address:#x}\tJ\t{base:#x}\t{count}\n")

    source_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    outputs = {
        f"{name}.patched.bin": bytes(patched),
        f"{name}.fixups.tsv": "".join(fixups).encode(),
        f"{name}.gates.tsv": "".join(gate_lines).encode(),
        f"{name}.sites.tsv": "".join(site_lines).encode(),
        f"{name}.cuts.tsv": b"",
        f"{name}.obs.tsv": b"",
        f"{name}.params.tsv": b"",
        "build_src.sha256": f"{source_hash}  sky_patch_nso.py\n".encode(),
    }
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    for filename, contents in outputs.items():
        (output / filename).write_bytes(contents)
    print(name, stats, "cuts", 0, "obs", 0, "patch", len(patch),
          "PATCH_OFF", hex(patch_offset), "slot_refs", len(slot_references), "gates", len(gates))
    return stats


def main(argv=None):
    arguments = sys.argv if argv is None else argv
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path, help="input NSO")
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("module_name")
    parser.add_argument("--control-nacp", type=Path,
                        help="read the main module's save capacities from its Control NACP")
    options = parser.parse_args(arguments[1:])
    try:
        rewrite(options.source, options.output_directory, options.module_name, options.control_nacp)
    except (RewriteError, OSError, struct.error, CsError) as error:
        print(f"[sky_patch_nso] error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
