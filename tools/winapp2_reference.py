"""Winapp2.ini 的独立 Python 参考实现

两个用途：
  1. 立刻验证内置规则集里哪些规则在本机真正命中、能省多少空间
  2. 之后用作 C++ 引擎的比对基准（与 .reg 解析器相同的验证思路）
"""

import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INI = os.path.join(ROOT, "data", "Winapp2.ini")

# Winapp2 使用的变量 -> 本机路径
KNOWN = {
    "%localappdata%": os.environ.get("LOCALAPPDATA", ""),
    "%appdata%": os.environ.get("APPDATA", ""),
    "%programdata%": os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
    "%commonappdata%": os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
    "%userprofile%": os.environ.get("USERPROFILE", ""),
    "%systemroot%": os.environ.get("WINDIR", r"C:\Windows"),
    "%windir%": os.environ.get("WINDIR", r"C:\Windows"),
    "%systemdrive%": os.environ.get("SystemDrive", "C:"),
    "%documents%": os.path.join(os.environ.get("USERPROFILE", ""), "Documents"),
    "%programfiles%": os.environ.get("ProgramFiles", r"C:\Program Files"),
    "%temp%": os.environ.get("TEMP", ""),
    "%public%": os.environ.get("PUBLIC", r"C:\Users\Public"),
}


def expand(path):
    p = path
    for k, v in KNOWN.items():
        if k in p.lower():
            # 大小写不敏感替换
            idx = p.lower().find(k)
            while idx >= 0:
                p = p[:idx] + v + p[idx + len(k):]
                idx = p.lower().find(k, idx + len(v))
    return p


def dir_size(path):
    total = 0
    try:
        for e in os.scandir(path):
            try:
                if e.is_file(follow_symlinks=False):
                    total += e.stat().st_size
                elif e.is_dir(follow_symlinks=False):
                    total += dir_size(e.path)
            except OSError:
                continue
    except OSError:
        pass
    return total


def match_pattern(pattern):
    """展开含 * 的目录通配，返回匹配到的目录列表"""
    parts = pattern.replace("/", "\\").split("\\")
    current = [parts[0] + "\\" if len(parts[0]) == 2 and parts[0][1] == ":" else parts[0]]
    for seg in parts[1:]:
        nxt = []
        for base in current:
            if "*" in seg:
                try:
                    for e in os.scandir(base):
                        if e.is_dir(follow_symlinks=False) and glob.fnmatch.fnmatch(e.name, seg):
                            nxt.append(e.path)
                except OSError:
                    pass
            else:
                p = os.path.join(base, seg)
                if os.path.isdir(p):
                    nxt.append(p)
        current = nxt
        if not current:
            break
    return current


def parse_ini(path):
    sections = []
    cur = None
    for raw in open(path, encoding="utf-8-sig", errors="replace").read().splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = {"name": line[1:-1], "keys": [], "default": True,
                   "warning": False, "detect": None, "detectfile": None}
            sections.append(cur)
            continue
        if cur is None or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k, v = k.strip(), v.strip()
        kl = k.lower()
        if kl.startswith("filekey"):
            cur["keys"].append(v)
        elif kl == "default":
            cur["default"] = v.lower() != "false"
        elif kl.startswith("warning"):
            cur["warning"] = True
        elif kl.startswith("detectfile"):
            cur["detectfile"] = v
        elif kl.startswith("detect"):
            cur["detect"] = v
    return sections


def main():
    sections = parse_ini(INI)
    print(f"解析: {len(sections)} 个节")
    total_all = 0
    rows = []
    for s in sections:
        if s["warning"]:
            continue
        # DetectFile 检查
        if s["detectfile"]:
            p = expand(s["detectfile"].split("|")[0])
            if not os.path.exists(p):
                continue
        size = 0
        hits = []
        for key in s["keys"]:
            parts = key.split("|")
            pathpat = expand(parts[0])
            flags = "|".join(parts[2:]).upper()
            dirs = match_pattern(pathpat)
            if not dirs:
                continue
            fname = parts[1] if len(parts) > 1 and parts[1] else "*.*"
            for d in dirs:
                if "RECURSE" in flags:
                    sz = dir_size(d)
                else:
                    sz = 0
                    try:
                        for e in os.scandir(d):
                            if e.is_file(follow_symlinks=False) and glob.fnmatch.fnmatch(e.name, fname):
                                sz += e.stat().st_size
                    except OSError:
                        pass
                if sz:
                    size += sz
                    hits.append((d, sz))
        if size:
            rows.append((s["name"], size, hits))
            total_all += size

    rows.sort(key=lambda r: -r[1])
    print()
    print(f"{'节名':<26}{'大小':>12}   命中路径")
    print("-" * 100)
    for name, size, hits in rows:
        print(f"{name:<26}{size/1024/1024:>10.2f} MB   {hits[0][0][:60]}")
    print()
    print(f"命中节数: {len(rows)}/{len(sections)}")
    print(f"合计可清理: {total_all/1024/1024:.2f} MB ({total_all/1024/1024/1024:.2f} GB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
