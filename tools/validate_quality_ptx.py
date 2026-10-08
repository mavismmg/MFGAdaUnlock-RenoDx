"""JIT-validate the injected quality PTX fragments with the installed driver."""

from __future__ import annotations

import argparse
import ctypes
from pathlib import Path
import platform
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
from build_thin_geometry_variants import fingerprint_elf


def fragments(name: str, git_ref: str | None) -> list[str]:
    if git_ref:
        text = subprocess.check_output(
            ["git", "show", f"{git_ref}:src/addons/mfgunlock/{name}"],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
        )
    else:
        text = (ROOT / "src" / "addons" / "mfgunlock" / name).read_text(
            encoding="utf-8"
        )
    # Generated fragments contain placeholders until the C++ emitter resolves
    # them. Validate only complete constants here; validate_border_axis.py
    # executes the emitted candidate against the actual C++ oracle on GPU.
    return re.findall(r'inline constexpr const char\* \w+ = R"PTX\((.*?)\)PTX"', text, re.DOTALL)


def validation_entry(name: str, body: str) -> str:
    return f""".visible .entry {name}()
{{
.reg .pred %qv<7>;
.reg .f32 %qf<12>;
.reg .f32 %f<200>;
.reg .b32 %r<12>;
{body}
st.global.f32 [MfgUnlockQualityValidationSink], %qf0;
ret;
}}
"""


def check(result: int, cuda, log: ctypes.Array[ctypes.c_char]) -> None:
    if result == 0:
        return
    message = ctypes.c_char_p()
    cuda.cuGetErrorString(result, ctypes.byref(message))
    detail = message.value.decode(errors="replace") if message.value else str(result)
    compiler = log.value.decode(errors="replace")
    raise RuntimeError(f"CUDA driver JIT failed: {detail}\n{compiler}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--git-ref",
        help="validate fragments from a committed revision instead of the worktree",
    )
    args = parser.parse_args()
    if platform.system() != "Windows":
        raise SystemExit("quality PTX JIT validation currently requires Windows")
    program = """.version 8.0
.target sm_89
.address_size 64
.visible .global .align 4 .f32 MfgUnlockQualityValidationSink;
"""
    v1 = (
        fragments("quality_refinement.hpp", args.git_ref)[0]
        + fragments("quality_border.hpp", args.git_ref)[0]
        + fragments("adaptive_quality.hpp", args.git_ref)[0]
    )
    v2 = "".join(fragments("adaptive_quality_v2.hpp", args.git_ref))
    v3 = "".join(fragments("adaptive_quality_v3.hpp", args.git_ref))
    program += validation_entry("MfgUnlockQualityV1Validation", v1)
    program += validation_entry("MfgUnlockQualityV2Validation", v2)
    program += validation_entry("MfgUnlockQualityV3Validation", v3)

    cuda = ctypes.WinDLL("nvcuda.dll")
    cuda.cuInit.argtypes = [ctypes.c_uint]
    cuda.cuDeviceGet.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    cuda.cuCtxCreate_v2.argtypes = [
        ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint, ctypes.c_int
    ]
    cuda.cuModuleLoadDataEx.argtypes = [
        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_uint,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_void_p),
    ]
    cuda.cuModuleUnload.argtypes = [ctypes.c_void_p]
    cuda.cuModuleGetFunction.argtypes = [
        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_char_p
    ]
    cuda.cuFuncGetAttribute.argtypes = [
        ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_void_p
    ]
    cuda.cuCtxDestroy_v2.argtypes = [ctypes.c_void_p]
    cuda.cuGetErrorString.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    cuda.cuLinkCreate_v2.argtypes = [
        ctypes.c_uint, ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    ]
    cuda.cuLinkAddData_v2.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    cuda.cuLinkComplete.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    cuda.cuLinkDestroy.argtypes = [ctypes.c_void_p]

    empty_log = ctypes.create_string_buffer(1)
    check(cuda.cuInit(0), cuda, empty_log)
    device = ctypes.c_int()
    check(cuda.cuDeviceGet(ctypes.byref(device), 0), cuda, empty_log)
    context = ctypes.c_void_p()
    check(cuda.cuCtxCreate_v2(ctypes.byref(context), 0, device), cuda, empty_log)
    try:
        module = ctypes.c_void_p()
        log = ctypes.create_string_buffer(8192)
        log_size = ctypes.c_size_t(len(log))
        # CU_JIT_INFO_LOG_BUFFER and CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES.
        options = (ctypes.c_int * 2)(3, 4)
        values = (ctypes.c_void_p * 2)(
            ctypes.cast(log, ctypes.c_void_p),
            ctypes.cast(ctypes.pointer(log_size), ctypes.c_void_p),
        )
        encoded = ctypes.create_string_buffer(program.encode("ascii"))
        result = cuda.cuModuleLoadDataEx(
            ctypes.byref(module), ctypes.cast(encoded, ctypes.c_void_p),
            len(options), options, values,
        )
        check(result, cuda, log)
        register_counts = {}
        for function_name in (
            b"MfgUnlockQualityV1Validation",
            b"MfgUnlockQualityV2Validation",
            b"MfgUnlockQualityV3Validation",
        ):
            function = ctypes.c_void_p()
            check(
                cuda.cuModuleGetFunction(
                    ctypes.byref(function), module, function_name,
                ),
                cuda, log,
            )
            registers = ctypes.c_int()
            # CU_FUNC_ATTRIBUTE_NUM_REGS.
            check(cuda.cuFuncGetAttribute(
                ctypes.byref(registers), 4, function), cuda, log)
            register_counts[function_name.decode("ascii")] = registers.value
        check(cuda.cuModuleUnload(module), cuda, log)
        link_state = ctypes.c_void_p()
        check(
            cuda.cuLinkCreate_v2(
                len(options), options, values, ctypes.byref(link_state)
            ),
            cuda, log,
        )
        try:
            # CU_JIT_INPUT_PTX = 1. Include the NUL terminator in the input.
            check(
                cuda.cuLinkAddData_v2(
                    link_state, 1, ctypes.cast(encoded, ctypes.c_void_p),
                    len(encoded), b"mfgunlock-quality.ptx", 0, None, None,
                ),
                cuda, log,
            )
            cubin_pointer = ctypes.c_void_p()
            cubin_size = ctypes.c_size_t()
            check(
                cuda.cuLinkComplete(
                    link_state, ctypes.byref(cubin_pointer),
                    ctypes.byref(cubin_size),
                ),
                cuda, log,
            )
            cubin = ctypes.string_at(cubin_pointer, cubin_size.value)
            text_bytes, shared_bytes, elf_registers = fingerprint_elf(cubin)
        finally:
            cuda.cuLinkDestroy(link_state)
        source = args.git_ref or "worktree"
        print(f"quality PTX fragments ({source}) passed NVIDIA driver JIT validation")
        print(
            "synthetic V1/V2/V3 allocation: "
            f"{register_counts['MfgUnlockQualityV1Validation']}/"
            f"{register_counts['MfgUnlockQualityV2Validation']}/"
            f"{register_counts['MfgUnlockQualityV3Validation']} registers/thread"
        )
        if (register_counts["MfgUnlockQualityV2Validation"] >
                register_counts["MfgUnlockQualityV1Validation"]):
            raise RuntimeError("V2 synthetic fragment reduced theoretical occupancy")
        if register_counts["MfgUnlockQualityV3Validation"] > 48:
            raise RuntimeError("V3 synthetic fragment exceeds 48 registers/thread")
        print(
            "synthetic cubin: "
            f"{text_bytes} text bytes, {shared_bytes} shared bytes, "
            f"{elf_registers} ELF registers"
        )
        if log.value:
            print(log.value.decode(errors="replace").strip())
    finally:
        cuda.cuCtxDestroy_v2(context)


if __name__ == "__main__":
    main()
