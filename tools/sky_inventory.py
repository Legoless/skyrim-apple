#!/usr/bin/env python3
"""Inventory Switch NSO modules using only the Python standard library.

Usage: sky_inventory.py <ExeFS directory> <metadata output directory>

NSO decoding and MOD0/dynamic-table parsing follow tools/verify/nsoimg.py.
The LZ4 block decoder is implemented here so importing this tool does not
require the third-party lz4 or capstone packages.

Output schemas (all TSV files have a header row):
  inputs.tsv: filename, kind, bytes, sha256.
  modules.tsv: filename, sha256, build_id (all 32 NSO header bytes), file_bytes,
    stored_segment_bytes, uncompressed_segment_bytes, image_bytes, bss_bytes,
    compressed_segments, export_count, import_count, relocation_count,
    unsupported_relocation_count, dependencies, needed, startup_symbols.
  segments.tsv: module, segment, file_offset, memory_offset, stored_bytes,
    uncompressed_bytes, compressed.
  imports.tsv: module, name, binding, symbol_type, relocation_count,
    relocation_types, provider_modules, resolution. Providers are name matches
    against exports in the other enumerated modules, not DT_NEEDED guesses.
  exports.tsv: module, name, binding, symbol_type, section_index, value, size.
  relocations.tsv: module, table, type_id, type_name, count, loader_supported,
    loader_action. ABS64 and GLOB_DAT are never combined. loader_supported
    reflects skyhost/loader.c: ABS64, GLOB_DAT, JUMP_SLOT, RELATIVE only.
  Patched memory images (<module>.patched.bin or <module>) use their matching
    <module>.fixups.tsv for Build ID and raw segment boundaries, normalize their
    module name to <module>, and are marked as patched-memory-image in inputs.tsv.
    Their dynamic symbols and relocation entries are read from the image bytes,
    not reconstructed from rewrite statistics.
  dependencies.tsv: module, provider_module, resolved_import_count.
  evidence.tsv: module, category, evidence (import/export/string), match, value,
    offset, occurrences, code_xref_count, code_xref_offsets. String evidence
    excludes the dynamic string table. Code xrefs are adjacent AArch64
    ADRP+ADD pairs in the text segment that reference the string or match start.
  startup.tsv: symbol, import_modules, export_modules, string_modules,
    code_xref_modules, loader_available, status.

renderer.txt and startup.txt summarize evidence and limitations. They do not use
RomFS or make claims about RomFS assets. Game-derived output belongs outside Git.
"""

import argparse
from collections import Counter, defaultdict
import csv
from dataclasses import dataclass, field
import hashlib
from pathlib import Path
import re
import struct
import sys


DT_PLTRELSZ = 2
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_NEEDED = 1
DT_PLTREL = 20
DT_JMPREL = 23
DT_HASH = 4
DT_GNU_HASH = 0x6FFFFEF5

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
SUPPORTED_RELOCATIONS = {
    R_AARCH64_ABS64,
    R_AARCH64_GLOB_DAT,
    R_AARCH64_JUMP_SLOT,
    R_AARCH64_RELATIVE,
}

RELOCATION_NAMES = {
    0: "NONE",
    256: "COPY",
    257: "ABS64",
    258: "ABS32",
    259: "ABS16",
    260: "PREL64",
    261: "PREL32",
    262: "PREL16",
    263: "MOVW_UABS_G0",
    264: "MOVW_UABS_G0_NC",
    265: "MOVW_UABS_G1",
    266: "MOVW_UABS_G1_NC",
    267: "MOVW_UABS_G2",
    268: "MOVW_UABS_G2_NC",
    269: "MOVW_UABS_G3",
    270: "MOVW_SABS_G0",
    271: "MOVW_SABS_G1",
    272: "MOVW_SABS_G2",
    273: "LD_PREL_LO19",
    274: "ADR_PREL_LO21",
    275: "ADR_PREL_PG_HI21",
    276: "ADR_PREL_PG_HI21_NC",
    277: "ADD_ABS_LO12_NC",
    278: "LDST8_ABS_LO12_NC",
    279: "TSTBR14",
    280: "CONDBR19",
    282: "JUMP26",
    283: "CALL26",
    284: "LDST16_ABS_LO12_NC",
    285: "LDST32_ABS_LO12_NC",
    286: "LDST64_ABS_LO12_NC",
    299: "LDST128_ABS_LO12_NC",
    300: "MOVW_PREL_G0",
    301: "MOVW_PREL_G0_NC",
    302: "MOVW_PREL_G1",
    303: "MOVW_PREL_G1_NC",
    304: "MOVW_PREL_G2",
    305: "MOVW_PREL_G2_NC",
    306: "MOVW_PREL_G3",
    512: "TLSLD_MOVW_DTPREL_G2",
    513: "TLSLD_MOVW_DTPREL_G1",
    514: "TLSLD_MOVW_DTPREL_G1_NC",
    515: "TLSLD_MOVW_DTPREL_G0",
    516: "TLSLD_MOVW_DTPREL_G0_NC",
    517: "TLSLD_ADD_DTPREL_HI12",
    518: "TLSLD_ADD_DTPREL_LO12",
    519: "TLSLD_ADD_DTPREL_LO12_NC",
    520: "TLSLD_LDST8_DTPREL_LO12",
    521: "TLSLD_LDST8_DTPREL_LO12_NC",
    522: "TLSLD_LDST16_DTPREL_LO12",
    523: "TLSLD_LDST16_DTPREL_LO12_NC",
    524: "TLSLD_LDST32_DTPREL_LO12",
    525: "TLSLD_LDST32_DTPREL_LO12_NC",
    526: "TLSLD_LDST64_DTPREL_LO12",
    527: "TLSLD_LDST64_DTPREL_LO12_NC",
    528: "TLSIE_MOVW_GOTTPREL_G1",
    529: "TLSIE_MOVW_GOTTPREL_G0_NC",
    530: "TLSIE_ADR_GOTTPREL_PAGE21",
    531: "TLSIE_LD64_GOTTPREL_LO12_NC",
    532: "TLSIE_LD_GOTTPREL_PREL19",
    533: "TLSLE_MOVW_TPREL_G2",
    534: "TLSLE_MOVW_TPREL_G1",
    535: "TLSLE_MOVW_TPREL_G1_NC",
    536: "TLSLE_MOVW_TPREL_G0",
    537: "TLSLE_MOVW_TPREL_G0_NC",
    538: "TLSLE_ADD_TPREL_HI12",
    539: "TLSLE_ADD_TPREL_LO12",
    540: "TLSLE_ADD_TPREL_LO12_NC",
    541: "TLSLE_LDST8_TPREL_LO12",
    542: "TLSLE_LDST8_TPREL_LO12_NC",
    543: "TLSLE_LDST16_TPREL_LO12",
    544: "TLSLE_LDST16_TPREL_LO12_NC",
    545: "TLSLE_LDST32_TPREL_LO12",
    546: "TLSLE_LDST32_TPREL_LO12_NC",
    547: "TLSLE_LDST64_TPREL_LO12",
    548: "TLSLE_LDST64_TPREL_LO12_NC",
    549: "TLSLE_LDST128_TPREL_LO12",
    550: "TLSLE_LDST128_TPREL_LO12_NC",
    1024: "TLSDESC",
    1025: "GLOB_DAT",
    1026: "JUMP_SLOT",
    1027: "RELATIVE",
    1032: "IRELATIVE",
}

BINDING_NAMES = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK", 10: "UNIQUE"}
SYMBOL_TYPE_NAMES = {
    0: "NOTYPE",
    1: "OBJECT",
    2: "FUNC",
    3: "SECTION",
    4: "FILE",
    5: "COMMON",
    6: "TLS",
    10: "IFUNC",
}

STARTUP_SYMBOLS = (
    "nnMain",
    "nnosInitialize",
    "nninitInitializeSdkModule",
    "nninitStartup",
    "_init_libc0",
    "_init_libc1",
    "_init_libc2",
    "nvnBootstrapLoader",
)


class InventoryError(ValueError):
    pass


@dataclass(frozen=True)
class EvidenceRule:
    category: str
    label: str
    pattern: str


EVIDENCE_RULES = (
    EvidenceRule("nvn", "nvn-api", r"\bnvn[A-Za-z0-9_]*|_ZNK?2nn3nvn"),
    EvidenceRule("vi", "vi-api", r"\bnn::vi(?:::|\b)|_ZNK?2nn2vi|\bvi(?:Initialize|Open|Close|Set|Get|Display|Layer|NativeWindow)[A-Za-z0-9_]*"),
    EvidenceRule("renderer", "nn-gfx", r"\bnn::gfx(?:::|\b)|_ZNK?2nn3gfx"),
    EvidenceRule("renderer", "bethesda-renderer", r"\bBS(?:Graphics|Render|Shader|LightingShader|Imagespace)[A-Za-z0-9_]*|\bRenderManager\b"),
    EvidenceRule("renderer", "other-graphics-api", r"\b(?:Vulkan|OpenGL|GLES[0-9]*|EGL|Metal|D3D(?:9|11|12)?|DirectX|bgfx|SDL2?|GLFW|AGX)[A-Za-z0-9_]*\b"),
    EvidenceRule("movie", "movie-or-intro", r"\b(?:Bink(?:Video)?|BK2|WebM|Movie(?:Player|Decoder)?|IntroMovie|BGSIntro|BethesdaLogo)[A-Za-z0-9_]*\b|\.(?:bik|bk2)\b|\bnn::movie(?:::|\b)|_ZNK?2nn5movie"),
    EvidenceRule("codec", "codec", r"\b(?:Vorbis|Opus|Ogg|H264|H265|HEVC|AVC|NVDec|AAC|MP3|ADPCM|WAV|hwopus)[A-Za-z0-9_]*\b|\bnn::codec(?:::|\b)|_ZNK?2nn5codec"),
    EvidenceRule("audio", "audio", r"\bnn::audio(?:::|\b)|_ZNK?2nn5audio|\b(?:AudioRenderer|AudioOut|audren|FMOD|Wwise|Miles|XAudio2?|DirectSound)[A-Za-z0-9_]*\b"),
)
COMPILED_RULES = tuple(
    (rule, re.compile(rule.pattern, re.IGNORECASE)) for rule in EVIDENCE_RULES
)
STARTUP_PATTERNS = {
    name: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")
    for name in STARTUP_SYMBOLS
}
PRINTABLE_STRING = re.compile(rb"[\x20-\x7e]{4,}")


@dataclass
class Segment:
    name: str
    file_offset: int
    memory_offset: int
    size: int
    stored_size: int
    compressed: bool


@dataclass
class Symbol:
    index: int
    name: str
    binding: int
    symbol_type: int
    section_index: int
    value: int
    size: int

    @property
    def is_import(self):
        return self.section_index == 0 and bool(self.name)

    @property
    def is_export(self):
        return self.section_index != 0 and self.binding in (1, 2) and bool(self.name)


@dataclass
class Relocation:
    table: str
    offset: int
    type_id: int
    symbol_index: int
    symbol_name: str
    addend: int


@dataclass
class Module:
    path: Path
    filename: str
    sha256: str
    build_id: str
    file_size: int
    image: bytes
    bss_size: int
    segments: list
    needed: list
    symbols: list
    relocations: list
    strtab_offset: int
    strtab_size: int
    stored_segment_bytes: int
    uncompressed_segment_bytes: int

    @property
    def imports(self):
        return [symbol for symbol in self.symbols if symbol.is_import]

    @property
    def exports(self):
        return [symbol for symbol in self.symbols if symbol.is_export]


@dataclass
class Evidence:
    module: str
    category: str
    kind: str
    match: str
    value: str
    offset: int = -1
    occurrences: int = 1
    code_xrefs: set = field(default_factory=set)
    offsets: list = field(default_factory=list)
    targets: set = field(default_factory=set)


def checked_range(image, offset, size, description):
    if offset < 0 or size < 0 or offset + size > len(image):
        raise InventoryError(
            "%s range %#x+%#x is outside image size %#x"
            % (description, offset, size, len(image))
        )


def decompress_lz4_block(data, expected_size):
    """Decode one raw LZ4 block, including overlapping matches."""
    output = bytearray()
    position = 0

    def extended_length(base):
        nonlocal position
        length = base
        if base != 15:
            return length
        while True:
            if position >= len(data):
                raise InventoryError("truncated LZ4 length")
            value = data[position]
            position += 1
            length += value
            if value != 255:
                return length

    while position < len(data):
        token = data[position]
        position += 1
        literal_length = extended_length(token >> 4)
        if position + literal_length > len(data):
            raise InventoryError("truncated LZ4 literals")
        output.extend(data[position:position + literal_length])
        position += literal_length
        if position == len(data):
            break
        if position + 2 > len(data):
            raise InventoryError("truncated LZ4 match offset")
        match_offset = data[position] | (data[position + 1] << 8)
        position += 2
        if match_offset == 0 or match_offset > len(output):
            raise InventoryError("invalid LZ4 match offset")
        match_length = extended_length(token & 0xF) + 4
        if len(output) + match_length > expected_size:
            raise InventoryError("LZ4 output exceeds expected size")
        for _ in range(match_length):
            output.append(output[-match_offset])
    if len(output) != expected_size:
        raise InventoryError(
            "LZ4 output is %d bytes, expected %d" % (len(output), expected_size)
        )
    return bytes(output)


def read_cstring(image, offset, limit, description):
    if offset < 0 or offset >= limit:
        raise InventoryError("%s offset %#x is outside its string table" % (description, offset))
    end = image.find(b"\0", offset, limit)
    if end < 0:
        raise InventoryError("unterminated %s" % description)
    return image[offset:end].decode("utf-8", "replace")


def parse_dynamic(image):
    checked_range(image, 4, 4, "MOD0 pointer")
    mod0 = struct.unpack_from("<I", image, 4)[0]
    checked_range(image, mod0, 24, "MOD0")
    if image[mod0:mod0 + 4] != b"MOD0":
        raise InventoryError("MOD0 magic not found at %#x" % mod0)
    dynamic_offset = mod0 + struct.unpack_from("<i", image, mod0 + 4)[0]
    checked_range(image, dynamic_offset, 16, "dynamic table")
    dynamic = defaultdict(list)
    position = dynamic_offset
    while True:
        checked_range(image, position, 16, "dynamic entry")
        tag, value = struct.unpack_from("<qQ", image, position)
        position += 16
        if tag == 0:
            break
        dynamic[tag].append(value)
    return dynamic


def dynamic_value(dynamic, tag, default=None):
    values = dynamic.get(tag)
    return values[0] if values else default


def gnu_hash_symbol_count(image, offset, symtab, syment):
    checked_range(image, offset, 16, "GNU hash header")
    buckets, symbol_offset, bloom_size, _ = struct.unpack_from("<IIII", image, offset)
    if buckets == 0 or bloom_size == 0:
        raise InventoryError("invalid GNU hash header")
    bucket_offset = offset + 16 + bloom_size * 8
    checked_range(image, bucket_offset, buckets * 4, "GNU hash buckets")
    bucket_values = struct.unpack_from("<%dI" % buckets, image, bucket_offset)
    last = max(bucket_values)
    if last < symbol_offset:
        return symbol_offset
    index = last
    chain_offset = bucket_offset + buckets * 4 + (index - symbol_offset) * 4
    while True:
        checked_range(image, chain_offset, 4, "GNU hash chain")
        value = struct.unpack_from("<I", image, chain_offset)[0]
        index += 1
        chain_offset += 4
        if value & 1:
            break
    checked_range(image, symtab, index * syment, "dynamic symbols")
    return index


def symbol_count(image, dynamic, symtab, strtab, syment):
    if DT_HASH in dynamic:
        offset = dynamic_value(dynamic, DT_HASH)
        checked_range(image, offset, 8, "hash header")
        count = struct.unpack_from("<I", image, offset + 4)[0]
    elif DT_GNU_HASH in dynamic:
        count = gnu_hash_symbol_count(
            image, dynamic_value(dynamic, DT_GNU_HASH), symtab, syment
        )
    elif strtab > symtab and (strtab - symtab) % syment == 0:
        count = (strtab - symtab) // syment
    else:
        raise InventoryError("cannot determine dynamic symbol count")
    if count < 1:
        raise InventoryError("dynamic symbol table is empty")
    checked_range(image, symtab, count * syment, "dynamic symbols")
    return count


def parse_symbols(image, dynamic):
    symtab = dynamic_value(dynamic, DT_SYMTAB)
    strtab = dynamic_value(dynamic, DT_STRTAB)
    strtab_size = dynamic_value(dynamic, DT_STRSZ)
    syment = dynamic_value(dynamic, DT_SYMENT, 24)
    if symtab is None or strtab is None or strtab_size is None:
        raise InventoryError("dynamic table lacks SYMTAB/STRTAB/STRSZ")
    if syment != 24:
        raise InventoryError("unsupported dynamic symbol entry size %d" % syment)
    checked_range(image, strtab, strtab_size, "dynamic string table")
    count = symbol_count(image, dynamic, symtab, strtab, syment)
    symbols = []
    for index in range(count):
        name_offset, info, _, section, value, size = struct.unpack_from(
            "<IBBHQQ", image, symtab + index * syment
        )
        if name_offset >= strtab_size:
            raise InventoryError("symbol %d has an invalid string offset" % index)
        name = read_cstring(
            image, strtab + name_offset, strtab + strtab_size, "symbol name"
        ) if name_offset else ""
        symbols.append(Symbol(
            index=index,
            name=name,
            binding=info >> 4,
            symbol_type=info & 0xF,
            section_index=section,
            value=value,
            size=size,
        ))
    return symbols, strtab, strtab_size


def parse_relocations(image, dynamic, symbols):
    if dynamic_value(dynamic, DT_RELAENT, 24) != 24:
        raise InventoryError("unsupported RELA entry size")
    if dynamic_value(dynamic, DT_PLTREL, DT_RELA) != DT_RELA:
        raise InventoryError("JMPREL is not in RELA format")
    tables = (
        ("RELA", DT_RELA, DT_RELASZ),
        ("JMPREL", DT_JMPREL, DT_PLTRELSZ),
    )
    relocations = []
    for table_name, pointer_tag, size_tag in tables:
        pointer = dynamic_value(dynamic, pointer_tag)
        size = dynamic_value(dynamic, size_tag, 0)
        if pointer is None and not size:
            continue
        if pointer is None or size % 24:
            raise InventoryError("invalid %s table" % table_name)
        checked_range(image, pointer, size, table_name)
        for index in range(size // 24):
            offset, info, addend = struct.unpack_from("<QQq", image, pointer + index * 24)
            type_id = info & 0xFFFFFFFF
            symbol_index = info >> 32
            if symbol_index >= len(symbols):
                raise InventoryError(
                    "%s relocation %d has invalid symbol index %d"
                    % (table_name, index, symbol_index)
                )
            symbol_name = symbols[symbol_index].name if symbol_index else ""
            relocations.append(Relocation(
                table=table_name,
                offset=offset,
                type_id=type_id,
                symbol_index=symbol_index,
                symbol_name=symbol_name,
                addend=addend,
            ))
    return relocations


def module_from_image(path, data, image, build_id, segments, bss_size,
                      stored_segment_bytes, uncompressed_segment_bytes, filename=None):
    dynamic = parse_dynamic(image)
    symbols, strtab, strtab_size = parse_symbols(image, dynamic)
    relocations = parse_relocations(image, dynamic, symbols)
    needed = []
    for offset in dynamic.get(DT_NEEDED, []):
        if offset >= strtab_size:
            raise InventoryError("%s: invalid DT_NEEDED string offset" % path.name)
        needed.append(read_cstring(
            image, strtab + offset, strtab + strtab_size, "DT_NEEDED"
        ))
    return Module(
        path=path,
        filename=filename or path.name,
        sha256=hashlib.sha256(data).hexdigest(),
        build_id=build_id,
        file_size=len(data),
        image=image,
        bss_size=bss_size,
        segments=segments,
        needed=needed,
        symbols=symbols,
        relocations=relocations,
        strtab_offset=strtab,
        strtab_size=strtab_size,
        stored_segment_bytes=stored_segment_bytes,
        uncompressed_segment_bytes=uncompressed_segment_bytes,
    )


def patched_module_name(path):
    name = path.name
    if name.endswith(".patched.bin"):
        return name[:-len(".patched.bin")]
    return name


def patched_fixups_path(path):
    name = patched_module_name(path)
    if not name:
        return None
    return path.with_name(name + ".fixups.tsv")


def parse_patched_module(path):
    module_name = patched_module_name(path)
    fixups_path = patched_fixups_path(path)
    if not module_name or fixups_path is None or not fixups_path.is_file():
        raise InventoryError("%s has no matching fixups.tsv" % path)
    data = path.read_bytes()
    values = {}
    for line in fixups_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# "):
            fields = line[2:].split("\t")
            if len(fields) == 2:
                values[fields[0]] = fields[1]
    required = ("NSO_BUILD_ID", "TEXT_END", "RO_OFF", "DATA_OFF", "PATCH_OFF")
    for name in required:
        if name not in values:
            raise InventoryError("%s: missing %s" % (fixups_path, name))
    build_id = values["NSO_BUILD_ID"]
    if len(build_id) != 64 or any(character not in "0123456789abcdefABCDEF" for character in build_id):
        raise InventoryError("%s: invalid NSO Build ID" % fixups_path)
    try:
        text_end, rodata_offset, data_offset, patch_offset = (
            int(values[name], 0) for name in required[1:]
        )
    except ValueError as error:
        raise InventoryError("%s: invalid segment offset" % fixups_path) from error
    if not 0 < text_end <= rodata_offset <= data_offset <= patch_offset <= len(data):
        raise InventoryError("%s: segment boundaries exceed patched image" % fixups_path)
    segments = [
        Segment("text", 0, 0, text_end, text_end, False),
        Segment("rodata", rodata_offset, rodata_offset,
                data_offset - rodata_offset, data_offset - rodata_offset, False),
        Segment("data", data_offset, data_offset,
                patch_offset - data_offset, patch_offset - data_offset, False),
        Segment("patch", patch_offset, patch_offset,
                len(data) - patch_offset, len(data) - patch_offset, False),
    ]
    return module_from_image(
        path, data, data, build_id, segments, 0, len(data), len(data), filename=module_name
    )


def parse_module(path):
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b"NSO0":
        raise InventoryError("%s is not an NSO0 file" % path)
    flags = struct.unpack_from("<I", data, 0xC)[0]
    bss_size = struct.unpack_from("<I", data, 0x3C)[0]
    compressed_sizes = struct.unpack_from("<III", data, 0x60)
    image_size = None
    segments = []
    decoded = []
    for index, name in enumerate(("text", "rodata", "data")):
        file_offset, memory_offset, size = struct.unpack_from(
            "<III", data, 0x10 + index * 0x10
        )
        compressed = bool(flags & (1 << index))
        stored_size = compressed_sizes[index] if compressed else size
        if file_offset + stored_size > len(data):
            raise InventoryError("%s: %s segment exceeds file size" % (path.name, name))
        raw = data[file_offset:file_offset + stored_size]
        if compressed and size:
            raw = decompress_lz4_block(raw, size)
        elif len(raw) != size:
            raise InventoryError("%s: invalid stored size for %s" % (path.name, name))
        if index == 2:
            image_size = memory_offset + size + bss_size
        segments.append(Segment(
            name=name,
            file_offset=file_offset,
            memory_offset=memory_offset,
            size=size,
            stored_size=stored_size,
            compressed=compressed,
        ))
        decoded.append(raw)
    if image_size is None or image_size < 1:
        raise InventoryError("%s has an empty memory image" % path.name)
    image = bytearray(image_size)
    for segment, raw in zip(segments, decoded):
        checked_range(image, segment.memory_offset, segment.size, segment.name)
        image[segment.memory_offset:segment.memory_offset + segment.size] = raw
    return module_from_image(
        path, data, bytes(image), data[0x40:0x60].hex(), segments, bss_size,
        sum(segment.stored_size for segment in segments),
        sum(segment.size for segment in segments),
    )


def relocation_name(type_id):
    return RELOCATION_NAMES.get(type_id, "UNKNOWN_%d" % type_id)


def loader_action(type_id):
    if type_id == R_AARCH64_RELATIVE:
        return "module_base + addend"
    if type_id == R_AARCH64_JUMP_SLOT:
        return "resolved symbol; addend ignored"
    if type_id == R_AARCH64_ABS64:
        return "resolved symbol + addend (ABS64)"
    if type_id == R_AARCH64_GLOB_DAT:
        return "resolved symbol + addend (GLOB_DAT)"
    return "unsupported; loader.c generic fall-through is not valid support"


def relocation_rows(modules):
    rows = []
    for module in modules:
        counts = Counter((item.table, item.type_id) for item in module.relocations)
        for (table, type_id), count in sorted(counts.items()):
            rows.append({
                "module": module.filename,
                "table": table,
                "type_id": str(type_id),
                "type_name": relocation_name(type_id),
                "count": str(count),
                "loader_supported": "yes" if type_id in SUPPORTED_RELOCATIONS else "NO",
                "loader_action": loader_action(type_id),
            })
    return rows


def export_map(modules):
    result = defaultdict(list)
    for module in modules:
        for symbol in module.exports:
            result[symbol.name].append(module.filename)
    return result


def dependency_rows(modules):
    providers = export_map(modules)
    counts = Counter()
    for module in modules:
        for symbol in module.imports:
            for provider in providers.get(symbol.name, []):
                if provider != module.filename:
                    counts[(module.filename, provider)] += 1
    return [{
        "module": module,
        "provider_module": provider,
        "resolved_import_count": str(count),
    } for (module, provider), count in sorted(counts.items())]


def module_dependencies(module, providers):
    dependencies = set()
    for symbol in module.imports:
        for provider in providers.get(symbol.name, []):
            if provider != module.filename:
                dependencies.add(provider)
    return sorted(dependencies)


def clean_field(value):
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def write_tsv(path, fieldnames, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: clean_field(row.get(key, "")) for key in fieldnames})


def symbol_rows(modules):
    providers = export_map(modules)
    imports = []
    exports = []
    for module in modules:
        relocation_counts = Counter(item.symbol_name for item in module.relocations if item.symbol_name)
        relocation_types = defaultdict(set)
        for item in module.relocations:
            if item.symbol_name:
                relocation_types[item.symbol_name].add(relocation_name(item.type_id))
        for symbol in module.symbols:
            common = {
                "module": module.filename,
                "name": symbol.name,
                "binding": BINDING_NAMES.get(symbol.binding, str(symbol.binding)),
                "symbol_type": SYMBOL_TYPE_NAMES.get(symbol.symbol_type, str(symbol.symbol_type)),
            }
            if symbol.is_import:
                matched = sorted(providers.get(symbol.name, []))
                imports.append({
                    **common,
                    "relocation_count": str(relocation_counts[symbol.name]),
                    "relocation_types": ";".join(sorted(relocation_types[symbol.name])),
                    "provider_modules": ";".join(matched),
                    "resolution": "resolved" if matched else "unresolved",
                })
            if symbol.is_export:
                exports.append({
                    **common,
                    "section_index": str(symbol.section_index),
                    "value": "%#x" % symbol.value,
                    "size": str(symbol.size),
                })
    imports.sort(key=lambda row: (row["module"], row["name"]))
    exports.sort(key=lambda row: (row["module"], row["name"]))
    return imports, exports


def add_symbol_evidence(evidence, module):
    for symbol in module.symbols:
        if not (symbol.is_import or symbol.is_export):
            continue
        kind = "import" if symbol.is_import else "export"
        matches = [(name, "startup") for name in STARTUP_SYMBOLS if symbol.name == name]
        matches.extend(
            (rule.label, rule.category)
            for rule, regex in COMPILED_RULES if regex.search(symbol.name)
        )
        for match, category in matches:
            evidence.append(Evidence(
                module=module.filename,
                category=category,
                kind=kind,
                match=match,
                value=symbol.name,
                offset=symbol.value if symbol.is_export else -1,
            ))


def string_excerpt(text, start, end, limit=180):
    left = max(0, start - (limit - (end - start)) // 2)
    right = min(len(text), left + limit)
    left = max(0, right - limit)
    return ("..." if left else "") + text[left:right] + ("..." if right < len(text) else "")


def add_string_evidence(module, records):
    targets = set()
    strtab_start = module.strtab_offset
    strtab_end = strtab_start + module.strtab_size
    for found in PRINTABLE_STRING.finditer(module.image):
        if found.start() >= strtab_start and found.end() <= strtab_end:
            continue
        text = found.group().decode("ascii")
        matches = []
        for name, regex in STARTUP_PATTERNS.items():
            match = regex.search(text)
            if match:
                matches.append(("startup", name, match))
        for rule, regex in COMPILED_RULES:
            match = regex.search(text)
            if match:
                matches.append((rule.category, rule.label, match))
        for category, label, match in matches:
            excerpt = string_excerpt(text, match.start(), match.end())
            key = (module.filename, category, label, excerpt)
            record = records.get(key)
            match_offset = found.start() + match.start()
            if record is None:
                record = Evidence(
                    module=module.filename,
                    category=category,
                    kind="string",
                    match=label,
                    value=excerpt,
                    offset=match_offset,
                    occurrences=0,
                )
                records[key] = record
            record.occurrences += 1
            record.offsets.append(match_offset)
            record.targets.update((found.start(), match_offset))
            targets.update((found.start(), match_offset))
    return targets


def find_code_xrefs(image, text_offset, text_size, targets):
    """Return adjacent ADRP+ADD references to the requested image offsets."""
    result = defaultdict(list)
    if not targets:
        return result
    checked_range(image, text_offset, text_size, "text")
    text_end = text_offset + text_size
    for offset in range(text_offset, text_end - 7, 4):
        adrp, add = struct.unpack_from("<II", image, offset)
        if adrp & 0x9F000000 != 0x90000000:
            continue
        register = adrp & 31
        if add & 0xFF000000 != 0x91000000 or (add >> 5) & 31 != register:
            continue
        immediate = (((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        page = (offset & ~0xFFF) + (immediate << 12)
        immediate12 = (add >> 10) & 0xFFF
        if (add >> 22) & 1:
            immediate12 <<= 12
        target = page + immediate12
        if target in targets:
            result[target].append(offset)
    return result


def collect_evidence(modules):
    evidence = []
    for module in modules:
        add_symbol_evidence(evidence, module)
    string_records = {}
    for module in modules:
        targets = add_string_evidence(module, string_records)
        text = module.segments[0]
        xrefs = find_code_xrefs(module.image, text.memory_offset, text.size, targets)
        module_records = [record for key, record in string_records.items() if key[0] == module.filename]
        for record in module_records:
            for target in record.targets:
                record.code_xrefs.update(xrefs.get(target, []))
    evidence.extend(string_records.values())
    evidence.sort(key=lambda row: (
        row.module, row.category, row.kind, row.match, row.value, row.offset
    ))
    return evidence


def evidence_rows(evidence):
    return [{
        "module": row.module,
        "category": row.category,
        "evidence": row.kind,
        "match": row.match,
        "value": row.value,
        "offset": "%#x" % row.offset if row.offset >= 0 else "",
        "occurrences": str(row.occurrences),
        "code_xref_count": str(len(row.code_xrefs)) if row.kind == "string" else "",
        "code_xref_offsets": ";".join("%#x" % offset for offset in sorted(row.code_xrefs)),
    } for row in evidence]


def startup_rows(modules, evidence):
    rows = []
    for name in STARTUP_SYMBOLS:
        imports = sorted(module.filename for module in modules for symbol in module.imports if symbol.name == name)
        exports = sorted(module.filename for module in modules for symbol in module.exports if symbol.name == name)
        related = [row for row in evidence if row.kind == "string" and row.match == name]
        strings = sorted(set(row.module for row in related))
        xrefs = sorted(set(row.module for row in related if row.code_xrefs))
        if exports:
            status = "exported+imported" if imports else "exported"
        elif imports:
            status = "imported-only-unresolved"
        elif xrefs:
            status = "string+code-xref-only"
        elif strings:
            status = "string-only"
        else:
            status = "absent-from-inventory"
        rows.append({
            "symbol": name,
            "import_modules": ";".join(imports),
            "export_modules": ";".join(exports),
            "string_modules": ";".join(strings),
            "code_xref_modules": ";".join(xrefs),
            "loader_available": "yes" if exports else "no",
            "status": status,
        })
    return rows


def format_module_list(modules):
    return ",".join(modules) if modules else "none"


def append_evidence_summary(lines, title, rows, limit=30):
    lines.append(title)
    if not rows:
        lines.append("  none matched")
        return
    for row in rows[:limit]:
        location = " offset=%s" % ("%#x" % row.offset if row.offset >= 0 else "n/a")
        xrefs = " code_xrefs=%d" % len(row.code_xrefs) if row.kind == "string" else ""
        lines.append("  %s %s %s: %s%s%s" % (
            row.module, row.kind, row.match, row.value, location, xrefs
        ))
    if len(rows) > limit:
        lines.append("  ... %d additional rows; see evidence.tsv" % (len(rows) - limit))


def write_renderer_report(path, modules, evidence):
    dynamic = [row for row in evidence if row.kind in ("import", "export") and row.category in ("nvn", "vi", "renderer")]
    strings = [row for row in evidence if row.kind == "string" and row.category in ("nvn", "vi", "renderer")]
    media = [row for row in evidence if row.category in ("movie", "audio", "codec")]
    lines = [
        "Stage-2 renderer and intro-related evidence",
        "Scope: %d NSO modules. RomFS was not inspected; no RomFS asset conclusion is made." % len(modules),
        "",
    ]
    append_evidence_summary(lines, "NVN/VI/renderer dynamic-symbol evidence:", dynamic)
    lines.append("")
    append_evidence_summary(lines, "NVN/VI/renderer non-dynamic-string evidence:", strings)
    nvn = [row for row in evidence if row.category == "nvn"]
    lines.extend(["", "Renderer assessment:"])
    if any(row.value == "nvnBootstrapLoader" and row.kind in ("import", "export") for row in nvn):
        lines.append("  nvnBootstrapLoader linkage is concrete evidence of an NVN bootstrap interface.")
        lines.append("  It does not enumerate functions obtained indirectly by name or prove the complete renderer architecture.")
    elif nvn:
        lines.append("  NVN-related evidence exists, but its presence alone does not prove an active rendering path.")
    else:
        lines.append("  No NVN evidence was matched. This is not proof of no NVN: stripped/static code and indirect lookup remain possible.")
    lines.append("  The renderer is not definitively identified by symbol absence or by this inventory alone.")
    lines.append("  Alternative-API and Bethesda-renderer matches are lexical/symbol evidence, not proof that the API is used for final rendering.")
    lines.append("  String code-xref scanning covers only adjacent ADRP+ADD pairs in text; no xref is not evidence of no use.")
    lines.append("")
    append_evidence_summary(lines, "Intro/movie/audio/codec evidence:", media)
    lines.extend([
        "",
        "Intro assessment: these matches do not establish which clip plays at startup or whether playback is reached.",
        "Without RomFS, movie files, container contents, and asset-level codec use remain unknown.",
        "Absence of matched media symbols/strings would likewise not rule out stripped or statically linked playback code.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_startup_report(path, exefs, modules, relocs, startup):
    lines = [
        "Stage-2 module and startup inventory",
        "ExeFS: %s" % exefs,
        "NSO modules: %d" % len(modules),
        "Build IDs below contain all 32 bytes from the NSO header.",
        "",
        "Modules:",
    ]
    providers = export_map(modules)
    for module in modules:
        dependencies = module_dependencies(module, providers)
        compressed = ",".join(segment.name for segment in module.segments if segment.compressed) or "none"
        lines.append("  %s sha256=%s" % (module.filename, module.sha256))
        lines.append("    build_id=%s" % module.build_id)
        lines.append("    file=%d stored_segments=%d uncompressed_segments=%d image=%d bss=%d compressed=%s" % (
            module.file_size, module.stored_segment_bytes,
            module.uncompressed_segment_bytes, len(module.image), module.bss_size, compressed
        ))
        lines.append("    imports=%d exports=%d relocations=%d dependencies=%s needed=%s" % (
            len(module.imports), len(module.exports), len(module.relocations),
            format_module_list(dependencies), format_module_list(module.needed)
        ))
    lines.extend(["", "Startup symbols (loader_available means an enumerated module exports the exact name):"])
    for row in startup:
        lines.append("  %s: %s; imports=%s exports=%s strings=%s code_xrefs=%s loader_available=%s" % (
            row["symbol"], row["status"],
            format_module_list(row["import_modules"].split(";") if row["import_modules"] else []),
            format_module_list(row["export_modules"].split(";") if row["export_modules"] else []),
            format_module_list(row["string_modules"].split(";") if row["string_modules"] else []),
            format_module_list(row["code_xref_modules"].split(";") if row["code_xref_modules"] else []),
            row["loader_available"],
        ))
    unsupported = [row for row in relocs if row["loader_supported"] == "NO"]
    lines.extend(["", "Relocation support against skyhost/loader.c:"])
    for row in relocs:
        lines.append("  %s %s %s(%s): count=%s supported=%s action=%s" % (
            row["module"], row["table"], row["type_name"], row["type_id"],
            row["count"], row["loader_supported"], row["loader_action"]
        ))
    if unsupported:
        lines.append("Relocation blockers:")
        for row in unsupported:
            lines.append("  %s: %d %s relocation(s) have no loader implementation." % (
                row["module"], int(row["count"]), row["type_name"]
            ))
        lines.append("  The loader's generic fall-through must not be treated as support for these types.")
    else:
        lines.append("Relocation blockers: none among the observed RELA/JMPREL entries.")
    lines.append("ABS64 and GLOB_DAT are reported separately even though their loader paths share symbol-plus-addend arithmetic.")
    lines.append("Cross-module dependencies are inferred from exact undefined-import/export name matches; DT_NEEDED is reported separately when present.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def inventory(exefs, output):
    exefs = Path(exefs)
    output = Path(output)
    if not exefs.is_dir():
        raise InventoryError("ExeFS directory not found: %s" % exefs)
    files = sorted(path for path in exefs.iterdir() if path.is_file())
    input_rows = []
    modules = []
    seen_module_names = set()
    for path in files:
        data = path.read_bytes()
        is_nso = data[:4] == b"NSO0"
        fixups = patched_fixups_path(path)
        is_patched = not is_nso and fixups is not None and fixups.is_file()
        kind = ("NSO0" if is_nso else "patched-memory-image" if is_patched
                else "NPDM" if path.suffix.lower() == ".npdm" else "other")
        input_rows.append({
            "filename": path.name,
            "kind": kind,
            "bytes": str(len(data)),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        module = None
        if is_nso:
            module = parse_module(path)
        elif is_patched:
            module = parse_patched_module(path)
        if module is not None:
            if module.filename in seen_module_names:
                raise InventoryError("duplicate module %s in %s" % (module.filename, exefs))
            seen_module_names.add(module.filename)
            modules.append(module)
    if not modules:
        raise InventoryError("no NSO0 modules found in %s" % exefs)
    output.mkdir(parents=True, exist_ok=True)

    imports, exports = symbol_rows(modules)
    relocs = relocation_rows(modules)
    dependencies = dependency_rows(modules)
    providers = export_map(modules)
    module_rows = []
    segment_rows = []
    for module in modules:
        unsupported = sum(
            1 for item in module.relocations if item.type_id not in SUPPORTED_RELOCATIONS
        )
        startup = sorted(set(
            symbol.name for symbol in module.symbols
            if symbol.name in STARTUP_SYMBOLS and (symbol.is_import or symbol.is_export)
        ))
        module_rows.append({
            "filename": module.filename,
            "sha256": module.sha256,
            "build_id": module.build_id,
            "file_bytes": str(module.file_size),
            "stored_segment_bytes": str(module.stored_segment_bytes),
            "uncompressed_segment_bytes": str(module.uncompressed_segment_bytes),
            "image_bytes": str(len(module.image)),
            "bss_bytes": str(module.bss_size),
            "compressed_segments": ";".join(segment.name for segment in module.segments if segment.compressed),
            "export_count": str(len(module.exports)),
            "import_count": str(len(module.imports)),
            "relocation_count": str(len(module.relocations)),
            "unsupported_relocation_count": str(unsupported),
            "dependencies": ";".join(module_dependencies(module, providers)),
            "needed": ";".join(module.needed),
            "startup_symbols": ";".join(startup),
        })
        for segment in module.segments:
            segment_rows.append({
                "module": module.filename,
                "segment": segment.name,
                "file_offset": "%#x" % segment.file_offset,
                "memory_offset": "%#x" % segment.memory_offset,
                "stored_bytes": str(segment.stored_size),
                "uncompressed_bytes": str(segment.size),
                "compressed": "yes" if segment.compressed else "no",
            })

    evidence = collect_evidence(modules)
    startup = startup_rows(modules, evidence)
    write_tsv(output / "inputs.tsv", ("filename", "kind", "bytes", "sha256"), input_rows)
    write_tsv(output / "modules.tsv", (
        "filename", "sha256", "build_id", "file_bytes", "stored_segment_bytes",
        "uncompressed_segment_bytes", "image_bytes", "bss_bytes", "compressed_segments",
        "export_count", "import_count", "relocation_count", "unsupported_relocation_count",
        "dependencies", "needed", "startup_symbols",
    ), module_rows)
    write_tsv(output / "segments.tsv", (
        "module", "segment", "file_offset", "memory_offset", "stored_bytes",
        "uncompressed_bytes", "compressed",
    ), segment_rows)
    write_tsv(output / "imports.tsv", (
        "module", "name", "binding", "symbol_type", "relocation_count",
        "relocation_types", "provider_modules", "resolution",
    ), imports)
    write_tsv(output / "exports.tsv", (
        "module", "name", "binding", "symbol_type", "section_index", "value", "size",
    ), exports)
    write_tsv(output / "relocations.tsv", (
        "module", "table", "type_id", "type_name", "count", "loader_supported", "loader_action",
    ), relocs)
    write_tsv(output / "dependencies.tsv", (
        "module", "provider_module", "resolved_import_count",
    ), dependencies)
    write_tsv(output / "evidence.tsv", (
        "module", "category", "evidence", "match", "value", "offset", "occurrences",
        "code_xref_count", "code_xref_offsets",
    ), evidence_rows(evidence))
    write_tsv(output / "startup.tsv", (
        "symbol", "import_modules", "export_modules", "string_modules",
        "code_xref_modules", "loader_available", "status",
    ), startup)
    write_startup_report(output / "startup.txt", exefs, modules, relocs, startup)
    write_renderer_report(output / "renderer.txt", modules, evidence)
    return modules, relocs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("exefs", help="directory containing extracted ExeFS files")
    parser.add_argument("metadata", help="output directory for TSV and report files")
    args = parser.parse_args(argv)
    try:
        modules, relocs = inventory(args.exefs, args.metadata)
    except (InventoryError, OSError, struct.error) as error:
        print("[sky_inventory] error: %s" % error, file=sys.stderr)
        return 1
    unsupported = [row for row in relocs if row["loader_supported"] == "NO"]
    for module in modules:
        print("[sky_inventory] %s: build_id=%s imports=%d exports=%d relocations=%d" % (
            module.filename, module.build_id, len(module.imports), len(module.exports),
            len(module.relocations),
        ))
    if unsupported:
        for row in unsupported:
            print("[sky_inventory] UNSUPPORTED %s %s %s count=%s" % (
                row["module"], row["table"], row["type_name"], row["count"],
            ), file=sys.stderr)
    print("[sky_inventory] wrote %d modules to %s" % (len(modules), Path(args.metadata)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
