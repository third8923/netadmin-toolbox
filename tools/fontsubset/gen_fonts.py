#!/usr/bin/env python3
# ============================================================================
#  gen_fonts.py — VI-D2 字体子集生成（离线一次，产物入库）
#  ---------------------------------------------------------------------------
#  用 fonttools/pyftsubset 从上游字体裁出 Display / Data 两层所需的
#  最小字符集，产物为可直接嵌入 RCDATA 的 .bin（TTF 内容）：
#    * Baloo 2 ExtraBold（可变字体实例化到 wght=800）
#        字符集：U+0020-007E（覆盖 "biohacker.fun" / "NetAdmin Toolbox" /
#        版本号等拉丁与数字）。产物约 12-18 KB。
#    * JetBrains Mono Regular / SemiBold
#        字符集：U+0020-007E + U+00B7（中点，字标金点前的分隔符）。
#        各约 25-35 KB。
#  中文绝不进子集：Display 层只画拉丁字标与数字；正文中文走雅黑；
#  等宽行里的中文由 GDI 字体链接自动回落雅黑。
#
#  OFL 纪律：pyftsubset 默认保留 name 表（版权/许可字段），勿加
#  --no-name-tables；许可全文随仓保存在 src/res/fonts/OFL.txt。
#
#  用法（受管 venv）：
#    C:/Users/bhfadmin/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
#        tools/fontsubset/gen_fonts.py <上游字体目录> [输出目录]
#  上游字体目录需含：Baloo2[wght].ttf、JetBrainsMono-Regular.ttf、
#  JetBrainsMono-SemiBold.ttf（Google Fonts / JetBrains 官方仓库发布版）。
# ============================================================================
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

LATIN = "U+0020-007E"
MONO_EXTRA = ",U+00B7"


def subset_one(src_ttf: str, unicodes: str, out_path: str,
               flavor: str | None = None, wght: int | None = None) -> int:
    """pyftsubset 核心：可变字体先实例化到指定字重再裁子集。返回产物字节数。"""
    if wght is not None:
        font = TTFont(src_ttf)
        if "fvar" in font:
            instantiateVariableFont(font, {"wght": wght}, inplace=True)
            # 固定字重后清理 fvar 残留（instancer inplace 已处理），补写名表
            name = font["name"]
            sub = "ExtraBold" if wght >= 800 else "Regular"
            name.setName("Baloo 2", 1, 3, 1, 0x409)
            name.setName(sub, 2, 3, 1, 0x409)
            name.setName(f"Baloo 2 {sub}", 4, 3, 1, 0x409)
            name.setName(f"Baloo2-{sub}", 6, 3, 1, 0x409)
        tmp = src_ttf + ".inst.ttf"
        font.save(tmp)
        work = tmp
    else:
        work = src_ttf

    args = [
        work,
        f"--unicodes={unicodes}",
        "--layout-features=*",     # 保留 kern 等（字标负字距语义依赖 kern）
        "--name-IDs=*",            # OFL：保留版权与许可字段
        "--glyph-names",
        "--recalc-bounds",
        "--drop-tables+=DSIG",
        f"--output-file={out_path}",
    ]
    subset.main(args)

    if wght is not None and os.path.exists(work):
        os.remove(work)
    return os.path.getsize(out_path)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: gen_fonts.py <upstream-font-dir> [out-dir]")
        return 2
    src_dir = os.path.abspath(sys.argv[1])
    proj = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", ".."))
    out_dir = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else \
        os.path.join(proj, "src", "res", "fonts")
    os.makedirs(out_dir, exist_ok=True)

    jobs = [
        # (上游文件, 输出名, 字符集, 实例化字重 or None)
        ("Baloo2[wght].ttf", "baloo2-800-subset.bin", LATIN, 800),
        ("JetBrainsMono-Regular.ttf", "jbmono-regular-subset.bin",
         LATIN + MONO_EXTRA, None),
        ("JetBrainsMono-SemiBold.ttf", "jbmono-semibold-subset.bin",
         LATIN + MONO_EXTRA, None),
    ]

    total = 0
    for i, (src, out, uni, wght) in enumerate(jobs, 1):
        src_path = os.path.join(src_dir, src)
        out_path = os.path.join(out_dir, out)
        if not os.path.exists(src_path):
            print(f"[ERROR] missing upstream font: {src_path}")
            return 1
        size = subset_one(src_path, uni, out_path, wght=wght)
        total += size
        print(f"[{i}/{len(jobs)}] {out}: {size:,} bytes")

    # OFL 全文（两字体各自 license 合并入库）
    ofl_out = os.path.join(out_dir, "OFL.txt")
    parts = []
    for lic in ("OFL-baloo.txt", "OFL-jbmono.txt"):
        p = os.path.join(src_dir, lic)
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                parts.append(f.read().strip())
    if parts:
        sep = ("\n" + "=" * 78 + "\n\n")
        with open(ofl_out, "w", encoding="utf-8", newline="\n") as f:
            f.write(sep.join(parts) + "\n")
        print(f"OFL.txt: {os.path.getsize(ofl_out):,} bytes")

    print(f"TOTAL: {total:,} bytes (budget <= 300 KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
