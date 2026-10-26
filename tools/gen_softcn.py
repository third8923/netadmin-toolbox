#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_softcn.py — 将 SoftCnKiller 数据仓库特征文件（UTF-16 LE）转为内嵌头文件。

来源：gitee.com/softcnkiller/data（四海一叶秋）sign.txt / folder.txt / whitepath.txt
格式：UTF-16 LE + BOM，CRLF 行分隔，一行一条特征。
产出：data/softcn_rules.h —— mz::purg::kSoftCnSign / kSoftCnFolder / kSoftCnWhitePath
      wchar_t 字符串字面量（\n 分隔），供 ruleset.cpp 编译期嵌入。

用法：python tools/gen_softcn.py   （在仓库根目录执行）
致谢：特征数据来自 SoftCnKiller 开源数据仓库，仅格式适配、内容未改动。
"""
import base64
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "softcn")
OUT = os.path.join(ROOT, "data", "softcn_rules.h")

CHUNK = 800  # 每个字符串字面量片段的行数（避免超长字面量）


def load_lines(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] == b"\xff\xfe":
        text = raw[2:].decode("utf-16-le")
    else:
        text = raw.decode("utf-8-sig", errors="replace")
    lines = []
    for line in text.replace("\r\n", "\n").split("\n"):
        line = line.strip()
        if line:
            lines.append(line)
    return lines


def esc(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def chunks(lines):
    for i in range(0, len(lines), CHUNK):
        yield lines[i:i + CHUNK]


def emit_block(fh, varname, lines, comment):
    """以 base64(UTF-16LE) 内嵌：明文特征串会被杀软内容扫描误报
    （实测 Defender PUA:Win32/Softcnapp），编码存储运行时解码。"""
    fh.write("// ---- %s（%d 条，base64 of UTF-16LE，\\n 分隔）----\n" % (comment, len(lines)))
    joined = "".join(l + "\n" for l in lines)
    b64 = base64.b64encode(joined.encode("utf-16-le")).decode("ascii")
    # 每 96 字符一段，C 字符串相邻拼接
    fh.write("inline const wchar_t* const %s[] = {\n" % varname)
    for i in range(0, len(b64), 96):
        fh.write('    L"%s"\n' % b64[i:i + 96])
    fh.write("};\n")
    fh.write("inline const int %s_count = 1;\n\n" % varname)


def main():
    sign = load_lines(os.path.join(SRC, "sign.txt"))
    folder = load_lines(os.path.join(SRC, "folder.txt"))
    white = load_lines(os.path.join(SRC, "whitepath.txt"))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("// ============================================================================\n")
        fh.write("//  softcn_rules.h — SoftCnKiller 特征库（编译期内嵌，自动生成，勿手改）\n")
        fh.write("//  生成器：tools/gen_softcn.py    数据：data/softcn/*.txt（UTF-16 LE 源文件）\n")
        fh.write("//  数据来源：SoftCnKiller 数据仓库（四海一叶秋）\n")
        fh.write("//            https://gitee.com/softcnkiller/data\n")
        fh.write("//            https://github.com/SiHaiYiYeQiu/SoftCnKiller\n")
        fh.write("//  本项目仅做格式适配（逐行原样保留），特征内容版权与更新归原作者。\n")
        fh.write("//  行分隔：\\n；空行已剔除；运行期由 ruleset.cpp 解析为匹配索引。\n")
        fh.write("// ============================================================================\n")
        fh.write("#pragma once\n\nnamespace mz {\nnamespace purg {\n\n")
        emit_block(fh, "kSoftCnSign", sign, "签名主体黑名单 sign.txt")
        emit_block(fh, "kSoftCnFolder", folder, "目录特征黑名单 folder.txt")
        emit_block(fh, "kSoftCnWhitePath", white, "路径白名单 whitepath.txt")
        fh.write("} // namespace purg\n} // namespace mz\n")

    print("OK %s  sign=%d folder=%d white=%d" % (OUT, len(sign), len(folder), len(white)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
