#!/usr/bin/env python3
# ============================================================================
#  qa_gold_ratio.py — 金面积抽查（VI 纪律：金 ≤10% 画面占比）
#  ---------------------------------------------------------------------------
#  统计截图 PNG 中「金信号」像素（围绕 #F2B441 的容差球）占比。
#  用法：python tools/qa_gold_ratio.py <screenshot.png> [<screenshot2.png> ...]
#       --max 10.0   阈值（百分比，默认 10）
#  纯标准库：手写 PNG 行滤波解码（24bit truecolor / 8bit palette）。
# ============================================================================
import os
import struct
import sys
import zlib

# 金 #F2B441 容差（欧氏距离球）
GOLD = (0xF2, 0xB4, 0x41)
TOL2 = 48 * 48 * 3   # 每通道 48 的近似球


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    w = h = bitd = ctype = None
    idat = b""
    plte = None
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bitd, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"PLTE":
            plte = chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if bitd != 8:
        raise ValueError(f"unsupported bit depth {bitd}")
    stride = w * channels
    # 反滤波
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 1:   # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:  # Avg
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + (left + prev[i]) // 2) & 0xFF
        elif ft == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, channels, bytes(out), plte, ctype


def gold_ratio(path):
    w, h, ch, px, plte, ctype = read_png(path)
    total = w * h
    gold = 0
    if ctype == 3:   # palette
        entries = [tuple(plte[i * 3:i * 3 + 3]) for i in range(len(plte) // 3)]
        gold_idx = set()
        for i, c in enumerate(entries):
            if sum((c[k] - GOLD[k]) ** 2 for k in range(3)) <= TOL2:
                gold_idx.add(i)
        for v in px:
            if v in gold_idx:
                gold += 1
    else:
        for i in range(0, len(px), ch):
            r, g, b = px[i], px[i + 1], px[i + 2]
            if (r - GOLD[0]) ** 2 + (g - GOLD[1]) ** 2 + (b - GOLD[2]) ** 2 <= TOL2:
                gold += 1
    return gold / total * 100.0


def main() -> int:
    args = sys.argv[1:]
    max_pct = 10.0
    if "--max" in args:
        i = args.index("--max")
        max_pct = float(args[i + 1])
        del args[i:i + 2]
    if not args:
        print("usage: qa_gold_ratio.py <png...> [--max 10]")
        return 2
    fails = 0
    for p in args:
        pct = gold_ratio(p)
        ok = pct <= max_pct
        print(f"{os.path.basename(p):44} gold {pct:6.3f}%  (max {max_pct}%) "
              f"{'OK' if ok else 'FAIL'}")
        if not ok:
            fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
