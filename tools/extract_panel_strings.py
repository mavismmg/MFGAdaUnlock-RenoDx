"""Block 5 tooling: find the panel's user-visible string literals that are whole
arguments of UI calls, replace them with T(Str::k...) (advanced_settings.hpp
tables and label arrays: Str::k... / T(...) with the constexpr dropped) and
generate the key enum and the English table. Dry run writes to an output
directory; --apply writes into the tree.

Out of scope on purpose: fragments joined with + or <<, the support reports,
log lines, INI/config keys, ImGui ids, the unused legacy overlay. They stay
English.

usage:
  python tools/extract_panel_strings.py --check
      fails when a panel file has user-visible text outside the tables (the
      automated suite runs this);
  python tools/extract_panel_strings.py <src dir> <out dir> [--apply]
      moves new text into string_keys.hpp and strings_en.inl; then run
      tools/sync_panel_tables.py so the other languages get the new keys.
"""
import os, re, sys, collections, json

check = "--check" in sys.argv
if check:
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "addons", "mfgunlock")
    out = None
else:
    src = sys.argv[1]
    out = sys.argv[2]
apply = "--apply" in sys.argv
files = ["addon_overlay.inl", "overlay_page_general.inl", "overlay_page_quality.inl", "overlay_page_latency.inl",
         "overlay_page_support.inl", "overlay_page_dlssinfo.inl", "panel_ui.hpp", "advanced_settings.hpp",
         os.path.join("xemfg", "native_panel.cpp"), os.path.join("xemfg", "native_ui.cpp"),
         os.path.join("xemfg", "native_controls.cpp"), os.path.join("xemfg", "dependency_manager.cpp"),
         "dlssg_latency.hpp"]
TABLE_FILES = {"advanced_settings.hpp"}

lit_run = re.compile(r'(?<![L\w])"((?:[^"\\\n]|\\.)*)"(?:\s*"((?:[^"\\\n]|\\.)*)")*')
single = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ARRAY_DECL = re.compile(r"^([ \t]*)(?:static )?constexpr const char\* (?:const )?(k\w+)\[\]\s*=\s*\{|^([ \t]*)static const char\* const (k\w+)\[\]\s*=\s*\{", re.M)

def unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")

EXCLUDE_LINE = re.compile(r"reshade::log::message|GetPrivateProfile|WritePrivateProfile|set_config_value|get_config_value|"
                          r"#include|#error|#pragma|SPDX|std::printf|snprintf|wprintf|\breport\b|\bfields\s*\+=|WriteLog|"
                          r"GetProcAddress|GetModuleHandle|LoadLibrary|strcmp|_stricmp|\.rfind\(|\.find\(|std::wstring|L\"|"
                          r"constexpr|static_assert|register_overlay|ImGui::PushID|ImGui::BeginChild|ImGui::Begin\(|"
                          r"SetClipboardText|std::ofstream|std::ifstream|CreateFileW|_wfopen|\.ini\b|\.log\b")
FUNC_HEADER = re.compile(r"^(?:[\w:<>*&\s]+?)\s+(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{\s*$", re.M)

def visible(raw):
    s = raw
    if s.startswith("##") or len(s) < 2: return False
    if "mfgunlock:" in s or "mfgunlock::" in s: return False
    if not re.search(r"[A-Za-z]", s): return False
    if re.fullmatch(r"[\w.\-/=%:+*]+", s) and " " not in s and not re.fullmatch(r"[A-Z][a-z]+", s): return False
    if s.startswith("\\n") or s.startswith("\n"): return False
    return True

def whole_argument(text, a, b):
    i = a - 1
    while i >= 0 and text[i] in " \t\n": i -= 1
    before = text[i] if i >= 0 else ""
    before2 = text[i - 1:i + 1] if i >= 1 else ""
    j = b
    while j < len(text) and text[j] in " \t\n": j += 1
    after = text[j] if j < len(text) else ""
    after2 = text[j:j + 2]
    if before == "+" or before2 in ("<<", "+=") or after == "+" or after2 == "<<" or after == "[":
        return False
    if after == "#":  # continued in a preprocessor branch: a fragment
        return False
    if before in "(,?:=" or before == "{":
        return True
    return False

def slug(text):
    words = re.findall(r"[A-Za-z0-9]+", text)[:5]
    base = "".join(w[:1].upper() + w[1:].lower() for w in words) or "Text"
    h = 2166136261
    for c in text.encode("utf-8"):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return f"k{base}_{h & 0xFFFF:04x}"

keys = collections.OrderedDict()
per_file = {}
stats = collections.Counter()
for rel in files:
    p = os.path.join(src, rel)
    if not os.path.exists(p): continue
    text = open(p, encoding="utf-8").read()
    table = os.path.basename(rel) in TABLE_FILES
    headers = [(m.start(), m.group(1)) for m in FUNC_HEADER.finditer(text)]
    def excluded_function(pos):
        name = None
        for s, n in headers:
            if s <= pos: name = n
            else: break
        return name is not None and ("Report" in name or "report" in name or "Legacy" in name)
    # Label arrays: their items are translated and the declaration loses constexpr.
    arrays = []  # (decl_start, decl_end, items_end, new_decl)
    for m in ARRAY_DECL.finditer(text):
        indent = m.group(1) if m.group(1) is not None else m.group(3)
        name = m.group(2) or m.group(4)
        depth = 1; k = m.end()
        while k < len(text) and depth:
            if text[k] == "{": depth += 1
            elif text[k] == "}": depth -= 1
            k += 1
        if excluded_function(m.start()): continue
        arrays.append((m.start(), m.end(), k, f"{indent}const char* {name}[] = {{"))
    def in_array(pos):
        return any(s <= pos < e for s, _, e, _ in arrays)
    edits = []
    for m in lit_run.finditer(text):
        span_text = text[m.start():m.end()]
        raw = "".join(single.findall(span_text))
        ls = text.rfind("\n", 0, m.start()) + 1
        le = text.find("\n", m.end()); le = len(text) if le < 0 else le
        line = text[ls:le]
        if "//" in text[ls:m.start()]: continue
        stats["literals"] += 1
        table_line = table and (line.lstrip().startswith("{L\"") or "Choice k" in line)
        if table_line:
            pass
        else:
            if not visible(raw): continue
            if excluded_function(m.start()): stats["excluded_function"] += 1; continue
            # The exclusions look at the code only: a help text that mentions
            # "report" or ".ini" is still text for the player.
            code = single.sub('""', line)
            if not in_array(m.start()) and EXCLUDE_LINE.search(code): stats["excluded_line"] += 1; continue
            if not in_array(m.start()) and not whole_argument(text, m.start(), m.end()): stats["fragment"] += 1; continue
        suffix = ""
        if "##" in raw:
            raw, suffix = raw.split("##", 1); suffix = "##" + suffix
            if not raw.strip(): continue
        try: english = unescape(raw)
        except Exception: stats["unescape_failed"] += 1; continue
        k = slug(english)
        if k in keys and keys[k] != english: stats["hash_clash"] += 1; k += "x"
        keys[k] = english
        rep = f"Str::{k}" if table_line else f"T(Str::{k})"
        if suffix: rep = f'(std::string(T(Str::{k})) + "{suffix}").c_str()'
        edits.append((m.start(), m.end(), rep)); stats["replaced"] += 1
    for s, e, _, new_decl in arrays:
        edits.append((s, e, new_decl)); stats["arrays"] += 1
    for a, b, rep in sorted(edits, key=lambda x: x[0], reverse=True):
        text = text[:a] + rep + text[b:]
    per_file[rel] = (text, len(edits))

print("literals:", stats["literals"], "replaced:", stats["replaced"], "keys:", len(keys), "arrays:", stats["arrays"],
      "fragments:", stats["fragment"], "excluded lines:", stats["excluded_line"], "excluded functions:", stats["excluded_function"],
      "clashes:", stats["hash_clash"])
for rel, (_, n) in per_file.items(): print(f"  {rel:35s} {n:4d}")

if check:
    found = [(rel, n) for rel, (_, n) in per_file.items() if n]
    if found:
        print("panel text outside the language tables:", found)
        print("run tools/extract_panel_strings.py <src> <out> --apply, then tools/sync_panel_tables.py")
        sys.exit(1)
    print("PASS: every user-visible panel string is in the language tables")
    sys.exit(0)

def c_escape(s):
    # NUL separators of ImGui::Combo lists: close and reopen the literal so the
    # next character can never extend the escape.
    out = s.replace(chr(92), chr(92) * 2).replace(chr(34), chr(92) + chr(34)).replace(chr(10), chr(92) + 'n')
    return out.replace(chr(0), chr(92) + '0' + chr(34) + chr(34))
# The tables grow: keys already in string_keys.hpp keep their place and English
# text (some, like the language selector's, are written by hand), new keys go
# at the end. tools/sync_panel_tables.py then gives the other languages nullptr
# for them until someone translates them.
existing_order, existing_text = [], {}
i18n_src = os.path.join(src, "i18n")
if os.path.exists(os.path.join(i18n_src, "string_keys.hpp")):
    body = open(os.path.join(i18n_src, "string_keys.hpp"), encoding="utf-8").read()
    body = body[body.index("enum class Str"):body.index("kCount")]
    existing_order = re.findall(r"^\s+(k\w+),", body, re.M)
    for line in open(os.path.join(i18n_src, "strings_en.inl"), encoding="utf-8").read().split("\n"):
        m = re.match(r'^/\* (k\w+) \*/ (".*"),$', line)
        if m: existing_text[m.group(1)] = m.group(2)
added = [k for k in keys if k not in existing_text]
order = existing_order + added
entry = {k: existing_text[k] for k in existing_order if k in existing_text}
entry.update({k: '"' + c_escape(keys[k]) + '"' for k in added})
print(f"keys: {len(existing_order)} kept, {len(added)} added")
enum_lines = ["// SPDX-License-Identifier: MIT", "// 1.10.0 (plan 5.1): generated by tools/extract_panel_strings.py; the key",
              "// and its English text in strings_en.inl change together.", "#pragma once", "", "namespace mfgunlock::i18n {",
              "enum class Str : unsigned {"] + [f"  {k}," for k in order] + ["  kCount", "};", "}  // namespace mfgunlock::i18n", ""]
en_lines = ["// SPDX-License-Identifier: MIT", "// 1.10.0 (plan 5.1): English, the base text. One entry per Str, in enum order."] + \
           [f"/* {k} */ {entry[k]}," for k in order] + [""]
os.makedirs(out, exist_ok=True)
for rel, (text, n) in per_file.items():
    if n == 0: continue
    dst = os.path.join(src if apply else out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "w", encoding="utf-8", newline="\n").write(text)
i18n_dir = os.path.join(src if apply else out, "i18n")
os.makedirs(i18n_dir, exist_ok=True)
open(os.path.join(i18n_dir, "string_keys.hpp"), "w", encoding="utf-8", newline="\n").write("\n".join(enum_lines))
open(os.path.join(i18n_dir, "strings_en.inl"), "w", encoding="utf-8", newline="\n").write("\n".join(en_lines))
json.dump({k: keys[k] for k in added}, open(os.path.join(out, "strings_en_added.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("written to", src if apply else out)
