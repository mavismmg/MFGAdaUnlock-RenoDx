"""Prepare isolated, exact-control PTX experiments; never installs a runtime patch."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from build_local_stability import accesses, assemble


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Expected one exact experiment anchor')
    return text.replace(old, new, 1)


def division_group(text, register, constant, reciprocal, count):
    if text.count(f'mov.f32 %{register}, {constant};') != 1:
        raise ValueError('Divisor is not the exact expected constant')
    pattern = re.compile(r'div\.approx\.ftz\.f32 (%\w+), (%\w+), %' + register + r';')
    if len(pattern.findall(text)) != count:
        raise ValueError('Division site count differs from the validated profile')
    return pattern.sub(lambda m: f'mul.ftz.f32 {m[1]}, {m[2]}, {reciprocal};', text)


def variants(control):
    if 'MFGUNLOCK_AXIS_BLEND_CANDIDATE' in control:
        raise ValueError('Experiments must start from the 1.4.2 control, not the border candidate')
    agreement = replace_once(control,
        'sub.f32 %qf3, 0f3E99999A, %f176;\nmul.sat.f32 %qf3, %qf3, 0f4091745D;',
        'sub.f32 %qf3, 0f3E75C28F, %f176;\nmul.sat.f32 %qf3, %qf3, 0f40B1C71C;')
    half = replace_once(control, '// MFGUNLOCK_DIRECTIONAL_BORDER_CONFIDENCE_V3',
        '// Research: halve only added weight for structurally valid candidates.\n'
        'sub.f32 %qf2, %qf0, %f174;\n'
        '@%qv0 fma.rn.f32 %qf0, 0f3F000000, %qf2, %f174;\n'
        'sub.f32 %qf3, %qf1, %f175;\n'
        '@%qv1 fma.rn.f32 %qf1, 0f3F000000, %qf3, %f175;\n'
        '// MFGUNLOCK_DIRECTIONAL_BORDER_CONFIDENCE_V3')
    return {
        'agreement-strict': agreement,  # Agreement interval .06..24; other ramps unchanged.
        'half-added-warp': half,
        'divide-one': division_group(control,'f537','0f3F800000','0f3F800000',8),
        'divide-four': division_group(control,'f528','0f40800000','0f3E800000',8),
        'divide-three': division_group(control,'f193','0f40400000','0f3EAAAAAB',1),
    }


def sass_counts(text):
    instructions = re.findall(r'/\*[0-9a-fA-F]+\*/\s+(?:@!?P\d+\s+)?([A-Z][A-Z0-9_.]*)', text)
    return dict(instructions=len(instructions), reciprocal=sum(x=='MUFU.RCP' for x in instructions),
        local_accesses=sum(x.startswith(('LDL','STL')) for x in instructions))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control',type=Path,required=True,help='Directory containing control validation.json and warp-preset-0.ptx')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ptxas',type=Path)
    parser.add_argument('--nvdisasm',type=Path)
    args=parser.parse_args()
    if bool(args.ptxas) != bool(args.nvdisasm):
        parser.error('Provide both ptxas and nvdisasm or neither')
    report=json.loads((args.control/'validation.json').read_text())
    control=(args.control/'warp-preset-0.ptx').read_text()
    control_hash=hashlib.sha256(control.encode('ascii')).hexdigest()
    if report['metadata']['variant']!='control' or report['warp']['ptx_sha256']!=control_hash:
        raise ValueError('Control identity or PTX hash differs from its validated report')
    args.output.mkdir(parents=True,exist_ok=True)
    results={}
    for name, source in {'control':control, **variants(control)}.items():
        if accesses(source)!=accesses(control):
            raise ValueError(f'{name} changed resource access instructions')
        (args.output/(name+'.ptx')).write_text(source,encoding='ascii')
        result=dict(ptx_sha256=hashlib.sha256(source.encode('ascii')).hexdigest(),
            div_approx_instructions=source.count('div.approx'),
            resource_accesses='unchanged',runtime_enabled=False)
        if args.ptxas:
            _, result['assembly']=assemble(source,name,args,48)
            result['sass']=sass_counts((args.output/(name+'.sass')).read_text())
        if args.ptxas and name != 'control':
            result['same_cubin_as_control'] = result['assembly']['sha256'] == results['control']['assembly']['sha256']
            result['same_sass_as_control'] = (args.output/(name+'.sass')).read_text() == (args.output/'control.sass').read_text()
        results[name]=result
    output=dict(baseline_version='1.4.2',control_ptx_sha256=control_hash,
        provider_sha256=report['metadata']['provider_sha256'], experiments=results,
        status='offline research; numeric and in-game quality/performance must be evaluated separately')
    (args.output/'experiments.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(output,indent=2))

if __name__=='__main__': main()
