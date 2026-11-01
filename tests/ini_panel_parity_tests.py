"""Every setting the addon reads must have a control in the panel (plan 1.8, D.3)."""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'src' / 'addons' / 'mfgunlock'
text = ''.join(p.read_text(encoding='utf-8', errors='ignore') for p in root.rglob('*')
               if p.suffix in ('.cpp', '.hpp', '.inl', '.h') and 'vendor' not in p.parts)

# Not settings: written by the addon itself, or replaced by a key that has a control.
INI_EXEMPT = {('XeFG.Learned', 'Executable')}
RESHADE_EXEMPT = {
    'DisocclusionGuardLevel',   # migrated to DepthEdgeGuardLevel
    'DynamicReflexSourceCap',   # migrated to ReflexSourceFpsCap
    'FixedOutputFpsCap',        # migrated to ReflexSourceFpsCap
    'ForceOTAPlugins',          # migrated to RuntimeSelectionMode
    'HDRUIRecomposition',       # migrated to HDRCompatibilityMode
    'SilhouetteBoundaryGuard',  # migrated to BoundaryArtifactMitigationMode
}
key = r'L"([\w.]+)",\s*L"(\w+)"'
read = set(re.findall(r'GetPrivateProfile(?:Int|String)W\(\s*' + key, text))
read |= set(re.findall(r'Read(?:Bool|Choice)\(' + key, text))
written = set(re.findall(r'\{' + key, text))
written |= set(re.findall(r'WritePrivateProfileStringW\(' + key, text))
missing = sorted(read - written - INI_EXEMPT)

reshade_read = set(re.findall(r'get_config_value\(\s*nullptr,\s*kConfigSection,\s*"(\w+)"', text))
reshade_written = set(re.findall(r'set_config_value\(\s*nullptr,\s*kConfigSection,\s*"(\w+)"', text))
reshade_missing = sorted(reshade_read - reshade_written - RESHADE_EXEMPT)

if len(read) < 30 or len(reshade_read) < 30:
    sys.exit(f'the scan found too few keys ({len(read)}, {len(reshade_read)}): the patterns are stale')
if missing or reshade_missing:
    sys.exit(f'settings without a panel control: {missing} {reshade_missing}')
print(f'{len(read)} INI and {len(reshade_read)} ReShade keys have panel controls')
