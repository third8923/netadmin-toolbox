"""比对 C++ 解析器与 Python 参考实现的结果（正确性证明）

C++ 引擎输出 vs Python 独立参考实现，逐条比对全部 80 个 .reg 文件。
"""

import json
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "prove.exe")
REF = os.path.join(ROOT, "build", "reg_reference.json")


def parse_cpp_output(text):
    """解析 prove.exe 输出 -> {key: [(kind, key, name, value)]}"""
    out = {}
    cur = None
    for line in text.splitlines():
        if line.startswith("FILE "):
            parts = line.split()
            cur = parts[1]
            out[cur] = []
        elif line.startswith("OP ") and cur:
            rest = line[3:]
            parts = rest.split('\t')
            if len(parts) == 4:
                out[cur].append((parts[0], parts[1], parts[2], parts[3]))
    return out


def ref_ops_to_tuples(ops):
    res = []
    for op in ops:
        kind, key, name, value = op
        if kind in ("DWORD", "QWORD"):
            res.append((kind, key, name, str(value)))
        elif kind == "HEX":
            typ, hexpart = value
            res.append((kind, key, name, f"{typ}:{hexpart}"))
        elif kind == "DELVAL":
            res.append((kind, key, name, ""))
        elif kind == "DELKEY":
            res.append((kind, key, "", ""))
        else:  # SZ
            res.append((kind, key, name, value if isinstance(value, str) else str(value)))
    return res


def main():
    if not os.path.exists(EXE):
        print(f"[FAIL] 找不到 {EXE}，请先编译")
        return 1
    proc = subprocess.run([EXE], capture_output=True)
    text = proc.stdout.decode("utf-8", errors="replace")
    cpp = parse_cpp_output(text)

    ref_raw = json.load(open(REF, encoding="utf-8"))
    ref = {k: ref_ops_to_tuples(v["ops"]) for k, v in ref_raw.items()}

    print(f"C++ 解析文件: {len(cpp)}")
    print(f"参考实现文件: {len(ref)}")
    print()

    fail = 0
    ok = 0
    missing = []
    for key in ref:
        if key not in cpp:
            missing.append(key)
            continue
        a = cpp[key]
        b = ref[key]
        if len(a) != len(b):
            print(f"[FAIL] {key}: 条数不一致 C++={len(a)} 参考={len(b)}")
            fail += 1
            continue
        diffs = 0
        for i, (x, y) in enumerate(zip(a, b)):
            # 规范化：SZ 值可能因转义差异微小不同，但应完全一致
            if x != y:
                if diffs < 3:
                    print(f"[DIFF] {key} #{i}")
                    print(f"   C++: {x}")
                    print(f"   参考: {y}")
                diffs += 1
        if diffs:
            print(f"[FAIL] {key}: {diffs}/{len(a)} 条不一致")
            fail += 1
        else:
            ok += 1

    if missing:
        print(f"[FAIL] C++ 缺少文件: {missing}")

    total_ops_cpp = sum(len(v) for v in cpp.values())
    total_ops_ref = sum(len(v) for v in ref.values())
    print()
    print("=" * 60)
    print(f"完全一致: {ok}/{len(ref)} 个文件")
    print(f"操作条数: C++ {total_ops_cpp} / 参考 {total_ops_ref}")
    if fail == 0 and not missing and total_ops_cpp == total_ops_ref:
        print("结论: .reg 解析器与独立参考实现完全等价 ✓")
        return 0
    print(f"结论: 存在 {fail} 处差异 ✗")
    return 1


if __name__ == "__main__":
    sys.exit(main())
