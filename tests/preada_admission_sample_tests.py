# SPDX-License-Identifier: MIT
# 1.10.0 (plan 2.1 and 3.1): the RTX 20/30 admission against NVIDIA's real
# Streamline Sample (tools/sl_sample.py), with the DLSS-G backend, on this Ada
# GPU under [DLSSG] SimulatePreAda=2: NVAPI answers Ampere to everyone, so
# Streamline refuses DLSS-G as on an RTX 30, and only the admission answers Ada
# again.
#
# The sample calls slInit before ReShade loads the addon (ReShade comes in
# through dxgi.dll during process start), which is the Ratchet & Clank: Rift
# Apart order: the addon arrives while Streamline is starting, outside the
# slInit hook. The verdict is Streamline's own, from sl.log (the test sets
# SL_LOG_*: the addon's StreamlineLog arrives too late in this order):
#   admitted : the default admission. The DLSS-G plugin must keep the adapter
#              (mask 0x1) and report multi-frame support.
#   refused  : PreAdaAdmission=0, the negative control. The plugin must say
#              "Disabling DLSS-G since it is not supported on current hardware",
#              the field message: the simulation reproduces an RTX 30.
#   requirements-*: 1.10.0, the admission with NGX's requirements query
#              failing (SimulateRequirementsFailure); see the end of the file.
# The sample does not turn DLSS-G on in this mode, so generated frames are not
# checked here; the admission is.
import argparse, pathlib, re, shutil, subprocess, sys, time, os

ap = argparse.ArgumentParser()
ap.add_argument('--bin', type=pathlib.Path, required=True)
ap.add_argument('--addon', type=pathlib.Path, required=True)
ap.add_argument('--reshade', type=pathlib.Path, required=True)
ap.add_argument('--work', type=pathlib.Path, required=True)
ap.add_argument('--seconds', type=int, default=15)
a = ap.parse_args()
if not (a.bin / 'StreamlineSample.exe').is_file():
    print('SKIP: sample not built:', a.bin); raise SystemExit(77)
if not a.reshade.is_file():
    print('SKIP: no ReShade module:', a.reshade); raise SystemExit(77)

REFUSAL = 'Disabling DLSS-G since it is not supported on current hardware'

# The sample loads its scene from <exe folder>/../media. The copies live in --work, so a junction there to the
# sample's media keeps it from stopping on "Couldn't read file /media/sponza-plus.scene.json".
media = (a.work / 'media')
if not media.exists() and (a.bin.parent / 'media').is_dir():
    a.work.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cmd', '/c', 'mklink', '/J', str(media.resolve()), str((a.bin.parent / 'media').resolve())],
                   capture_output=True)

def run(name, admission, requirements_failure=0, answer=True):
    d = (a.work / (name + '-' + time.strftime('%Y%m%d-%H%M%S'))).resolve()
    shutil.copytree(a.bin, d, ignore=shutil.ignore_patterns('*.log', 'MFG-Support-*', 'MFGAdaUnlock*', 'ReShade.ini',
                                                            'renodx-mfgunlock*.addon64', 'dxgi.dll', 'libxe*.dll'))
    shutil.copy2(a.addon, d / 'renodx-mfgunlock.addon64')
    shutil.copy2(a.reshade, d / 'dxgi.dll')
    (d / 'MFGAdaUnlock.ini').write_text('[Backend]\nSelected=DLSSG\n[DLSSG]\nSimulatePreAda=2\nAutoEarlyLoad=0\n'
                                        'PreAdaAdmission=' + ('1' if admission else '0') + '\n'
                                        f'SimulateRequirementsFailure={requirements_failure}\n'
                                        f'PreAdaRequirementsAnswer={int(answer)}\n', encoding='ascii')
    (d / 'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[OVERLAY]\nTutorialProgress=4\n'
                                   '[ADDON]\nLoadFromDllMain=renodx-mfgunlock.addon64\n', encoding='ascii')
    env = dict(os.environ, MFG_SAMPLE_DLSSG='1', SL_LOG_LEVEL='2', SL_LOG_PATH=str(d), SL_LOG_NAME='sl.log')
    p = subprocess.Popen([str(d / 'StreamlineSample.exe'), '-width', '1280', '-height', '720'], cwd=d, env=env)
    try:
        time.sleep(a.seconds)
    finally:
        p.kill(); p.wait(30)
    sl = (d / 'sl.log').read_text(encoding='utf-8', errors='replace') if (d / 'sl.log').exists() else ''
    masks = set(re.findall(r"Loaded plugin 'sl\.dlss_g'.*adapter mask (0x[0-9a-f]+)", sl))
    refused = REFUSAL in sl
    multi = re.search(r'Multi-frame supported, max generated frames (\d+)', sl)
    fallback = 'using SL defaults' in sl
    trace = d / 'MFGAdaUnlock-archtrace.txt'
    trace_text = trace.read_text(errors='replace') if trace.exists() else ''
    callers = sorted(set(re.findall(r'caller (\S+) .* answer (0x[0-9a-f]+)', trace_text)))
    requirements = sorted(set(re.findall(r'^requirements (.*)$', trace_text, re.M)))
    print(f'== {name}: {d}\n   sl.log: {len(sl.splitlines())} lines; dlss_g adapter masks {sorted(masks)}; refusal {refused}; '
          f'multi-frame {multi.group(1) if multi else "no"}; SL default fallback {fallback}\n'
          f'   architecture answers: {callers}\n   requirements: {requirements}')
    return sl != '', masks, refused, multi is not None, requirements, fallback

ok, masks, refused, multi, _, _ = run('admitted', True)
nok, nmasks, nrefused, _, _, _ = run('refused', False)
if not ok or not nok:
    raise SystemExit('no sl.log: the Streamline log environment did not take effect')
if not nrefused or '0x1' in nmasks:
    raise SystemExit('the negative control was admitted: SimulatePreAda=2 does not reproduce an RTX 30 refusal')
if refused or '0x1' not in masks or not multi:
    raise SystemExit('the admission did not keep the DLSS-G plugin on the simulated RTX 30')
# 1.10.0 (dashdogy item A1): NGX's requirements query failing while Streamline
# starts, as FeatureNotFound (no provider mapped yet) and InvalidParameter (the
# driver refusing a project ID). sl.dlss_g then disables DLSS-G even with the
# Ada architecture answer (the control, PreAdaRequirementsAnswer=0); with the
# addon's answer the plugin must keep the adapter.
fok, fmasks, frefused, _, fnotes, _ = run('requirements-notfound-off', True, 1, answer=False)
if not fok or not fnotes:
    raise SystemExit('requirements-notfound-off: no sl.log, or the addon did not see the failing query')
if not frefused or '0x1' in fmasks:
    raise SystemExit('requirements-notfound-off: the control kept DLSS-G; the failure no longer matters to Streamline')
for name, code, expected in (('requirements-notfound', 1, '0xbad00004'), ('requirements-identifier', 2, '0xbad00005')):
    rok, rmasks, rrefused, rmulti, rnotes, rfallback = run(name, True, code)
    if not rok:
        raise SystemExit(name + ': no sl.log')
    if not any(expected in n for n in rnotes):
        raise SystemExit(name + ': the addon did not see a failing requirements query (its hook was not in place in time)')
    if 'answered supported' not in rnotes:
        raise SystemExit(name + ': the addon did not answer the failing query')
    if rrefused or '0x1' not in rmasks or not rmulti:
        raise SystemExit(name + ': the DLSS-G plugin was dropped after the requirements query failed')
print('PASS: refused without the admission, admitted with multi-frame support with it, '
      "and still admitted when NGX's requirements query fails (FeatureNotFound, InvalidParameter)")
