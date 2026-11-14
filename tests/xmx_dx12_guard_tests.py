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
    'xmx_generator.hpp': '2421be9d9dd72e1e037754fe60762f6ebe08215c96aba1e80e85c53c93991eef',
    'xmx_hook.hpp': '33d53f96c18ef71cb172634f141dbced09142b81b42fca432bfb8448c1c840eb',
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
