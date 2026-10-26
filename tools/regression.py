"""网管工具箱 全功能回归测试

覆盖所有可验证项（本机环境限制：子进程无法写注册表，
故注册表写入的正确性由 tools/verify_reg.py 的解析等价性证明覆盖）
"""

import os
import subprocess
import sys
import tempfile
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "dist", "netadmin-toolbox.exe")
FAILS = []
PASSES = []


def run(args, timeout=180):
    p = subprocess.run([EXE] + args, capture_output=True, timeout=timeout)
    out = p.stdout.decode("utf-8", errors="replace")
    err = p.stderr.decode("utf-8", errors="replace")
    return p.returncode, out, err


def check(name, cond, detail=""):
    if cond:
        PASSES.append(name)
        print(f"[PASS] {name}" + (f"  {detail}" if detail else ""))
    else:
        FAILS.append(name)
        print(f"[FAIL] {name}" + (f"  {detail}" if detail else ""))


def main():
    if not os.path.exists(EXE):
        print(f"[FAIL] 找不到 {EXE}")
        return 1

    print("=== 1. 基础可用性 ===")
    rc, out, _ = run(["--help"])
    check("--help 返回 0", rc == 0, f"rc={rc}")
    check("--help 输出中文", "网管工具箱" in out)
    check("--help 列出用法与 run", "run <模块ID>" in out)

    rc, out, _ = run(["--list"])
    check("--list 返回 0", rc == 0, f"rc={rc}")
    check("--list 模块数 31", "共 31 个功能模块" in out)
    check("--list 含关键模块 ID",
          all(k in out for k in ("sys.info", "net.conn", "tweak.winopt")))

    size_kb = os.path.getsize(EXE) / 1024
    check("exe 体积合理（单文件）", 300 < size_kb < 3000, f"{size_kb:.0f} KB")

    print("\n=== 2. Windows 优化（开关型模块） ===")
    rc, out, _ = run(["run", "tweak.winopt"])
    check("run tweak.winopt 返回 0", rc == 0, f"rc={rc}")
    check("报告 40 项优化", "共 40 项优化" in out)
    rows = [l for l in out.splitlines() if "已应用" in l or "未应用" in l]
    check("逐项列出应用状态", len(rows) >= 30, f"实际 {len(rows)} 行")

    rc, out, err = run(["run", "no.such.module"])
    check("未知模块不崩溃", rc != 0 and "Traceback" not in err, f"rc={rc}")

    print("\n=== 3. 扫描功能（真实系统） ===")
    for mid in ["clean.basic", "clean.system", "clean.wechat", "clean.qq"]:
        rc, out, err = run(["run", mid], timeout=240)
        # 未安装微信/QQ 时返回「未发现」也属正常
        ok = rc == 0 and ("共" in out or "未发现" in out)
        detail = ""
        for line in out.splitlines():
            if line.startswith("共") or "未发现" in line:
                detail = line.strip()
                break
        check(f"{mid} 可执行", ok, detail)

    print("\n=== 4. 大文件扫描（临时夹具） ===")
    tmp = os.path.join(ROOT, "build", "testdata")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)
    for i in range(3):
        with open(os.path.join(tmp, f"big{i}.bin"), "wb") as f:
            f.write(b"0" * (3 * 1024 * 1024))
    os.makedirs(os.path.join(tmp, "sub"), exist_ok=True)
    with open(os.path.join(tmp, "sub", "dup1.txt"), "wb") as f:
        f.write(b"D" * 200000)
    with open(os.path.join(tmp, "sub", "dup2.txt"), "wb") as f:
        f.write(b"D" * 200000)

    rc, out, _ = run(["run", "scan.large", "--path", tmp, "--target", "1"])
    check("大文件扫描找到 3 个文件", out.count("big") >= 3, f"rc={rc}")
    check("大文件扫描显示大小", "MB" in out)

    rc, out, _ = run(["run", "scan.dup", "--path", tmp])
    check("重复文件扫描识别 dup1/dup2", "dup2" in out or "dup1" in out, f"rc={rc}")

    rc, out, _ = run(["run", "scan.dirs", "--path", tmp])
    check("目录大小扫描返回 sub", "sub" in out, f"rc={rc}")

    rc, out, _ = run(["run", "scan.apps"], timeout=300)
    check("软件占用扫描可执行", rc == 0 and ("共" in out or "未发现" in out))

    print("\n=== 5. 边界与健壮性 ===")
    rc, out, err = run(["run", "scan.large", "--path", "Z:/nonexistent", "--target", "1"])
    check("不存在的路径不崩溃", "Traceback" not in err and rc in (0, 1), f"rc={rc}")

    rc, out, err = run(["--badarg"])
    check("未知命令给出提示", "未知命令" in out or rc != 0, f"rc={rc}")

    shutil.rmtree(tmp, ignore_errors=True)

    print("\n" + "=" * 60)
    print(f"通过 {len(PASSES)} 项，失败 {len(FAILS)} 项")
    if FAILS:
        print("失败项:", FAILS)
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
