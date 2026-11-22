"""1.11.0 (R4): Add frame generation in a game that creates its swap chain on MFG Unlock's Streamline.

RenoDX's DLSS addon upgrades the game's factory with the hosted sl.interposer.dll and creates the swap
chain there (Lies of P, Hi-Fi RUSH, 2026-11-20). Before H4 the addon put a second Streamline swap chain
under it ("Streamline supports only one DXGISwapChain", DLSS-G "Reflex not detected" at every frame).
ngx_sr_app --sl-factory does the same. Both runs, with and without it, must generate with DLSS-G status
0x0 and one Streamline swap chain.
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--app', type=pathlib.Path, required=True)
ap.add_argument('--addon', type=pathlib.Path, required=True)
ap.add_argument('--reshade', type=pathlib.Path, required=True)
ap.add_argument('--sl-bin', type=pathlib.Path, required=True)
ap.add_argument('--seconds', type=int, default=12)
a = ap.parse_args()
for p in (a.app, a.addon, a.reshade, a.sl_bin / 'sl.interposer.dll', a.sl_bin / 'nvngx_dlss.dll'):
    if not p.exists():
        print(f'SKIP: {p} not found')
        sys.exit(77)

NL = '\r\n'
STREAMLINE = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')


def run(sl_factory):
    with tempfile.TemporaryDirectory(prefix='mfg-host-') as tmp:
        d = pathlib.Path(tmp)
        shutil.copy2(a.app, d / 'ngx_sr_app.exe')
        shutil.copy2(a.sl_bin / 'nvngx_dlss.dll', d / 'nvngx_dlss.dll')
        shutil.copy2(a.addon, d / 'renodx-mfgunlock.addon64')
        shutil.copy2(a.reshade, d / 'd3d12.dll')
        (d / 'MFGUnlock-Streamline').mkdir()
        for f in STREAMLINE:
            shutil.copy2(a.sl_bin / f, d / 'MFGUnlock-Streamline' / f)
        (d / 'ReShade.ini').write_text(NL.join(['[GENERAL]', 'NoDebugInfo=1', '[OVERLAY]', 'TutorialProgress=4',
                                                '[ADDON]', 'LoadFromDllMain=renodx-mfgunlock.addon64', '']))
        (d / 'MFGAdaUnlock.ini').write_text(NL.join(['[DLSSG]', 'AddFrameGeneration=1', 'StreamlineLog=2', '']))
        args = [str(d / 'ngx_sr_app.exe'), str(a.seconds), '--windowed'] + (['--sl-factory'] if sl_factory else [])
        out = subprocess.run(args, cwd=d, capture_output=True, text=True, timeout=a.seconds + 60).stdout
        log = (d / 'ReShade.log').read_text(errors='ignore') if (d / 'ReShade.log').exists() else ''
        sl = (d / 'sl.log').read_text(errors='ignore') if (d / 'sl.log').exists() else ''
        name = 'sl-factory' if sl_factory else 'plain'
        statuses = re.findall(r'DLSS-G status (0x[0-9a-f]+), frames presented per game frame (\d+)', log)
        app = [l for l in out.splitlines() if l.startswith('ngx_sr_app: frames')]
        print(f'{name}: {app[-1] if app else "(no frames line)"}; statuses {statuses[-3:]}')
        problems = []
        if 'ngx_sr_app: frames' not in out:
            problems.append('the app did not finish')
        if not statuses or any(s != '0x0' for s, _ in statuses[1:]):
            problems.append(f'DLSS-G status not 0x0: {statuses}')
        if not any(int(n) >= 2 for _, n in statuses):
            problems.append('no generated frames')
        if 'supports only one DXGISwapChain' in sl:
            problems.append('a second Streamline swap chain (sl.log)')
        if sl_factory and 'created by another addon' not in log:
            problems.append('the swap chain made on Streamline\'s factory was not adopted')
        for p in problems:
            print(f'{name}: FAIL {p}')
        return not problems


ok = run(False) & run(True)
sys.exit(0 if ok else 1)
