# SPDX-License-Identifier: MIT
"""Build NVIDIA's Streamline_Sample against a given Streamline SDK release.

python tools/sl_sample.py build --sample <checkout> --sdk-zip <streamline-sdk-vX.zip>

The sample checkout must match the SDK version (NVIDIA-RTX/Streamline_Sample
tags follow Streamline tags). DirectX 12 only: Vulkan is disabled and the
Windows SDK dxc.exe compiles DXIL. A local, uncommitted patch lets the
environment variable MFG_SAMPLE_DLSSG=1 start with DLSS-G and Reflex on,
so a test can run without UI interaction. Nothing here is redistributed.
"""
import argparse, glob, os, pathlib, re, shutil, subprocess, sys, zipfile

PATCH_MARK = '// MFGAdaUnlock test patch'
HELPER = PATCH_MARK + ': MFG_SAMPLE_DLSSG=1 starts with DLSS-G and Reflex enabled.\n' \
    'inline bool MfgSampleAutoFG() { char* v=nullptr; size_t n=0; _dupenv_s(&v,&n,"MFG_SAMPLE_DLSSG");\n' \
    '  const bool on=v && v[0]==\'1\'; free(v); return on; }\n'


def dxc():
    found = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\dxc.exe'))
    if not found:
        raise SystemExit('dxc.exe not found in the Windows SDK')
    return found[-1]


def extract(sample, archive):
    target = sample / 'streamline'
    if (target / 'include' / 'sl.h').is_file():
        return
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        # Releases either place include/ at the root or under one folder.
        prefix = min((n[:n.index('include/')] for n in names if n.endswith('include/sl.h')), key=len)
        for name in names:
            if not name.startswith(prefix) or name.endswith('/'):
                continue
            out = target / name[len(prefix):]
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(name) as src, open(out, 'wb') as dst:
                shutil.copyfileobj(src, dst)


def patch(sample):
    ui = sample / 'src' / 'UIData.h'
    text = ui.read_text(encoding='utf-8')
    if PATCH_MARK in text:
        return
    text, fg = re.subn(r'DLSSG_mode\s*=\s*sl::DLSSGMode::eOff', 'DLSSG_mode = MfgSampleAutoFG()?sl::DLSSGMode::eOn:sl::DLSSGMode::eOff', text)
    text, rx = re.subn(r'REFLEX_Mode\s*=\s*static_cast<int>\(sl::ReflexMode::eOff\)',
                       'REFLEX_Mode = static_cast<int>(MfgSampleAutoFG()?sl::ReflexMode::eLowLatency:sl::ReflexMode::eOff)', text)
    if fg != 1 or rx != 1:
        raise SystemExit(f'UIData.h layout not recognized (DLSS-G {fg}, Reflex {rx})')
    include = text.index('#include')
    text = text[:include] + '#include <cstdlib>\n' + HELPER + text[include:]
    ui.write_text(text, encoding='utf-8')


SR_MARK = '// MFGAdaUnlock SR-only patch'
SR_HELPER = SR_MARK + ': MFG_SAMPLE_SR_ONLY=1 makes the sample a game that integrates Streamline\n' \
    '// for DLSS Super Resolution only: no DLSS-G in slInit, no DLSS-G calls; Reflex stays on.\n' \
    'inline bool MfgSampleSrOnly() { char* v=nullptr; size_t n=0; _dupenv_s(&v,&n,"MFG_SAMPLE_SR_ONLY");\n' \
    '  const bool on=v && v[0]==\'1\'; free(v); return on; }\n'


def patch_sr_only(sample):
    """1.10.0 (plan XMX/Vulkan/VRAM, item 4): the forced DLSS-G test needs a game without DLSS-G."""
    ui = sample / 'src' / 'UIData.h'
    text = ui.read_text(encoding='utf-8')
    if SR_MARK not in text:
        old = 'MfgSampleAutoFG()?sl::ReflexMode::eLowLatency'
        if text.count(old) != 1:
            raise SystemExit('UIData.h: the DLSS-G test patch is not in place')
        text = text.replace(old, '(MfgSampleAutoFG()||MfgSampleSrOnly())?sl::ReflexMode::eLowLatency')
        helper_at = text.index(PATCH_MARK)
        text = text[:helper_at] + SR_HELPER + text[helper_at:]
        ui.write_text(text, encoding='utf-8')
    # The sample's own wrapper (src/SLWrapper.cpp) calls slInit; donut's StreamlineIntegration is unused.
    integration = sample / 'src' / 'SLWrapper.cpp'
    text = integration.read_text(encoding='utf-8')
    if SR_MARK in text:
        return
    load = 'pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));'
    available = 'm_dlssg_available = slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo) == sl::Result::eOk;'
    if text.count(load) != 1 or text.count(available) != 1:
        raise SystemExit('SLWrapper.cpp layout not recognized')
    # Its own helper name: SLWrapper.cpp may also see UIData.h's MfgSampleSrOnly.
    text = text.replace(load, load + '\n    static std::vector<sl::Feature> mfgSrOnly;\n'
                        '    if (MfgWrapperSrOnly()) {\n'
                        '        for (auto f : featuresToLoad) if (f != sl::kFeatureDLSS_G) mfgSrOnly.push_back(f);\n'
                        '        pref.featuresToLoad = mfgSrOnly.data();\n'
                        '        pref.numFeaturesToLoad = uint32_t(mfgSrOnly.size());\n'
                        '    }')
    # The sample unloads DLSS-G right after slInit and loads it when its UI turns frame
    # generation on; a game that never integrated DLSS-G makes neither call.
    unload = '        slSetFeatureLoaded(sl::kFeatureDLSS_G, false);\n'
    if text.count(unload) != 1:
        raise SystemExit('SLWrapper.cpp: the DLSS-G unload after slInit was not found')
    text = text.replace(unload, '        if (!MfgWrapperSrOnly()) slSetFeatureLoaded(sl::kFeatureDLSS_G, false);\n', 1)
    # No DLSS-G calls in SR-only mode. (Gating m_dlssg_available instead also stopped the
    # normal mode's DLSS-G from loading on this build, so the calls are gated where they are made.)
    for head in ('void SLWrapper::SetDLSSGOptions(const sl::DLSSGOptions consts) {\n',
                 'void SLWrapper::QueryDLSSGState(', 'void SLWrapper::CleanupDLSSG(bool wfi) {\n'):
        at = text.find(head)
        if at < 0:
            raise SystemExit('SLWrapper.cpp: ' + head.strip() + ' not found')
        body = text.index('{', at) + 1
        text = text[:body] + '\n    if (MfgWrapperSrOnly()) return;' + text[body:]
    include = text.index('#include')
    text = text[:include] + '#include <cstdlib>\n#include <vector>\n' + \
        SR_HELPER.replace('MfgSampleSrOnly', 'MfgWrapperSrOnly') + text[include:]
    integration.write_text(text, encoding='utf-8')


NR_MARK = '// MFGAdaUnlock no-Reflex patch'
NR_HELPER = NR_MARK + ': MFG_SAMPLE_NO_REFLEX=1 (with MFG_SAMPLE_SR_ONLY=1) makes the sample a game\n' \
    '// without Reflex (Ninja Gaiden 4): no Reflex in slInit, no Reflex options, sleep or PCL markers.\n' \
    'inline bool MfgWrapperNoReflex() { char* v=nullptr; size_t n=0; _dupenv_s(&v,&n,"MFG_SAMPLE_NO_REFLEX");\n' \
    '  const bool on=v && v[0]==\'1\'; free(v); return on; }\n'


def patch_no_reflex(sample):
    """1.10.1 (plan item 2): Route A's own Reflex needs a game that never uses Reflex."""
    integration = sample / 'src' / 'SLWrapper.cpp'
    text = integration.read_text(encoding='utf-8')
    if NR_MARK in text:
        return
    sr = '        for (auto f : featuresToLoad) if (f != sl::kFeatureDLSS_G) mfgSrOnly.push_back(f);\n'
    if text.count(sr) != 1:
        raise SystemExit('SLWrapper.cpp: the SR-only patch is not in place')
    text = text.replace(sr, '        for (auto f : featuresToLoad) if (f != sl::kFeatureDLSS_G && '
                        '!(MfgWrapperNoReflex() && (f == sl::kFeatureReflex || f == sl::kFeaturePCL))) mfgSrOnly.push_back(f);\n')
    # The frame token comes from the sleep callback: kept, the sleep itself is not.
    sleep = '        successCheck(slReflexSleep(*SLWrapper::Get().m_currentFrame), "Reflex_Sleep");\n'
    if text.count(sleep) != 1:
        raise SystemExit('SLWrapper.cpp: the Reflex sleep was not found')
    text = text.replace(sleep, '        if (!MfgWrapperNoReflex()) ' + sleep.lstrip())
    sleep_head = 'void SLWrapper::ReflexCallback_Sleep(donut::app::DeviceManager& manager, uint32_t frameID) {\n' \
                 '    if (SLWrapper::Get().GetReflexAvailable()) {'
    if text.count(sleep_head) != 1:
        raise SystemExit('SLWrapper.cpp: ReflexCallback_Sleep layout not recognized')
    text = text.replace(sleep_head, sleep_head.replace('if (SLWrapper::Get().GetReflexAvailable())',
                                                       'if (SLWrapper::Get().GetReflexAvailable() || MfgWrapperNoReflex())'))
    heads = ['void SLWrapper::SetReflexConsts(const sl::ReflexOptions options)\n'] + [
        f'void SLWrapper::ReflexCallback_{n}(donut::app::DeviceManager& manager, uint32_t frameID) {{'
        for n in ('SimStart', 'SimEnd', 'RenderStart', 'RenderEnd', 'PresentStart', 'PresentEnd')]
    for head in heads:
        at = text.find(head)
        if at < 0:
            raise SystemExit('SLWrapper.cpp: ' + head.strip() + ' not found')
        body = text.index('{', at + len(head) - 1) + 1
        text = text[:body] + '\n    if (MfgWrapperNoReflex()) return;' + text[body:]
    # After the SR-only helper, which follows <cstdlib>.
    helper_at = text.index(SR_MARK)
    text = text[:helper_at] + NR_HELPER + text[helper_at:]
    integration.write_text(text, encoding='utf-8')


def build(sample, archive):
    extract(sample, archive)
    patch(sample)
    patch_sr_only(sample)
    patch_no_reflex(sample)
    out = sample / '_build'
    subprocess.run(['cmake', '-S', str(sample), '-B', str(out), '-G', 'Visual Studio 17 2022', '-A', 'x64',
                    '-DUSE_SL=ON', '-DDONUT_WITH_VULKAN=OFF', '-DNVRHI_WITH_VULKAN=OFF', '-DDONUT_WITH_DX11=OFF',
                    # Newer donut reads SHADERMAKE_DXC_PATH; older ShaderMake searches the
                    # registry SDK root, which can differ from the installed bin folder.
                    '-DSHADERMAKE_FIND_DXC_VK=OFF', '-DSHADERMAKE_FIND_DXC_SPIRV=OFF', f'-DSHADERMAKE_DXC_PATH={dxc()}',
                    f'-DDXC_PATH={dxc()}', f'-DFXC_PATH={pathlib.Path(dxc()).with_name("fxc.exe")}'], check=True)
    subprocess.run(['cmake', '--build', str(out), '--config', 'Release', '--parallel', '8'], check=True)
    exes = [p for p in sample.rglob('StreamlineSample.exe') if 'Release' in str(p) or 'bin' in str(p)]
    if not exes:
        raise SystemExit('StreamlineSample.exe not produced')
    print(max(exes, key=lambda p: p.stat().st_mtime))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('command', choices=['build'])
    ap.add_argument('--sample', type=pathlib.Path, required=True)
    ap.add_argument('--sdk-zip', type=pathlib.Path, required=True)
    a = ap.parse_args()
    build(a.sample.resolve(), a.sdk_zip.resolve())
