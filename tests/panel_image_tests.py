# SPDX-License-Identifier: MIT
# 1.10.0 (plan 3.2 and 5): image regression of the XeMFG panel in every
# language. The panel preview (tests/xemfg_panel_preview.cpp) renders the real
# widgets with a synthetic status; tests/image_ssim.cpp compares each render
# with its reference in tests/fixtures/ui-golden. A layout change, a missing
# glyph or a clipped translation lowers the worst window's SSIM.
#
#   python tests/panel_image_tests.py --preview <exe> --ssim <exe> --work <dir>
#   ... --update     writes the references after a deliberate change
#
# The references were made on the development PC (Windows 11 fonts, its GPU
# name in the System card); a different PC changes those pixels, so a missing
# font or a missing reference skips with exit 77 instead of failing.
import argparse, os, pathlib, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--preview', type=pathlib.Path, required=True)
ap.add_argument('--ssim', type=pathlib.Path, required=True)
ap.add_argument('--work', type=pathlib.Path, required=True)
ap.add_argument('--update', action='store_true')
a = ap.parse_args()
golden = pathlib.Path(__file__).resolve().parent / 'fixtures' / 'ui-golden'
fonts = pathlib.Path(os.environ.get('WINDIR', r'C:\Windows')) / 'Fonts'
# (language, page, font file or None for ImGui's default font)
CASES = [('en', 0, None), ('pt-BR', 1, None), ('es', 2, None),
         ('ja', 0, 'msgothic.ttc'), ('ko', 2, 'malgun.ttf'), ('zh-CN', 3, 'msyh.ttc')]
a.work.mkdir(parents=True, exist_ok=True)
failures, skipped = [], []
for language, page, font in CASES:
    name = f'page{page}-{language}.png'
    env = dict(os.environ, MFG_PREVIEW_LANGUAGE=language)
    if font:
        if not (fonts / font).exists():
            skipped.append(f'{name}: {font} is not installed'); continue
        env['MFG_PREVIEW_FONT'] = str(fonts / font)
    else:
        env.pop('MFG_PREVIEW_FONT', None)
    out = a.work / name
    run = subprocess.run([str(a.preview), str(out), str(page), '560'], env=env, capture_output=True, text=True)
    if run.returncode != 0 or not out.exists():
        failures.append(f'{name}: preview failed ({run.returncode}) {run.stderr.strip()}'); continue
    reference = golden / name
    if a.update:
        golden.mkdir(parents=True, exist_ok=True)
        shutil.copy2(out, reference)
        print('updated', reference.name); continue
    if not reference.exists():
        skipped.append(f'{name}: no reference (run with --update)'); continue
    cmp = subprocess.run([str(a.ssim), str(reference), str(out), '0.985', '0.80'], capture_output=True, text=True)
    print(f'{name}: {cmp.stdout.strip()}')
    if cmp.returncode != 0:
        failures.append(f'{name}: {cmp.stdout.strip()} (render kept at {out})')
for line in skipped: print('SKIP', line)
if failures:
    print('\n'.join(failures)); sys.exit(1)
if skipped and len(skipped) == len(CASES):
    sys.exit(77)
print('PASS' if not a.update else 'references updated')
