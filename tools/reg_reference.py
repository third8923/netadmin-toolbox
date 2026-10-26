"""独立参考实现：解析 .reg 文件，输出规范化 JSON

用途：与 C++ 引擎的输出逐条比对，证明 .reg 解析器正确
（本机环境无法验证注册表写入，故用解析等价性证明正确性）
"""

import json
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def decode(data: bytes) -> str:
    if data[:2] == b'\xff\xfe':
        return data[2:].decode('utf-16-le').rstrip('\x00')
    if data[:3] == b'\xef\xbb\xbf':
        return data[3:].decode('utf-8')
    return data.decode('utf-8', errors='replace')


def parse_reg(text: str):
    """返回 [(key, name, type, value)] 的规范化列表"""
    ops = []
    cur_key = None
    for raw in text.split('\n'):
        line = raw.rstrip('\r')
        s = line.strip()
        if not s or s.startswith(';'):
            continue
        if s.startswith('Windows Registry Editor') or s.startswith('REGEDIT'):
            continue
        if s.startswith('['):
            del_key = s.startswith('[-')
            body = s[2:] if del_key else s[1:]
            body = body[:body.rfind(']')]
            body = body.strip('"')
            if del_key:
                ops.append(('DELKEY', body, '', ''))
                cur_key = None
            else:
                cur_key = body
            continue
        if cur_key is None:
            continue
        eq = s.find('=')
        if eq < 0:
            continue
        name = s[:eq].strip()
        val = s[eq + 1:].strip()
        if name == '@':
            name = ''
        elif name.startswith('"'):
            # 处理转义
            out = []
            i = 1
            while i < len(name):
                c = name[i]
                if c == '\\' and i + 1 < len(name):
                    n = name[i + 1]
                    out.append({'\\': '\\', '"': '"', 'r': '\r', 'n': '\n', '0': '\0'}.get(n, n))
                    i += 2
                elif c == '"':
                    break
                else:
                    out.append(c)
                    i += 1
            name = ''.join(out)

        if val == '-':
            ops.append(('DELVAL', cur_key, name, ''))
        elif val.lower().startswith('dword:'):
            ops.append(('DWORD', cur_key, name, int(val[6:], 16)))
        elif val.lower().startswith('qword:'):
            ops.append(('QWORD', cur_key, name, int(val[6:], 16)))
        elif val.lower().startswith('hex'):
            m = re.match(r'hex(?:\(([0-9a-fA-F]+)\))?:(.*)', val, re.S)
            if m:
                typ = int(m.group(1), 16) if m.group(1) else 3
                hexpart = ''.join(c for c in m.group(2) if c in '0123456789abcdefABCDEF')
                if len(hexpart) % 2:
                    hexpart += '0'
                ops.append(('HEX', cur_key, name, (typ, hexpart.lower())))
        elif val.startswith('"'):
            # 字符串
            out = []
            i = 1
            while i < len(val):
                c = val[i]
                if c == '\\' and i + 1 < len(val):
                    n = val[i + 1]
                    out.append({'\\': '\\', '"': '"', 'r': '\r', 'n': '\n', '0': '\0'}.get(n, n))
                    i += 2
                elif c == '"':
                    break
                else:
                    out.append(c)
                    i += 1
            ops.append(('SZ', cur_key, name, ''.join(out)))
    return ops


def main():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "gen", os.path.join(ROOT, "tools", "gen_data.py"))
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)

    regdir = os.path.join(ROOT, "upstream", "Regfiles")
    idx = {}
    for dp, _dn, fs in os.walk(regdir):
        for fn in fs:
            if fn.lower().endswith('.reg'):
                idx[fn.lower()] = os.path.join(dp, fn)

    feats_json = json.load(open(os.path.join(ROOT, "upstream", "Config", "Features.json"),
                                encoding="utf-8-sig"))
    F = {f["FeatureId"]: f for f in feats_json["Features"]}

    out = {}
    for fid in gen.SELECTED:
        f = F[fid]
        rk, uk = f.get("RegistryKey"), f.get("RegistryUndoKey")
        for tag, name in (("apply", rk), ("undo", uk)):
            if not name:
                continue
            p = idx.get(name.lower()) or idx.get(("undo__" + name).lower())
            if not p:
                continue
            text = decode(open(p, 'rb').read())
            out[f"{fid}.{tag}"] = {
                "file": os.path.basename(p),
                "ops": parse_reg(text),
            }

    dst = os.path.join(ROOT, "build", "reg_reference.json")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=1)

    total_ops = sum(len(v["ops"]) for v in out.values())
    print(f"生成参考解析: {len(out)} 个 reg 文件, {total_ops} 条操作")
    print(f"输出: {dst}")

    # 统计操作类型分布
    from collections import Counter
    c = Counter(op[0] for v in out.values() for op in v["ops"])
    print("操作类型分布:", dict(c))


if __name__ == "__main__":
    sys.exit(main())
