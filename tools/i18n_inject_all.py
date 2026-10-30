# -*- coding: utf-8 -*-
"""Unified i18n injection (v3): normalize dict keys/values to DECODED runtime text,
re-escape once for C++ output. Replaces the two earlier injection blocks."""
import io, re, json, ast

LANG = 'src/lang.cpp'

def decode_filetext(s):
    """file-text escape sequences -> decoded runtime text (idempotent for already-decoded)."""
    return (s.replace('\\\\', '\x00')
             .replace('\\n', '\n')
             .replace('\\t', '\t')
             .replace('\\"', '"')
             .replace('\x00', '\\'))

def cpp_escape(s):
    """decoded runtime text -> C++ string-literal body."""
    return (s.replace('\\', '\\\\')
             .replace('\n', '\\n')
             .replace('\t', '\\t')
             .replace('"', '\\"'))

# ---- gather dicts from the two injector scripts ----
def extract_dict(path):
    src = io.open(path, encoding='utf-8').read()
    i = src.index('T = {')
    j = src.index('\n}', i)
    return ast.literal_eval(src[i + 4:j + 2])

T1 = extract_dict('tools/i18n_inject.py')
T2 = extract_dict('tools/i18n_inject2.py')

# decoded text universe
Tdec = {}
for src in (T1, T2):
    for k, v in src.items():
        Tdec[decode_filetext(k)] = decode_filetext(v)

# allowed keys = missing sets (file text) decoded
allowed = set()
for f in ('tools/i18n_missing.json', 'tools/i18n_missing2.json'):
    for s in json.load(io.open(f, encoding='utf-8')):
        allowed.add(decode_filetext(s))

# drop junk not in allowed (e.g. 马拉丁) and duplicates
junk = [k for k in Tdec if k not in allowed]
for k in junk:
    del Tdec[k]
print('junk dropped:', junk)
print('entries to inject:', len(Tdec))

# ---- strip previous injection blocks from lang.cpp ----
lang = io.open(LANG, encoding='utf-8').read()
for marker in ('    // ---- v2.18 i18n：菜单/标签/提示/警告全量英文（tools/i18n_inject.py 生成）----',
               '    // ---- v2.18 i18n 第二批：构造参数式元数据 + 主题名（tools/i18n_inject2.py 生成）----'):
    if marker in lang:
        i = lang.index(marker)
        j = lang.index('\n};', i)
        lang = lang[:i].rstrip('\n') + '\n' + lang[j + 4:]
        # careful: keep the '};' line itself
        lang = lang[:i].rstrip('\n') + '\n};\n' + lang[j + 4:] if False else lang

# (the above inline trickery is fragile; do it cleanly:)
def strip_blocks(text):
    out = text
    for marker in ('    // ---- v2.18 i18n：菜单/标签/提示/警告全量英文（tools/i18n_inject.py 生成）----',
                   '    // ---- v2.18 i18n 第二批：构造参数式元数据 + 主题名（tools/i18n_inject2.py 生成）----'):
        while marker in out:
            i = out.index(marker)
            j = out.index('\n};', i)
            out = out[:i].rstrip('\n') + '\n};\n' + out[j + 4:]
    return out

lang = strip_blocks(io.open(LANG, encoding='utf-8').read())

# ---- dedupe against original table (decoded keys) ----
orig = set()
for m in re.finditer(r'\{\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"(?:[^"\\]|\\.)*"\s*\}', lang[:lang.index('const wchar_t* LookupEn')]):
    orig.add(decode_filetext(m.group(1)))
for k in list(Tdec):
    if k in orig:
        del Tdec[k]
print('after dedupe vs original:', len(Tdec))

# ---- emit ----
lines = ['']
lines.append('    // ---- v2.18 i18n：菜单/标签/提示/警告/元数据全量英文（tools/i18n_inject_all.py 生成）----')
for k in sorted(Tdec):
    lines.append('    { L"%s", L"%s" },' % (cpp_escape(k), cpp_escape(Tdec[k])))
block = '\n'.join(lines) + '\n'

m = None
for m in re.finditer(r'\n\};\n', lang[:lang.index('const wchar_t* LookupEn')]):
    pass
pos = m.start() + 1
io.open(LANG, 'w', encoding='utf-8', newline='').write(lang[:pos] + block + lang[pos:])
print('lang.cpp updated')

# ---- verify: no duplicate decoded keys, no doubled backslashes ----
final = io.open(LANG, encoding='utf-8').read()
pairs = re.findall(r'\{\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"(?:[^"\\]|\\.)*"\s*\}', final)
dec = [decode_filetext(p) for p in pairs]
dups = [k for k in set(dec) if dec.count(k) > 1]
print('total pairs:', len(pairs), 'dup decoded:', len(dups))
for d in dups[:8]:
    print('  DUP:', repr(d[:70]))
bad = [p for p in pairs if '\\\\\\\\' in p]
print('doubled-backslash lines:', len(bad))
