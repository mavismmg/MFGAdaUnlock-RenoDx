# SPDX-License-Identifier: MIT
"""1.10.0 (plan 5): keep the panel's language tables in step with
src/addons/mfgunlock/i18n/string_keys.hpp.

Each strings_<lang>.inl line is `/* kKey */ "text",` or `/* kKey */ nullptr,`.
This rewrites every table in the enum's order: kept keys keep their text, new
keys get nullptr (English is shown until someone translates them), removed keys
go. Optional translations for new or changed keys come from JSON files:

  python tools/sync_panel_tables.py                       # sync only
  python tools/sync_panel_tables.py --add ja=new_ja.json  # also set texts
  python tools/sync_panel_tables.py --check               # fail if out of step
"""
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "addons", "mfgunlock", "i18n")
LANGS = ["pt_br", "es", "ja", "ko", "zh_cn"]
LINE = re.compile(r'^/\* (k\w+) \*/ (nullptr|".*"),$')
HEADER = re.compile(r"^//")


def keys():
    text = open(os.path.join(ROOT, "string_keys.hpp"), encoding="utf-8").read()
    body = text[text.index("enum class Str"):text.index("kCount")]
    return re.findall(r"^\s+(k\w+),", body, re.M)


def c_escape(s):
    out = s.replace(chr(92), chr(92) * 2).replace(chr(34), chr(92) + chr(34)).replace(chr(10), chr(92) + "n")
    return out.replace(chr(0), chr(92) + "0" + chr(34) + chr(34))


def read(lang):
    path = os.path.join(ROOT, f"strings_{lang}.inl")
    header, entries = [], {}
    for line in open(path, encoding="utf-8").read().split("\n"):
        m = LINE.match(line)
        if m:
            entries[m.group(1)] = m.group(2)
        elif HEADER.match(line) and not entries:
            header.append(line)
    return path, header, entries


def main():
    order = keys()
    adds = {}
    args = sys.argv[1:]
    for i, arg in enumerate(args):
        if arg == "--add":
            lang, path = args[i + 1].split("=", 1)
            adds[lang] = json.load(open(path, encoding="utf-8"))
    out_of_step = []
    for lang in LANGS:
        path, header, entries = read(lang)
        for key, text in adds.get(lang, {}).items():
            if key not in order:
                sys.exit(f"{lang}: unknown key {key}")
            entries[key] = '"' + c_escape(text) + '"'
        lines = header + [f"/* {k} */ {entries.get(k, 'nullptr')}," for k in order] + [""]
        new = "\n".join(lines)
        old = open(path, encoding="utf-8").read()
        if new != old:
            out_of_step.append(lang)
            if "--check" not in args:
                open(path, "w", encoding="utf-8", newline="\n").write(new)
        missing = sum(1 for k in order if entries.get(k, "nullptr") == "nullptr")
        print(f"{lang}: {len(order) - missing} of {len(order)} translated")
    if "--check" in args and out_of_step:
        sys.exit(f"tables out of step with string_keys.hpp: {out_of_step}; run tools/sync_panel_tables.py")
    print("PASS" if "--check" in args else "synced")


main()
