# -*- coding: utf-8 -*-
"""i18n audit: find CJK string literals in src/ that are missing from lang.cpp kEn table."""
import re, io, os, sys, json

ROOT = os.path.join(os.path.dirname(__file__), '..', 'src')
LANG = os.path.join(ROOT, 'lang.cpp')

def read(p):
    return io.open(p, 'r', encoding='utf-8', errors='replace').read()

def strip_comments(code):
    # remove // line comments and /* */ block comments (naive but ok: no string contains "//" w/ CJK risk)
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
    code = re.sub(r'//[^\n]*', '', code)
    return code

CJK = re.compile(r'[\u4e00-\u9fff\u3400-\u4dbf]')

# 1) collect kEn keys
lang_src = read(LANG)
# Pair entries: { L"zh", L"en" },
keys = []
for m in re.finditer(r'\{\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"((?:[^"\\]|\\.)*)"\s*\}', lang_src):
    keys.append(m.group(1))
keyset = set(keys)
from collections import Counter
dup = [k for k, c in Counter(keys).items() if c > 1]

# 2) scan source files for CJK string literals
missing_wrapped = {}   # string -> [(file,line)] passed to T(/Tx( but not in kEn
missing_raw = {}       # string -> [(file,line)] CJK literal not wrapped
skip_dirs = set()
results = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d not in ('res',)]
    for fn in filenames:
        if not fn.endswith(('.cpp', '.h')):
            continue
        p = os.path.join(dirpath, fn)
        if os.path.normpath(p) == os.path.normpath(LANG):
            continue
        code = strip_comments(read(p))
        # find string literals with CJK, and whether preceded by T( or Tx(
        for m in re.finditer(r'L?"((?:[^"\\\n]|\\.)*)"', code):
            s = m.group(1)
            if not CJK.search(s):
                continue
            line = code[:m.start()].count('\n') + 1
            before = code[max(0, m.start()-4):m.start()]
            wrapped = before.endswith(('T(', 'x('))  # Tx( covered by x(
            # unescape minimal (\n -> \n literal for key compare: keys in lang.cpp raw text)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            if wrapped and s not in keyset:
                missing_wrapped.setdefault(s, []).append((rel, line))
            elif not wrapped:
                missing_raw.setdefault(s, []).append((rel, line))

def unesc(s):
    return s.replace('\\n', ' ').replace('\\t', ' ').replace('\\"', '"').replace('\\\\', '\\')

print('=== kEn keys: %d, duplicates: %d ===' % (len(keys), len(dup)))
for d in dup[:20]:
    print('  DUP:', d)
print()
print('=== MISSING (wrapped in T/Tx but no kEn key): %d ===' % len(missing_wrapped))
for s in sorted(missing_wrapped):
    locs = missing_wrapped[s][:3]
    print(json.dumps(s, ensure_ascii=False), ' <-', ', '.join('%s:%d' % l for l in locs))
print()
print('=== RAW CJK literals (not wrapped) count: %d (top 400) ===' % len(missing_raw))
for s in sorted(missing_raw)[:400]:
    locs = missing_raw[s][:2]
    print(json.dumps(s, ensure_ascii=False), ' <-', ', '.join('%s:%d' % l for l in locs))
