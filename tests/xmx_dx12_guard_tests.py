# SPDX-License-Identifier: MIT
"""1.10.0 (plan XMX/Vulkan/VRAM, section 1): XMX on Direct3D 12 works today, and
the Vulkan and forced frame generation work must not change it. This fails when
its two files change. A change that is meant (and approved) updates the hashes
below in the same commit, with the reason in the commit message.
"""
import hashlib, pathlib, sys

root = pathlib.Path(__file__).resolve().parents[1] / 'src' / 'addons' / 'mfgunlock'
PINNED = {
    # 603a4fa: passive copy-size counters for the VRAM measurement.
    # 1.10.0 (user-approved): XeFG UI composition opt-in (XmxUiComposition); the Streamline host's HUD fix
    # (xmx_hud_composite.hpp, its own HUD-less copy, HUD debug); Metro Exodus HDR fix (only a copy captured for
    # this frame, composition only at the back buffer's size); the composition follows the panel's HUD fix; 1.10.0 rename XMX -> XeMFG DLSS-G in a message.
    'xmx_generator.hpp': '2224c07a2a8bee6c8e84b7dc0d5a928631115727515b2392584b3ab97b2296f0',
    # 1.10.0 (user-approved): EvaluateWithoutNvidia, NGX's own Evaluate for XMX's handle when
    # NVIDIA's unused feature is not created (addon_provider.inl, [DLSSG] XmxSaveVram).
    'xmx_hook.hpp': '735014c5a16584da455a099b56c3fc054ff4f1bdf7510281bba1692f4b4682ce',
}
failed = False
for name, pinned in PINNED.items():
    digest = hashlib.sha256((root / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if digest != pinned:
        print(f'FAIL: {name} changed ({digest}); XMX on D3D12 is guarded, see the docstring')
        failed = True
if failed:
    sys.exit(1)
print('PASS: the D3D12 XMX files are unchanged')
