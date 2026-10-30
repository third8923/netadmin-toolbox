# -*- coding: utf-8 -*-
"""Wrap display-bound CJK literals with lang::T() in safe contexts.

Contexts (whitelist):
  A. module files: ActionWarning() function bodies (brace-tracked)
  B. module files: .hint = / .label = / .warning = assignment statements (to ';')
  C. module files: ModuleAction{ ... } / ModuleField{ ... } initializer literals
  D. cli.cpp / cleanresult.cpp / ui/dialog.cpp / ui/controls.cpp: CJK literals not already T(/Tx(-wrapped

Skips:
  - literals already preceded by T( or Tx( (4 chars back)
  - lines containing comparison ops (wcscmp|wcsstr|rfind(|find(|== L|!= L) in contexts A-C (safety net)
Emits a report of every wrap for review.
"""
import io, os, re, sys

ROOT = 'src'
CJK = re.compile(r'[\u4e00-\u9fff\u3400-\u4dbf]')
LIT = re.compile(r'L?"((?:[^"\\\n]|\\.)*)"')
CMP = re.compile(r'wcscmp|wcsncmp|wcsstr|\.rfind\(|\.find\(|==\s*L"|!=\s*L"')

def wrap_line(ln, report, path, lineno):
    """Add T(/Tx( around CJK literals on this logical line not already wrapped."""
    out = []
    i = 0
    changed = False
    for m in LIT.finditer(ln):
        s = m.group(0)
        body = m.group(1)
        if not CJK.search(body):
            continue
        before = ln[max(0, m.start()-4):m.start()]
        if before.endswith('T(') or before.endswith('x('):
            out.append((m.start(), m.end(), s, False))
            continue
        out.append((m.start(), m.end(), s, True))
        changed = True
    if not changed:
        return ln
    # rebuild from the end
    res = ln
    for st, en, s, doit in reversed(out):
        if not doit:
            continue
        # choose T( for plain literal; keep L prefix inside
        res = res[:st] + 'T(' + (s if s.startswith('L"') else 'L' + s) + ')' + res[en:]
        report.append('%s:%d: %s' % (path, lineno, s[:90]))
    return res

def process_module(path, report):
    code = io.open(path, encoding='utf-8', errors='replace').read()
    lines = code.split('\n')
    n = len(lines)
    wrapped_regions = []  # (start_line, end_line) inclusive, 0-based

    # A) ActionWarning bodies
    for m in re.finditer(r'ActionWarning\s*\([^)]*\)\s*const\s*\{', code):
        i = m.end() - 1  # at '{'
        depth = 0
        j = i
        while j < len(code):
            if code[j] == '{': depth += 1
            elif code[j] == '}':
                depth -= 1
                if depth == 0: break
            j += 1
        sl = code[:m.start()].count('\n')
        el = code[:j].count('\n')
        wrapped_regions.append((sl, el))

    # helper: find statement extents from a match of the trigger
    def stmt_extent(idx, openers=';{'):
        # expand to full logical statement: back to previous ; { } \n boundary-ish, forward to ;
        s = idx
        while s > 0 and code[s-1] not in ';\n{}':
            s -= 1
        e = idx
        while e < len(code) and code[e] != ';':
            e += 1
        return s, min(e, len(code)-1)

    triggers = []
    for m in re.finditer(r'\.(?:hint|label|warning)\s*=', code):
        triggers.append(stmt_extent(m.start()))
    for m in re.finditer(r'Module(?:Action|Field)\s*\{', code):
        i = code.index('{', m.start())
        depth = 0; j = i
        while j < len(code):
            if code[j] == '{': depth += 1
            elif code[j] == '}':
                depth -= 1
                if depth == 0: break
            j += 1
        triggers.append((m.start(), j))
    for s, e in triggers:
        sl = code[:s].count('\n'); el = code[:e].count('\n')
        wrapped_regions.append((sl, el))

    for sl, el in wrapped_regions:
        for ln in range(sl, min(el+1, n)):
            line = lines[ln]
            if CMP.search(line):
                report.append('SKIP-CMP %s:%d: %s' % (path, ln+1, line.strip()[:90]))
                continue
            lines[ln] = wrap_line(line, report, path, ln+1)
    io.open(path, 'w', encoding='utf-8', newline='').write('\n'.join(lines))

def process_plain(path, report):
    code = io.open(path, encoding='utf-8', errors='replace').read()
    lines = code.split('\n')
    # skip kGroupOrder[] statement (compared against raw Group() strings)
    skip = set()
    in_kgo = False
    for i, line in enumerate(lines):
        if 'kGroupOrder[]' in line:
            in_kgo = True
        if in_kgo:
            skip.add(i)
            if '};' in line:
                in_kgo = False
    for i, line in enumerate(lines):
        if not CJK.search(line) or i in skip:
            continue
        lines[i] = wrap_line(line, report, path, i+1)
    io.open(path, 'w', encoding='utf-8', newline='').write('\n'.join(lines))

def main():
    report = []
    moddir = os.path.join(ROOT, 'modules')
    for fn in sorted(os.listdir(moddir)):
        if fn.endswith('.cpp'):
            process_module(os.path.join(moddir, fn), report)
    for rel in ['cli.cpp', 'cleanresult.cpp', 'ui/dialog.cpp', 'ui/controls.cpp',
                'ui/appwindow.cpp', 'prove.cpp']:
        process_plain(os.path.join(ROOT, rel), report)
    io.open('tools/i18n_wrap_report.txt', 'w', encoding='utf-8').write('\n'.join(report))
    print('wrapped/skip lines:', len(report))

main()
