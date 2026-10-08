"""Plan repeated control/candidate game captures and inventory collected evidence.

This does not launch games, change settings, or automatically certify quality.
"""
import argparse
import json
from pathlib import Path

GAMES = [
    ('onimusha','D3D12','fences, silhouettes, hair and slow border pans'),
    ('stalker2','D3D12','foliage, disocclusions and warmed camera route'),
    ('cyberpunk2077','D3D12','moving shadows, bright HUD, particles and camera pans'),
    ('hogwarts-legacy','D3D12','HDR UI, highlights and split/final-color transitions'),
    ('indiana-jones','Vulkan','renderer compatibility, borders and HUD'),
    ('witcher3','D3D12','deformation-motion investigation; compatibility exploratory'),
]


def matrix(tier="core"):
    jobs=[]
    for game, renderer, scene in GAMES:
        for mode in ['fixed-2x','fixed-4x','fixed-6x','dynamic']:
            for color in ['SDR','HDR-if-supported']:
                for variant in ['control','border-axis-candidate']:
                    for repetition in range(1,4):
                        identity=f'{game}_{mode}_{color}_{variant}_run{repetition}'
                        core = ((mode=='fixed-4x' and color=='SDR' and game!='witcher3') or
                            (game=='hogwarts-legacy' and mode=='fixed-4x' and color=='HDR-if-supported') or
                            (game=='onimusha' and mode=='fixed-2x' and color=='SDR'))
                        if tier=='core' and not core: continue
                        jobs.append(dict(id=identity, game=game, renderer=renderer, scene=scene,
                            tier='core' if core else 'extended',
                            requested_mode=mode,color=color,variant=variant,repetition=repetition,
                            duration_seconds=45,status='pending',
                            eligibility='Verify native support and effective multiplier; record unsupported cases instead of forcing support'))
    return dict(baseline_version='1.4.2',jobs=jobs,
        fixed_conditions=['warmed save and route','resolution and SR preset','cap','VSync/VRR',
            'driver','loaded runtime hashes','output color space and HDR settings','quality profile and UI composition mode'],
        required_evidence=['PresentMon summary','capture-metadata.json','NVAPI effective multiplier',
            'binary manifest','matched visual footage','GPU/VRAM/Reflex fields when available'],
        quality_cases=['45-degree direction crossing','subpixel pans','four edges and corners',
            'wires and foliage','hair','moving shadows','particles','HUD','disocclusions'],
        input_audit=['subrect and extents','UI alpha and premultiplication','exposure','MV scale/jitter/dilation',
            'camera matrices','deformation motion','split/final-color resource lifetimes'],
        color_domains=['SDR','PQ when actually used','scRGB when actually used'],
        expensive_research='Temporal history/reconstruction disabled; first distinguish missing motion from candidate rejection and fallback',
        promotion='Requires repeatable visual benefit and performance within repeated-control variability; never inferred from missing metrics or synthetic tests')


def inventory(plan, capture_root):
    evidence=[]
    for job in plan['jobs']:
        directory=capture_root/job['id']
        summaries=list(directory.glob('*.summary.json')) if directory.exists() else []
        metadata=directory/'capture-metadata.json'
        if summaries and metadata.is_file():
            json.loads(metadata.read_text(encoding='utf-8-sig'))
            reports=[json.loads(p.read_text(encoding='utf-8-sig')) for p in summaries]
            status='capture-present; visual and comparability review pending'
        else:
            reports=[]
            status='pending; required capture files missing'
        evidence.append(dict(id=job['id'],status=status,summaries=reports))
    return dict(evidence=evidence,promotion='not evaluated',
        reason='Presence of capture files does not establish comparable runs, effective MFG, HDR domain or image quality')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--capture-root',type=Path)
    parser.add_argument('--tier',choices=['core','all'],default='core')
    args=parser.parse_args()
    plan=matrix(args.tier)
    if args.capture_root: plan['inventory']=inventory(plan,args.capture_root)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(plan,indent=2)+'\n',encoding='utf-8')
    print(f'{len(plan["jobs"])} planned runs; unsupported modes may be recorded as skipped; no in-game validation claimed')

if __name__=='__main__': main()
