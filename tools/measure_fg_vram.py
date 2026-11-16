# SPDX-License-Identifier: MIT
"""1.10.0 (plan XMX/VRAM): how much video memory frame generation costs, NVIDIA's
DLSS-G against XMX, on NVIDIA's Streamline Sample (tools/sl_sample.py build), on
this PC's GPU.

Each configuration runs the sample in place (it loads its media from beside _bin,
as tests/xemfg_sl_sample_tests.py does) with ReShade and the addon for --seconds,
then reads Windows' own per-process counter, "\\GPU Process Memory(pid_<pid>*)\\
Dedicated Usage", averaged over 5 samples. "off" runs without DLSS-G (the
baseline); the others start with DLSS-G on (MFG_SAMPLE_DLSSG=1). The counter does
not depend on the addon. Logs of each run are kept under --work.

  python tools/measure_fg_vram.py --bin <sample _bin> --addon <native addon64>
      --reshade <ReShade64.dll> --sdk-fg <libxess_fg.dll> --sdk-ll <libxell.dll>
      [--config off,native,xmx] [--multiplier 4] [--seconds 25] [--work <dir>]

Extra configurations come as name=ini-lines, for example
  --config "xmx-match=[Backend]\\nSelected=DLSSG\\n[DLSSG]\\nXmxGenerator=1\\nXmxCapacity=1"
"""
import argparse, os, pathlib, shutil, subprocess, sys, time

NL = '\n'
PRESETS = {
    'off': NL.join(['[Backend]', 'Selected=DLSSG', '[DLSSG]', 'XmxGenerator=0', 'AutoEarlyLoad=0', '']),
    'native': NL.join(['[Backend]', 'Selected=DLSSG', '[DLSSG]', 'XmxGenerator=0', 'AutoEarlyLoad=0', '']),
    'xmx': NL.join(['[Backend]', 'Selected=DLSSG', '[DLSSG]', 'XmxGenerator=1', 'AutoEarlyLoad=0', '']),
    # 1.10.0: XMX as before, with NVIDIA's unused feature created (XmxSaveVram=0).
    'xmx-nvidia': NL.join(['[Backend]', 'Selected=DLSSG', '[DLSSG]', 'XmxGenerator=1', 'XmxSaveVram=0',
                           'AutoEarlyLoad=0', '']),
}

ap = argparse.ArgumentParser()
ap.add_argument('--bin', type=pathlib.Path, required=True)
ap.add_argument('--addon', type=pathlib.Path, required=True)
ap.add_argument('--reshade', type=pathlib.Path, required=True)
ap.add_argument('--sdk-fg', type=pathlib.Path, required=True)
ap.add_argument('--sdk-ll', type=pathlib.Path, required=True)
ap.add_argument('--config', default='off,native,xmx,xmx-nvidia')
ap.add_argument('--multiplier', type=int, default=4)
ap.add_argument('--seconds', type=int, default=25)
ap.add_argument('--width', type=int, default=2560)
ap.add_argument('--height', type=int, default=1440)
ap.add_argument('--work', type=pathlib.Path, default=pathlib.Path('build/vram'))
a = ap.parse_args()
if not (a.bin / 'StreamlineSample.exe').is_file():
    print('SKIP: sample not built:', a.bin)
    raise SystemExit(77)


def dedicated_bytes(pid):
    """Sum of the process's dedicated GPU memory over its adapters, mean of 5 samples."""
    script = ("$s = Get-Counter -Counter '\\GPU Process Memory(pid_" + str(pid) + "_*)\\Dedicated Usage' "
              "-SampleInterval 1 -MaxSamples 5 -ErrorAction SilentlyContinue; "
              "if ($s) { ($s | ForEach-Object { ($_.CounterSamples | Measure-Object CookedValue -Sum).Sum } "
              "| Measure-Object -Average).Average } else { 0 }")
    out = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True)
    try:
        return int(float(out.stdout.strip().splitlines()[-1]))
    except (ValueError, IndexError):
        return 0


configs = []
for item in a.config.split(','):
    name, _, ini = item.partition('=')
    configs.append((name, ini.replace('\\n', NL) if ini else PRESETS[name]))

d = a.bin.resolve()
results = []
for name, ini in configs:
    for src, dst in ((a.addon, 'renodx-mfgunlock-native.addon64'), (a.reshade, 'dxgi.dll'),
                     (a.sdk_fg, 'libxess_fg.dll'), (a.sdk_ll, 'libxell.dll')):
        shutil.copy2(src, d / dst)
    # Killed runs count as unclean sessions; after 3 the addon's safe mode turns XMX off.
    for old in ('ReShade.log', 'MFGAdaUnlock-XeMFG.log', 'MFGAdaUnlock.session', 'MFGAdaUnlock-vramtrace.txt', 'sl.log'):
        if (d / old).exists():
            (d / old).unlink()
    (d / 'MFGAdaUnlock.ini').write_text(ini, encoding='ascii')
    reshade_ini = NL.join(['[GENERAL]', 'NoDebugInfo=1', '[OVERLAY]', 'TutorialProgress=4',
                           '[RenoDX.MFGUnlock]', 'ForceMultiplier=' + str(a.multiplier), ''])
    (d / 'ReShade.ini').write_text(reshade_ini, encoding='ascii')
    env = dict(os.environ)
    if name != 'off':
        env['MFG_SAMPLE_DLSSG'] = '1'
    p = subprocess.Popen([str(d / 'StreamlineSample.exe'), '-width', str(a.width), '-height', str(a.height)],
                         cwd=d, env=env)
    try:
        time.sleep(a.seconds)
        used = dedicated_bytes(p.pid) if p.poll() is None else 0
    finally:
        p.kill()
        p.wait(30)
    keep = a.work / (name + '-' + time.strftime('%Y%m%d-%H%M%S'))
    keep.mkdir(parents=True, exist_ok=True)
    for f in ('ReShade.log', 'MFGAdaUnlock-XeMFG.log', 'MFGAdaUnlock.ini', 'MFGAdaUnlock-vramtrace.txt', 'sl.log'):
        if (d / f).exists():
            shutil.copy2(d / f, keep / f)
    log = (d / 'ReShade.log').read_text(encoding='utf-8', errors='replace') if (d / 'ReShade.log').exists() else ''
    init = [l.split('mfgunlock: ')[-1] for l in log.splitlines() if 'mfgunlock: xmx init' in l]
    results.append((name, used, init[-1] if init else ''))
    print('== %s: %.0f MiB dedicated  %s  (%s)' % (name, used / 2**20, results[-1][2], keep))

base = next((u for n, u, _ in results if n == 'off'), 0)
print('\nSummary (%dx%d, %dx)' % (a.width, a.height, a.multiplier))
for name, used, _ in results:
    extra = '  (+%.0f MiB over off)' % ((used - base) / 2**20) if base and name != 'off' else ''
    print('  %-12s %8.0f MiB%s' % (name, used / 2**20, extra))
