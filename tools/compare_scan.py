"""优化效果对比：C++ 版 vs 正式版 vs 优化后

用法:
  python tools/compare_scan.py baseline    # 记录优化前基线
  python tools/compare_scan.py after       # 记录优化后结果并对比
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "dist", "netadmin-toolbox.exe")
BASE_JSON = os.path.join(ROOT, "build", "scan_baseline.json")


def run_scan(args, timeout=600):
    p = subprocess.run([EXE] + args, capture_output=True, timeout=timeout)
    return p.stdout.decode("utf-8", errors="replace")


def parse_items(out):
    """解析 CLI 输出：返回 (items, summary)

    新格式（可清理/仅统计分离）::

        共 17 项
          可清理:      15 项     4.19 GB
          仅统计:       2 项     6.04 GB   (系统关键项，需用系统工具处理)
    """
    items = []
    summary = {"total": 0, "cleanable_n": 0, "cleanable_mb": 0.0,
               "report_n": 0, "report_mb": 0.0}
    in_report_section = False
    for line in out.splitlines():
        m = re.search(r"^共\s+(\d+)\s+项", line)
        if m:
            summary["total"] = int(m.group(1))
            continue
        m = re.search(r"可清理:\s*(\d+)\s*项\s+([\d\.]+)\s*(B|KB|MB|GB)", line)
        if m:
            summary["cleanable_n"] = int(m.group(1))
            summary["cleanable_mb"] = round(to_mb(float(m.group(2)), m.group(3)), 2)
            continue
        m = re.search(r"仅统计:\s*(\d+)\s*项\s+([\d\.]+)\s*(B|KB|MB|GB)", line)
        if m:
            summary["report_n"] = int(m.group(1))
            summary["report_mb"] = round(to_mb(float(m.group(2)), m.group(3)), 2)
            continue
        if "仅统计（不会删除）" in line:
            in_report_section = True
            continue
        m = re.match(r"^\s{2}(\S+)\s+(.+?)\s{2,}([\d\.]+)\s*(B|KB|MB|GB)\+?\s*$", line)
        if m:
            items.append({
                "category": m.group(1),
                "name": m.group(2).strip(),
                "size": float(m.group(3)),
                "unit": m.group(4),
                "report": in_report_section,
            })
    return items, summary


def to_mb(v, unit):
    return {"B": v / 1024 / 1024, "KB": v / 1024, "MB": v, "GB": v * 1024}[unit]


def collect():
    data = {}
    for kind in ["basic", "system", "wechat", "qq"]:
        out = run_scan(["clean", kind])
        items, summary = parse_items(out)
        data[kind] = {
            "count": summary["total"] or len(items),
            "total_mb": summary["cleanable_mb"],
            "cleanable_n": summary["cleanable_n"],
            "report_n": summary["report_n"],
            "report_mb": summary["report_mb"],
            "items": [i["name"] for i in items],
        }
    return data


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "after"
    os.makedirs(os.path.dirname(BASE_JSON), exist_ok=True)
    cur = collect()

    if mode == "baseline":
        json.dump(cur, open(BASE_JSON, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print("已记录基线：")
        for k, v in cur.items():
            print(f"  {k:<8} {v['count']:>3} 项  {v['total_mb']:>9.2f} MB")
        return 0

    if not os.path.exists(BASE_JSON):
        print("没有基线文件，先运行 compare_scan.py baseline")
        json.dump(cur, open(BASE_JSON, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        return 0

    base = json.load(open(BASE_JSON, encoding="utf-8"))
    print(f"{'类别':<8}{'优化前':>18}{'优化后(可清理)':>22}{'仅统计':>18}{'项数提升':>12}")
    print("-" * 82)
    for k in cur:
        b, a = base.get(k, {}), cur[k]
        bc = b.get("count", 0)
        bm = b.get("total_mb", 0)
        ac, am = a["count"], a["total_mb"]
        rep = a.get("report_n", 0)
        repmb = a.get("report_mb", 0)
        print(f"{k:<8}{bc:>5}项 {bm:>8.0f}MB {ac:>6}项 {am:>9.0f}MB "
              f"{rep:>4}项 {repmb:>8.0f}MB {ac-bc:>+8d}项")

    print()
    print("新增扫描项（优化后有、优化前无）：")
    for k in cur:
        new = [x for x in cur[k]["items"] if x not in base.get(k, {}).get("items", [])]
        if new:
            print(f"  [{k}] {len(new)} 项:")
            for x in new:
                print(f"      + {x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
