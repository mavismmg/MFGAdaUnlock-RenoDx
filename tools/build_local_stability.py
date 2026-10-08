"""Offline local-stability release: isolated cubin table and actual C++ warp audit."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from build_thin_geometry_variants import (extract_kernels, load_generated_records,
    emit_header, patch_adaptive_geometry_v31_local, patch_adaptive_blend_v3,
    fingerprint_elf, fnv1a64, CudaDriverCompiler)


def diagonal_consensus(source):
    local = patch_adaptive_geometry_v31_local(source)
    pair = ("min.f32 %qgf6, %qgf6, %qgf11;\n"
            "mul.f32 %qgf6, %qgf6, %qgf8;\n")
    update = ("min.f32 %qgf7, %qgf0, %qgf6;\n"
              "max.f32 %qgf1, %qgf1, %qgf7;\n"
              "max.f32 %qgf0, %qgf0, %qgf6;\n")
    assert local.count(pair + update) == 2
    # Each of the two independently tested diagonal sides contributes the
    # pair minimum. Neither can exceed the weaker side or bypass depth gates.
    return local.replace(pair + update, pair + update + update)


def accesses(source):
    return [line.strip() for line in source.splitlines()
            if re.search(r"\b(?:ld\.|st\.|tex\.|suld\.|sust\.)", line)]


def assemble(source, stem, args, registers):
    ptx = args.output / (stem + ".ptx")
    cubin = ptx.with_suffix(".cubin")
    ptx.write_text(source, encoding="ascii")
    result = subprocess.run([str(args.ptxas), "-arch=sm_89", "-O3", "-v",
        "--warn-on-spills", "--override-directive-values", f"-maxrregcount={registers}",
        str(ptx), "-o", str(cubin)], capture_output=True, text=True, check=True)
    diagnostic = result.stdout + result.stderr
    (args.output / (stem + ".ptxas.txt")).write_text(diagnostic)
    assert re.search(r"0 bytes stack frame, 0 bytes spill stores, 0 bytes spill loads", diagnostic), diagnostic
    payload = cubin.read_bytes()
    text, shared, regs = fingerprint_elf(payload)
    assert regs <= registers
    sass = subprocess.check_output([str(args.nvdisasm), str(cubin)], text=True)
    assert not re.search(r"\b(?:LDL|STL)\b", sass)
    (args.output / (stem + ".sass")).write_text(sass)
    return payload, dict(text=text, shared=shared, registers=regs,
        sha256=hashlib.sha256(payload).hexdigest(), stack=0, spills=0, local=0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--provider", type=Path, required=True)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--header", type=Path, required=True)
    p.add_argument("--emitter", type=Path, required=True)
    p.add_argument("--ptxas", type=Path, required=True)
    p.add_argument("--nvdisasm", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--variant", choices=["control", "border-axis-candidate", "release-1.4.3"], default="control")
    args = p.parse_args()
    assert args.header.resolve() != args.baseline.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    records = load_generated_records(args.baseline)
    report = {}
    for name, sources, ada in extract_kernels(args.provider):
        if name == "Kernel_EstimateIntermMvecsScatter":
            source = sources[120].replace(".target sm_120", ".target sm_89")
            baseline = patch_adaptive_geometry_v31_local(source)
            patched = diagonal_consensus(source)
            assert accesses(patched) == accesses(baseline)
            payload, stats = assemble(patched, "geometry-paired-diagonals", args, 40)
            assert stats["shared"] == 7776
            # Two extra best/second updates add one 128-byte code block.
            # Keep the occupancy/storage limits; permit this measured local-stability size.
            assert stats["text"] <= 39680, stats
            matched = 0
            for record in records:
                if (record["mechanism"] == "adaptive_quality_geometry_v31_local" and
                    record["source_hash"] == fnv1a64(ada) and record["slot_size"] == len(ada) and
                    record["source_fingerprint"] == fingerprint_elf(ada)):
                    record["replacement"] = payload
                    matched += 1
            assert matched, "No exact approved geometry source identity"
            report["geometry"] = stats
        elif name == "Kernel_BlendCandidatesFused":
            source = sources[120]
            assert len(source) == 39638 and fnv1a64(source.encode("ascii")) == 0x7a6f5f41105c6d85
            source = source.replace(".target sm_120", ".target sm_89")
            source_path = args.output / "native-warp.ptx"
            source_path.write_bytes(source.encode("ascii"))
            subprocess.run([str(args.emitter), "--emit", str(source_path), str(args.output)], check=True)
            patched = (args.output / "warp-preset-0.ptx").read_text()
            has_axis_candidate = "MFGUNLOCK_AXIS_BLEND_CANDIDATE" in patched
            if has_axis_candidate != (args.variant in ["border-axis-candidate", "release-1.4.3"]):
                raise ValueError("Emitter does not match --variant; refusing mislabeled validation")
            assert accesses(patched) == accesses(patch_adaptive_blend_v3(source))
            _, stats = assemble(patched, "warp-continuous-border", args, 48)
            compiler = CudaDriverCompiler()
            try:
                jit = compiler.compile(patched, "stability-warp")
                jit_text, jit_shared, jit_regs = fingerprint_elf(jit)
                assert jit_regs <= 48, (jit_text, jit_shared, jit_regs)
                jit_path = args.output / "warp-driver-jit.cubin"
                jit_path.write_bytes(jit)
                sass = subprocess.check_output([str(args.nvdisasm), str(jit_path)], text=True)
                assert not re.search(r"\b(?:LDL|STL)\b", sass)
                stats["driver_jit_registers"] = jit_regs
                stats["driver_jit_text"] = jit_text
            finally:
                compiler.close()
            stats["ptx_sha256"] = hashlib.sha256(patched.encode("ascii")).hexdigest()
            report["warp"] = stats
    assert set(report) == {"geometry", "warp"}
    emit_header(records, args.header, [args.provider])
    report["metadata"] = dict(baseline_version="1.4.2", variant=args.variant,
        release_version="1.4.3" if args.variant == "release-1.4.3" else None,
        provider_sha256=hashlib.sha256(args.provider.read_bytes()).hexdigest(),
        baseline_table_sha256=hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
        generated_table_sha256=hashlib.sha256(args.header.read_bytes()).hexdigest(),
        emitter_sha256=hashlib.sha256(args.emitter.read_bytes()).hexdigest(),
        warp_source_sha256=hashlib.sha256((Path(__file__).resolve().parents[1] /
            "src/addons/mfgunlock/adaptive_quality_v3.hpp").read_bytes()).hexdigest(),
        rewrite_source_sha256=hashlib.sha256((Path(__file__).resolve().parents[1] /
            "src/addons/mfgunlock/thin_geometry.hpp").read_bytes()).hexdigest(),
        ptxas=subprocess.check_output([str(args.ptxas), "--version"], text=True).strip(),
        resource_accesses="identical to the 1.4.2 local baseline",
        runtime_validation="pending; assembly/JIT only")
    (args.output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
