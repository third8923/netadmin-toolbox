# -*- coding: utf-8 -*-
# 独立验收脚本（非工程师工具链）：从 exe 的 CLI 文本输出重建 QR 矩阵 -> PNG -> zxing 解码
# 用法: python t216_qr_indep.py <cli_output.txt> <expected_payload>
import sys
import numpy as np
import cv2

txt_path, expected = sys.argv[1], sys.argv[2]
rows_half = []  # 每行文本 -> (top_bits, bottom_bits) 两个模块行
for line in open(txt_path, encoding="utf-8", errors="replace"):
    line = line.rstrip("\r\n")
    if line and set(line) - set(" ▀▄█"):
        continue  # 只保留纯「空格+块字符」行（含静区全空行，保奇偶对齐）
    if not line.strip() and not rows_half and not line:
        continue
    top = [1 if c == "▀" or c == "█" else 0 for c in line]
    bot = [1 if c == "▄" or c == "█" else 0 for c in line]
    rows_half.append((top, bot))

if not rows_half:
    print("NO_QR_TEXT"); sys.exit(1)

width = max(len(t) for t, _ in rows_half)
grid = []
for top, bot in rows_half:
    t = top + [0] * (width - len(top))
    b = bot + [0] * (width - len(bot))
    grid.append(t); grid.append(b)

arr = np.array(grid, dtype=np.uint8) * 255
# 白边再加 8 模块静区
arr = np.pad(arr, 8, constant_values=255)
img = cv2.resize(arr, (arr.shape[1] * 8, arr.shape[0] * 8), interpolation=cv2.INTER_NEAREST)
png = r"C:\Users\bhfadmin\AppData\Local\Temp\t216_qr_indep.png"
cv2.imwrite(png, img)
print("matrix %dx%d -> %s" % (arr.shape[1], arr.shape[0], png))

import zxingcpp
res = zxingcpp.read_barcodes(img)
if not res:
    print("DECODE_FAIL"); sys.exit(2)
got = res[0].text
print("decoded :", got)
print("expected:", expected)
print("MATCH" if got == expected else "MISMATCH")
sys.exit(0 if got == expected else 3)
