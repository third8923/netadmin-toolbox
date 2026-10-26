"""把 data/Winapp2.ini 生成为内嵌 C++ 头文件

这样 exe 无需外部规则文件也能扫出第三方应用缓存。
外部 Winapp2.ini 放在 exe 同目录时会优先加载（覆盖内置）。
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "Winapp2.ini")
DST = os.path.join(ROOT, "data", "winapp2_builtin.h")


def main():
    if not os.path.exists(SRC):
        print(f"[FAIL] 找不到 {SRC}")
        return 1
    raw = open(SRC, "rb").read()
    # 统一换行为 CRLF，Winapp2 解析器按行处理
    text = raw.decode("utf-8").replace("\r\n", "\n")
    data = text.encode("utf-8")

    sections = text.count("\n[") + (1 if text.startswith("[") else 0)
    filekeys = sum(1 for line in text.splitlines() if line.lower().startswith("filekey"))

    with open(DST, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// 自动生成，请勿手改 —— 由 tools/gen_winapp2.py 从 data/Winapp2.ini 生成\n")
        fh.write("#pragma once\n#include <cstddef>\n\n")
        fh.write("static const unsigned char WINAPP2_BUILTIN[] = {")
        for i, b in enumerate(data):
            if i % 24 == 0:
                fh.write("\n    ")
            fh.write(f"0x{b:02x},")
        fh.write("\n    0x00\n};\n")
        fh.write(f"static const size_t WINAPP2_BUILTIN_SIZE = {len(data)};\n")

    print(f"内置规则集: {sections} 个节, {filekeys} 条 FileKey, {len(data)/1024:.1f} KB")
    print(f"输出: {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
