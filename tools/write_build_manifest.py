"""Write a reviewable identity manifest without exporting any provider payloads."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from build_thin_geometry_variants import load_generated_records, fingerprint_elf
from inspect_generated_cubins import arrays

IDENTITIES = {
    'release-1.4.3': b'Release 1.4.3 - Continuous Border Stability',
    'control': b'Release 1.4.2 - Local Stability & CPU Overhead',
    'border-axis-candidate': b'1.4.2-base - Border Axis Candidate',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_validation(report, variant, directory, header_digest):
    if report['metadata']['variant'] != variant:
        raise ValueError('Validation report belongs to another variant')
    if report['metadata']['generated_table_sha256'] != header_digest:
        raise ValueError('Validation report belongs to another generated payload table')
    # The emitter must still be the same source that this binary was built from.
    if report['metadata']['warp_source_sha256'] != sha((directory / 'adaptive_quality_v3.hpp').read_bytes()):
        raise ValueError('Warp source changed since validation')
    if report['metadata']['rewrite_source_sha256'] != sha((directory / 'thin_geometry.hpp').read_bytes()):
        raise ValueError('Rewrite source changed since validation')


def make_manifest(addon, root, variant, validation=None):
    binary = addon.read_bytes()
    if not binary.startswith(b'MZ') or IDENTITIES[variant] not in binary:
        raise ValueError('Addon binary identity does not match requested variant')
    directory = root / 'src/addons/mfgunlock'
    header = directory / 'thin_geometry_stability.generated.hpp'
    blackwell = directory / 'blackwell_cubins.generated.hpp'
    payloads = []
    for record in load_generated_records(header):
        payloads.append(dict(mechanism=record['mechanism'],
            source_fingerprint=record['source_fingerprint'],
            source_fnv1a64=f"{record['source_hash']:016x}",
            slot_size=record['slot_size'],
            replacement_sha256=sha(record['replacement']),
            replacement_fingerprint=fingerprint_elf(record['replacement'])))
    blackwell_payloads = [dict(symbol=name, sha256=sha(data), fingerprint=fingerprint_elf(data))
        for name, data in arrays(blackwell).items()]
    commit = subprocess.check_output(['git','rev-parse','HEAD'], cwd=root, text=True).strip()
    status = subprocess.check_output(['git','status','--porcelain','--untracked-files=normal'], cwd=root, text=True)
    source_inputs = {}
    for subdir in ['src', 'tools', 'tests']:
        for file in sorted((root / subdir).rglob('*')):
            if file.is_file() and file.suffix in ['.hpp','.cpp','.py','.ps1','.txt'] and 'generated.hpp' not in file.name:
                source_inputs[file.relative_to(root).as_posix()] = sha(file.read_bytes())
    manifest = dict(baseline_version='1.4.2', baseline_commit='3a2c021d5882ffd62e2872d84ed752398be58f58',
        release_version='1.4.3' if variant == 'release-1.4.3' else ('1.4.2' if variant == 'control' else '1.4.2-base'),
        variant=variant, addon_sha256=sha(binary), source_commit=commit, source_modified=bool(status.strip()),
        source_inputs=source_inputs, geometry='Local Stable', inpaint='V2 Compatibility',
        confidence_history=False, temporal_launch_hooks=False,
        provider_validation='unchanged exact 310.9.0/310.9.1 profiles; actual activation requires runtime diagnostics',
        generated_headers={header.name:sha(header.read_bytes()),blackwell.name:sha(blackwell.read_bytes())},
        thin_geometry_payload_inputs=payloads, blackwell_payload_inputs=blackwell_payloads,
        in_game_validation='initial maintainer feedback positive; comparative benchmarks pending'
            if variant == 'release-1.4.3' else 'pending')
    if validation:
        report = json.loads(validation.read_text())
        check_validation(report, variant, directory, sha(header.read_bytes()))
        manifest['assembly_validation'] = report
        manifest['assembly_report_sha256'] = sha(validation.read_bytes())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--variant', choices=IDENTITIES, required=True)
    parser.add_argument('--validation', type=Path)
    args = parser.parse_args()
    manifest = make_manifest(args.addon, args.source_root, args.variant, args.validation)
    output = args.addon.with_suffix(args.addon.suffix+'.manifest.json')
    output.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'Build manifest: {output.name}; {manifest["addon_sha256"]}')

if __name__ == '__main__':
    main()
