# -*- coding: utf-8 -*-
"""i18n metadata audit: extract display-metadata strings (Title/Desc/Group/hints/
labels/warnings/fields) from module files + ui strings, check against kEn keys."""
import re, io, os, json
from collections import defaultdict

ROOT = 'src'
LANG = os.path.join(ROOT, 'lang.cpp')
def read(p): return io.open(p, 'r', encoding='utf-8', errors='replace').read()
def strip(code):
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
    code = re.sub(r'//[^\n]*', '', code)
    return code
CJK = re.compile(r'[\u4e00-\u9fff\u3400-\u4dbf]')

keyset = set()
for m in re.finditer(r'\{\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"(?:[^"\\]|\\.)*"\s*\}', read(LANG)):
    keyset.add(m.group(1))

def unesc(s):
    # lang.cpp stores keys as raw source text too, so keep escapes as-is for compare
    return s

missing = defaultdict(set)   # string -> set of contexts
add = lambda s, ctx: missing[s].add(ctx)

SLOT = r'(Title|Description|Group|RunLabel|ApplyLabel|UndoLabel|ApplyWarning|PathHint|TargetHint|GroupTitle)'

modfiles = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d != 'res']
    for fn in filenames:
        if fn.endswith(('.cpp', '.h')):
            modfiles.append(os.path.join(dirpath, fn))
modfiles = [p for p in modfiles if os.path.normpath(p) != os.path.normpath(LANG)]

for p in modfiles:
    code = strip(read(p))
    rel = os.path.relpath(p, ROOT).replace('\\', '/')
    # 1) slot accessors: return L"...";
    for m in re.finditer(SLOT + r'\s*\(\s*\)\s*const(?:\s*override)?\s*\{\s*(?:return\s+)?(L"(?:[^"\\\n]|\\.)*")', code):
        s = m.group(2)
        if CJK.search(s):
            add(s, '%s:%s()' % (rel, m.group(1)))
    # 2) ModuleAction{ L"label", L"warning", ... }  and Named init .label = L".."
    for m in re.finditer(r'ModuleAction\{([^}]*)\}', code):
        body = m.group(1)
        for i, part in enumerate(body.split(',')):
            pm = re.match(r'\s*L"((?:[^"\\]|\\.)*)"\s*$', part)
            if pm and CJK.search(pm.group(1)):
                # first string = label, second = warning
                ctx = 'label' if ('label' not in body.split(pm.group(1))[0]) else 'warn'
                # crude: count preceding strings
                n_before = len(re.findall(r'L"(?:[^"\\]|\\.)*"', body.split('L"' + pm.group(1) + '"')[0]))
                add('L"%s"' % pm.group(1), '%s:action#%d' % (rel, n_before))
    # 3) ModuleField{ L"label", L"hint", ... }
    for m in re.finditer(r'ModuleField\{([^}]*)\}', code):
        strs = re.findall(r'L"((?:[^"\\]|\\.)*)"', m.group(1))
        for i, s in enumerate(strs):
            if CJK.search(s):
                add('L"%s"' % s, '%s:field#%d' % (rel, i))
    # 4) named .label= / .warning= / .hint= / .label= assignments
    for m in re.finditer(r'\.(label|warning|hint)\s*=\s*(L"(?:[^"\\\n]|\\.)*")', code):
        s = m.group(2)
        if CJK.search(s):
            add(s, '%s:.%s' % (rel, m.group(1)))

print('=== METADATA-STRING KEYS MISSING FROM kEn: %d ===' % len(missing))
for s in sorted(missing):
    print(json.dumps(s, ensure_ascii=False), '  <-', ', '.join(sorted(missing[s])[:3]))
