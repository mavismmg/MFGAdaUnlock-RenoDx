"""Execute the exact C++-emitted border fragment against its FP32 oracle on CUDA.

Synthetic correctness only: this does not benchmark a game's DLSS-G pipeline.
"""
import argparse
import csv
import ctypes
import hashlib
import json
import math
import subprocess
from pathlib import Path
from build_thin_geometry_variants import CudaDriverCompiler, fingerprint_elf


def harness(body):
    return """.version 8.0
.target sm_89
.address_size 64
.visible .entry BorderAxis(.param .u64 input, .param .u64 output, .param .u32 count)
{
.reg .pred %qv<7>, %outside;
.reg .f32 %qf<12>, %f<200>;
.reg .b32 %r<12>, %index<4>;
.reg .b64 %address<4>;
ld.param.u64 %address0, [input];
ld.param.u64 %address1, [output];
ld.param.u32 %index3, [count];
mov.u32 %index0, %tid.x;
mov.u32 %index1, %ctaid.x;
mov.u32 %index2, %ntid.x;
mad.lo.u32 %index0, %index1, %index2, %index0;
setp.ge.u32 %outside, %index0, %index3;
@%outside bra FINISH;
mul.wide.u32 %address2, %index0, 32;
add.u64 %address2, %address0, %address2;
ld.global.f32 %qf0, [%address2];
ld.global.f32 %qf1, [%address2+4];
cvt.rni.u32.f32 %r10, %qf0;
cvt.rni.u32.f32 %r11, %qf1;
ld.global.f32 %f1, [%address2+8];
ld.global.f32 %f2, [%address2+12];
ld.global.f32 %f123, [%address2+16];
ld.global.f32 %f124, [%address2+20];
ld.global.f32 %f129, [%address2+24];
ld.global.f32 %f130, [%address2+28];
""" + body + """
mul.wide.u32 %address3, %index0, 8;
add.u64 %address3, %address1, %address3;
st.global.f32 [%address3], %f180;
st.global.f32 [%address3+4], %f181;
FINISH:
ret;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emitter', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(args.emitter), '--emit', str(args.output)], check=True)
    body = (args.output / 'border-program.ptx').read_text(encoding='ascii')
    rows = list(csv.DictReader((args.output / 'border-cases.csv').open(newline='')))
    fields = ['width', 'height', 'u', 'v', 'forward_u', 'forward_v', 'inverse_u', 'inverse_v']
    values = [float(row[k]) for row in rows for k in fields]
    expected = [float(row[k]) for row in rows for k in ['expected_forward', 'expected_inverse']]
    source = harness(body)
    (args.output / 'border-harness.ptx').write_text(source, encoding='ascii')
    compiler = CudaDriverCompiler()
    cuda = compiler.cuda
    module, allocations = ctypes.c_void_p(), []
    pointer = ctypes.c_uint64
    cuda.cuMemAlloc_v2.argtypes = [ctypes.POINTER(pointer), ctypes.c_size_t]
    cuda.cuMemFree_v2.argtypes = [pointer]
    cuda.cuMemcpyHtoD_v2.argtypes = [pointer, ctypes.c_void_p, ctypes.c_size_t]
    cuda.cuMemcpyDtoH_v2.argtypes = [ctypes.c_void_p, pointer, ctypes.c_size_t]
    cuda.cuModuleLoadData.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    cuda.cuModuleUnload.argtypes = [ctypes.c_void_p]
    cuda.cuModuleGetFunction.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_char_p]
    cuda.cuLaunchKernel.argtypes = [ctypes.c_void_p] + [ctypes.c_uint] * 7 + [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    cuda.cuCtxSynchronize.argtypes = []
    try:
        cubin = compiler.compile(source, 'border-axis-harness')
        (args.output / 'border-harness.cubin').write_bytes(cubin)
        encoded = ctypes.create_string_buffer(cubin)
        compiler._check(cuda.cuModuleLoadData(ctypes.byref(module), encoded), 'load harness')
        function = ctypes.c_void_p()
        compiler._check(cuda.cuModuleGetFunction(ctypes.byref(function), module, b'BorderAxis'), 'get harness')
        host_input = (ctypes.c_float * len(values))(*values)
        host_output = (ctypes.c_float * len(expected))()
        inputs, outputs = pointer(), pointer()
        for target, size in [(inputs, ctypes.sizeof(host_input)), (outputs, ctypes.sizeof(host_output))]:
            compiler._check(cuda.cuMemAlloc_v2(ctypes.byref(target), size), 'allocate harness')
            allocations.append(target)
        compiler._check(cuda.cuMemcpyHtoD_v2(inputs, host_input, ctypes.sizeof(host_input)), 'upload cases')
        count = ctypes.c_uint(len(rows))
        parameters = (ctypes.c_void_p * 3)(*[ctypes.cast(ctypes.byref(x), ctypes.c_void_p) for x in (inputs, outputs, count)])
        compiler._check(cuda.cuLaunchKernel(function, (len(rows)+127)//128, 1, 1, 128, 1, 1, 0, None, parameters, None), 'launch harness')
        compiler._check(cuda.cuCtxSynchronize(), 'synchronize harness')
        compiler._check(cuda.cuMemcpyDtoH_v2(host_output, outputs, ctypes.sizeof(host_output)), 'download results')
        errors = [abs(actual - reference) for actual, reference in zip(host_output, expected)]
        tolerance = 0.003  # Pixel distances, allowing reciprocal/FMA rounding.
        failures = [i for i, (actual, error) in enumerate(zip(host_output, errors)) if not math.isfinite(actual) or error > tolerance]
        report = dict(cases=len(rows), directions=2, tolerance_pixels=tolerance,
            max_error_pixels=max(errors), failures=len(failures),
            fragment_sha256=hashlib.sha256(body.encode('ascii')).hexdigest(),
            harness_fingerprint=fingerprint_elf(cubin),
            validation='synthetic GPU correctness; no in-game quality or latency claim')
        (args.output / 'gpu-validation.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2))
        if failures:
            raise RuntimeError(f'{len(failures)} GPU/oracle mismatches; first index {failures[0]}')
    finally:
        for allocation in allocations: cuda.cuMemFree_v2(allocation)
        if module: cuda.cuModuleUnload(module)
        compiler.close()

if __name__ == '__main__':
    main()
