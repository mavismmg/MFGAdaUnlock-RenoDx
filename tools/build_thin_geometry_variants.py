#!/usr/bin/env python3
"""Build opt-in thin-geometry cubin variants from an installed DLSS-G provider.

No NVIDIA payload is stored in source control.  This tool reads the provider
already installed on the developer machine, applies narrowly scoped PTX edits,
assembles them for sm_89 and emits the generated header used by an experimental
local build.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile


FATBIN_MAGIC = 0xBA55ED50
PTX_KIND = 1
CUBIN_KIND = 2
ADA_ARCH = 89
BLACKWELL_ARCH = 120


def pe_sections(path: Path) -> tuple[bytes, list[tuple[int, int, int, str]]]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: invalid PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + optional_size
    sections = []
    for index in range(count):
        offset = first + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, _, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((raw_offset, max(virtual_size, raw_size), index, name))
    return data, sections


def iter_fatbins(data: bytes, sections: list[tuple[int, int, int, str]]):
    for raw_offset, size, _, name in sections:
        if name != ".data":
            continue
        blob = data[raw_offset:raw_offset + size]
        magic = struct.pack("<I", FATBIN_MAGIC)
        cursor = 0
        while True:
            relative = blob.find(magic, cursor)
            if relative < 0:
                break
            start = raw_offset + relative
            if start + 16 <= len(data):
                header = struct.unpack_from("<H", data, start + 6)[0]
                payload = struct.unpack_from("<Q", data, start + 8)[0]
                end = start + 16 + payload
                if header == 16 and 0 < payload < 4 << 20 and end <= len(data):
                    yield start, end
            cursor = relative + 4


def iter_entries(data: bytes, start: int, end: int):
    cursor = start + 16
    while cursor + 64 <= end:
        kind = struct.unpack_from("<H", data, cursor)[0]
        header = struct.unpack_from("<I", data, cursor + 4)[0]
        payload = struct.unpack_from("<Q", data, cursor + 8)[0]
        compressed = struct.unpack_from("<I", data, cursor + 16)[0]
        arch = struct.unpack_from("<I", data, cursor + 28)[0]
        raw = struct.unpack_from("<Q", data, cursor + 56)[0]
        if not 64 <= header <= 256 or payload > end - cursor - header:
            return
        yield kind, arch, cursor + header, int(payload), compressed, int(raw)
        cursor += header + payload


def lz4_decompress(data: bytes, expected: int) -> bytes:
    output = bytearray()
    cursor = 0

    def extended_length(value: int) -> int:
        nonlocal cursor
        if value == 15:
            while True:
                if cursor >= len(data):
                    raise ValueError("truncated LZ4 length")
                extra = data[cursor]
                cursor += 1
                value += extra
                if extra != 255:
                    break
        return value

    while cursor < len(data):
        token = data[cursor]
        cursor += 1
        literals = extended_length(token >> 4)
        if cursor + literals > len(data):
            raise ValueError("truncated LZ4 literal run")
        output.extend(data[cursor:cursor + literals])
        cursor += literals
        if cursor == len(data):
            break
        if cursor + 2 > len(data):
            raise ValueError("truncated LZ4 back-reference")
        distance = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        count = extended_length(token & 15) + 4
        if distance == 0 or distance > len(output):
            raise ValueError("invalid LZ4 back-reference")
        for _ in range(count):
            output.append(output[-distance])
        if len(output) > expected:
            raise ValueError("LZ4 output exceeds declared size")
    if len(output) != expected:
        raise ValueError(f"LZ4 output is {len(output)} bytes, expected {expected}")
    return bytes(output)


def fingerprint_elf(blob: bytes) -> tuple[int, int, int]:
    if len(blob) < 0x40 or blob[:4] != b"\x7fELF":
        raise ValueError("not an ELF cubin")
    section_offset = struct.unpack_from("<Q", blob, 0x28)[0]
    section_size, section_count, string_index = struct.unpack_from("<HHH", blob, 0x3A)
    if section_size < 0x40 or string_index >= section_count:
        raise ValueError("invalid ELF section table")
    string_header = section_offset + string_index * section_size
    strings = struct.unpack_from("<Q", blob, string_header + 0x18)[0]
    text = shared = registers = 0
    for index in range(section_count):
        section = section_offset + index * section_size
        name_offset = struct.unpack_from("<I", blob, section)[0]
        end = blob.find(b"\0", strings + name_offset)
        name = blob[strings + name_offset:end]
        size = struct.unpack_from("<Q", blob, section + 0x20)[0]
        info = struct.unpack_from("<I", blob, section + 0x2C)[0]
        if name.startswith(b".text."):
            text = size
            registers = (info >> 24) & 0xFF
        elif name.startswith(b".nv.shared"):
            shared = size
    if not text:
        raise ValueError("cubin has no text section")
    return int(text), int(shared), int(registers)


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"{label}: found {count} instances, expected exactly one")
    return source.replace(old, new, 1)


def patch_previous_scatter(source: str) -> str:
    anchor = "ld.param.f32 %f24, [%rd6+60];\n"
    return replace_once(
        source,
        anchor,
        anchor + "mul.ftz.f32 %f24, %f24, 0f3F000000; // thin-geometry previous scatter\n",
        "previous scatter divisor load",
    )


def patch_intermediate_scatter(source: str) -> str:
    anchor = "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n"
    return replace_once(
        source,
        anchor,
        anchor + "mul.ftz.f32 %f2, %f2, 0f3F000000; // thin-geometry intermediate scatter\n",
        "intermediate scatter divisor load",
    )


def patch_validated_warp_blend(source: str) -> str:
    """Apply an independently authored conservative warp-validation experiment.

    Tony Joaca/DLSSG-Transfusion identified BlendCandidatesFused as a useful quality
    point.  This variant keeps that mechanism separate from scatter retention,
    retains the provider's bounds/sentinel/finite checks and uses a gradual
    0.85..1.0 confidence floor instead of forcing every accepted warp to 1.0.
    """
    declarations = ".reg .pred %p<260>;\n"
    extra_declarations = (
        declarations
        + ".reg .pred %qv<7>;\n"
        + ".reg .f32 %qf<12>;\n"
    )
    source = replace_once(source, declarations, extra_declarations, "blend register declaration")

    anchor = "ld.param.u8 %rs8, [%rd6+220];\n"
    program = """// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
"""
    return replace_once(source, anchor, program + anchor, "blend UIR insertion point")


PATCHERS = {
    "Kernel_EstimatePrev2CurrScatter": ("previous_scatter", patch_previous_scatter, ADA_ARCH),
    "Kernel_EstimateIntermMvecsScatter": ("intermediate_scatter", patch_intermediate_scatter, BLACKWELL_ARCH),
    "Kernel_BlendCandidatesFused": ("validated_warp_blend", patch_validated_warp_blend, BLACKWELL_ARCH),
}


def compile_ptx(ptxas: Path, source: str, directory: Path, name: str) -> bytes:
    source_path = directory / f"{name}.ptx"
    cubin_path = directory / f"{name}.cubin"
    source_path.write_text(source, encoding="ascii", newline="\n")
    result = subprocess.run(
        [str(ptxas), "-arch=sm_89", "-O3", str(source_path), "-o", str(cubin_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(f"ptxas failed for {name}:\n{result.stderr}")
    return cubin_path.read_bytes()


def extract_kernels(provider: Path):
    data, sections = pe_sections(provider)
    for start, end in iter_fatbins(data, sections):
        entries = list(iter_entries(data, start, end))
        sources: dict[int, str] = {}
        ada_cubin = None
        for kind, arch, offset, size, compressed, raw in entries:
            if kind == PTX_KIND and arch in (ADA_ARCH, BLACKWELL_ARCH):
                payload = data[offset:offset + compressed]
                decoded = lz4_decompress(payload, raw) if compressed else data[offset:offset + size]
                sources[arch] = decoded.replace(b"\r", b"").rstrip(b"\0").decode("ascii")
            elif kind == CUBIN_KIND and arch == ADA_ARCH and not compressed:
                ada_cubin = data[offset:offset + size]
        if ada_cubin is None or not sources:
            continue
        names = set()
        for source in sources.values():
            match = re.search(r"\.entry\s+([A-Za-z0-9_]+)\s*\(", source)
            if match:
                names.add(match.group(1))
        if len(names) == 1:
            yield names.pop(), sources, ada_cubin


def emit_header(records: list[dict], output: Path, providers: list[Path]) -> None:
    lines = [
        "// Generated by tools/build_thin_geometry_variants.py -- do not edit, do not commit.",
        "// Built from locally installed NVIDIA providers; no payload belongs in source control.",
        "// providers: " + ", ".join(path.name for path in providers),
        "",
        "#pragma once",
        "",
        "struct CubinVariant {",
        "  unsigned source_text;",
        "  unsigned source_shared;",
        "  unsigned source_regs;",
        "  unsigned slot_size;",
        "  unsigned long long source_fnv1a64;",
        "  unsigned size;",
        "  const unsigned char* data;",
        "  const char* mechanism;",
        "};",
        "",
    ]
    for index, record in enumerate(records):
        lines.append(f"static const unsigned char kThinGeometryCubin{index}[] = {{")
        blob = record["replacement"]
        for offset in range(0, len(blob), 16):
            lines.append("  " + "".join(f"0x{byte:02x}," for byte in blob[offset:offset + 16]))
        lines.extend(["};", ""])
    lines.append("static const CubinVariant kThinGeometryCubins[] = {")
    for index, record in enumerate(records):
        text, shared, registers = record["source_fingerprint"]
        lines.append(
            f"  {{{text}u, {shared}u, {registers}u, {record['slot_size']}u, "
            f"0x{record['source_hash']:016x}ull, sizeof(kThinGeometryCubin{index}), "
            f"kThinGeometryCubin{index}, \"{record['mechanism']}\"}},"
        )
    lines.extend(["};", ""])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="ascii", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("provider", nargs="+", type=Path)
    parser.add_argument("--ptxas", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--inspect", action="store_true")
    args = parser.parse_args()

    records = []
    seen = set()
    with tempfile.TemporaryDirectory(prefix="mfg-thin-geometry-") as temp:
        directory = Path(temp)
        for provider in args.provider:
            print(f"provider: {provider}")
            for name, sources, ada_cubin in extract_kernels(provider):
                if name not in PATCHERS:
                    continue
                mechanism, patcher, source_arch = PATCHERS[name]
                if source_arch not in sources:
                    raise ValueError(f"{name}: missing sm_{source_arch} PTX")
                source = sources[source_arch]
                if args.inspect:
                    print(f"  {name}: sm_{source_arch}, {len(source)} PTX bytes")
                    if name != "Kernel_BlendCandidatesFused":
                        for line_number, line in enumerate(source.splitlines(), 1):
                            if "+60]" in line or "+120]" in line:
                                print(f"    {line_number:04d}: {line}")
                baseline_source = source.replace(".target sm_120", ".target sm_89")
                patched_source = patcher(baseline_source)
                baseline = (
                    ada_cubin
                    if source_arch == ADA_ARCH
                    else compile_ptx(args.ptxas, baseline_source, directory, mechanism + "_baseline")
                )
                replacement = compile_ptx(args.ptxas, patched_source, directory, mechanism)
                if len(baseline) > len(ada_cubin):
                    print(f"  skip {mechanism}: baseline {len(baseline)} > slot {len(ada_cubin)}")
                    continue
                if len(replacement) > len(ada_cubin):
                    print(f"  skip {mechanism}: replacement {len(replacement)} > slot {len(ada_cubin)}")
                    continue
                key = (mechanism, fingerprint_elf(ada_cubin), len(ada_cubin), fnv1a64(ada_cubin))
                if key in seen:
                    continue
                seen.add(key)
                record = {
                    "mechanism": mechanism,
                    "source_fingerprint": fingerprint_elf(ada_cubin),
                    "slot_size": len(ada_cubin),
                    "source_hash": fnv1a64(ada_cubin),
                    "replacement": replacement,
                }
                records.append(record)
                print(
                    f"  ok {mechanism}: fp={record['source_fingerprint']} "
                    f"slot={len(ada_cubin)} replacement={len(replacement)} "
                    f"source-fnv={record['source_hash']:016x}"
                )
    if not records:
        raise SystemExit("no supported experimental variants were generated")
    emit_header(records, args.output, args.provider)
    print(f"wrote {args.output} ({len(records)} variants)")


if __name__ == "__main__":
    main()
