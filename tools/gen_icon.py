#!/usr/bin/env python3
# ============================================================================
#  gen_icon.py — 生成 NetAdmin Toolbox 应用图标（纯标准库，无第三方依赖）
#  ---------------------------------------------------------------------------
#  设计：深空圆角方块 + DNA 双螺旋（主色绿 → 青渐变）+ 柔光
#  输出：src/app.ico（16/32/48/64/128/256，32bpp BGRA BMP-in-ICO，windres 兼容）
#  用法：python gen_icon.py [输出路径]
# ============================================================================
import math
import struct
import sys
import os

SS = 4          # 超采样倍数（1024 = 256*4）
BASE = 256


def lerp(a, b, t):
    return a + (b - a) * t


def lerp_c(c1, c2, t):
    return tuple(lerp(a, b, t) for a, b in zip(c1, c2))


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


# ---------------------------------------------------------------- 绘制（1024²）
def render_big():
    """返回 (BASE*SS)² 的 RGBA 像素列表（每像素 4 个 0-255 分量）"""
    N = BASE * SS
    px_size = 1.0 / N                      # 归一化像素尺寸（抗锯齿宽度）
    half_aa = px_size * 0.5

    # ---- 设计参数（归一化坐标，y 向下）----
    R = 0.235                                # 圆角半径（相对边长）
    cx = 0.5
    cy = 0.52
    amp = 0.215                              # 螺旋半幅
    turns = 1.15                             # 半周期数
    x0, x1 = 0.235, 0.765                    # 螺旋水平范围
    strand_w = 0.0165                        # 线粗（半宽）
    rung_every = 0.118                       # 横档间隔
    rung_w = 0.0095

    bg_top = (0x13, 0x1D, 0x28)
    bg_bot = (0x08, 0x0C, 0x12)
    green = (0x00, 0xE5, 0x99)
    cyan = (0x22, 0xD3, 0xEE)
    glow_green = (0x00, 0x7A, 0x55)
    glow_cyan = (0x10, 0x6E, 0x7E)

    # 圆角矩形 SDF：内部 >=0
    def sdf_round(x, y):
        dx = abs(x - cx) - (0.5 - R)
        dy = abs(y - cy) - (0.5 - R)
        if dx <= 0 and dy <= 0:
            return max(dx, dy)                # 内部
        return math.hypot(max(dx, 0), max(dy, 0))

    # 螺旋曲线在给定 x 的 y（两条链）
    def helix_y(x, phase_off):
        t = (x - x0) / (x1 - x0)
        return cy + amp * math.sin(2.0 * math.pi * turns * t + phase_off)

    # 横档位置
    rungs = []
    xr = x0 + rung_every * 0.5
    while xr < x1 - rung_every * 0.3:
        rungs.append(xr)
        xr += rung_every

    out = bytearray(N * N * 4)

    for py_i in range(N):
        py = (py_i + 0.5) / N
        row = py_i * N * 4
        for px_i in range(N):
            px = (px_i + 0.5) / N

            # --- 背景：圆角方块 + 对角渐变 + 内侧细边 ---
            s = sdf_round(px, py)
            if s > R:                          # 圆角外（四角外）直接透明
                continue
            t = clamp01((px * 0.6 + py * 0.4))
            bg = lerp_c(bg_top, bg_bot, t)

            # 内侧 1px 亮边（靠近边缘时提亮）
            edge = clamp01(1.0 - abs(s - (R * 0.06)) / (px_size * 1.2))
            if s < R * 0.10:
                bg = lerp_c(bg, (0x2A, 0x3A, 0x48), 0.35 * (1.0 - s / (R * 0.10)))

            # 覆盖度（背景整体作为底）
            cov = 1.0

            # --- 螺旋柔光（宽而淡）---
            yA = helix_y(px, 0.0)
            yB = helix_y(px, math.pi)
            dA = abs(py - yA)
            dB = abs(py - yB)
            gw = strand_w * 4.2
            gA = clamp01((gw - dA) / (px_size * 2.0)) * 0.20
            gB = clamp01((gw - dB) / (px_size * 2.0)) * 0.16
            bg = lerp_c(bg, glow_green, gA)
            bg = lerp_c(bg, glow_cyan, gB)

            # --- 横档 ---
            for xr_ in rungs:
                if abs(px - xr_) > rung_w + px_size:
                    continue
                ya = helix_y(xr_, 0.0)
                yb = helix_y(xr_, math.pi)
                sep = abs(ya - yb)
                mid = (ya + yb) * 0.5
                halflen = sep * 0.42
                if abs(py - mid) > halflen:
                    continue
                covx = clamp01((rung_w - abs(px - xr_)) / px_size)
                covy = clamp01((halflen - abs(py - mid)) / px_size)
                a = covx * covy * 0.55
                bg = lerp_c(bg, (0xBF, 0xF5, 0xE4), a)

            # --- 两条链 ---
            aA = clamp01((strand_w - dA) / (px_size * 1.2))
            aB = clamp01((strand_w - dB) / (px_size * 1.2))
            # 链 A：绿（左半偏绿、右半偏青，形成渐变感）
            gA_col = lerp_c(green, cyan, clamp01((px - x0) / (x1 - x0)))
            bg = lerp_c(bg, gA_col, aA)
            bg = lerp_c(bg, cyan, aB)

            # 高光：链芯提亮
            hlA = clamp01((strand_w * 0.35 - dA) / (px_size * 1.5))
            hlB = clamp01((strand_w * 0.35 - dB) / (px_size * 1.5))
            bg = lerp_c(bg, (0xE8, 0xFF, 0xF6), hlA * 0.85)
            bg = lerp_c(bg, (0xE8, 0xFF, 0xF6), hlB * 0.6)

            i = row + px_i * 4
            out[i] = int(bg[2] + 0.5)          # B
            out[i + 1] = int(bg[1] + 0.5)      # G
            out[i + 2] = int(bg[0] + 0.5)      # R
            out[i + 3] = int(cov * 255 + 0.5)  # A

    # 四角补透明（圆角外）
    for py_i in range(N):
        py = (py_i + 0.5) / N
        row = py_i * N * 4
        for px_i in range(N):
            px = (px_i + 0.5) / N
            if sdf_round(px, py) > R:
                i = row + px_i * 4
                out[i] = out[i + 1] = out[i + 2] = out[i + 3] = 0
    return out, N


# ---------------------------------------------------------------- 缩放
def box_down(src, src_n, factor):
    """整数倍盒式降采样"""
    if factor == 1:
        return src, src_n
    n = src_n // factor
    out = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            acc = [0, 0, 0, 0]
            for dy in range(factor):
                base = ((y * factor + dy) * src_n + x * factor) * 4
                for dx in range(factor):
                    i = base + dx * 4
                    acc[0] += src[i]
                    acc[1] += src[i + 1]
                    acc[2] += src[i + 2]
                    acc[3] += src[i + 3]
            f2 = factor * factor
            i = (y * n + x) * 4
            out[i] = acc[0] // f2
            out[i + 1] = acc[1] // f2
            out[i + 2] = acc[2] // f2
            out[i + 3] = acc[3] // f2
    return out, n


def bilinear_down(src, src_n, dst_n):
    """双线性缩放（用于非整数倍）"""
    out = bytearray(dst_n * dst_n * 4)
    ratio = src_n / dst_n
    for y in range(dst_n):
        fy = (y + 0.5) * ratio - 0.5
        y0 = int(math.floor(fy)); ty = fy - y0
        y0 = max(0, min(src_n - 1, y0)); y1 = max(0, min(src_n - 1, y0 + 1))
        for x in range(dst_n):
            fx = (x + 0.5) * ratio - 0.5
            x0 = int(math.floor(fx)); tx = fx - x0
            x0 = max(0, min(src_n - 1, x0)); x1 = max(0, min(src_n - 1, x0 + 1))
            i00 = (y0 * src_n + x0) * 4; i01 = (y0 * src_n + x1) * 4
            i10 = (y1 * src_n + x0) * 4; i11 = (y1 * src_n + x1) * 4
            o = (y * dst_n + x) * 4
            for c in range(4):
                v = (src[i00 + c] * (1 - tx) + src[i01 + c] * tx) * (1 - ty) + \
                    (src[i10 + c] * (1 - tx) + src[i11 + c] * tx) * ty
                out[o + c] = int(v + 0.5)
    return out


# ---------------------------------------------------------------- ICO 组装
def bmp_entry(pixels, n):
    """32bpp BGRA，bottom-up，高度加倍（含 AND 掩码行，全零）"""
    hdr = struct.pack(
        "<IiiHHIIiiII",
        40,          # biSize
        n,           # biWidth
        n * 2,       # biHeight（XOR+AND）
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression
        n * n * 4,   # biSizeImage
        0, 0, 0, 0)
    body = bytearray()
    for y in range(n - 1, -1, -1):            # bottom-up
        body += pixels[y * n * 4:(y + 1) * n * 4]
    and_row_bytes = ((n + 31) // 32) * 4      # 1bpp 掩码行（4 字节对齐）
    body += bytes(and_row_bytes * n)
    return hdr + bytes(body)


def write_ico(path, images):
    """images: [(size, pixels)]"""
    count = len(images)
    offset = 6 + 16 * count
    dir_ = struct.pack("<HHH", 0, 1, count)
    entries = b""
    blobs = b""
    for size, px in images:
        data = bmp_entry(px, size)
        w = size if size < 256 else 0
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(dir_ + entries + blobs)
    return os.path.getsize(path)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "app.ico")
    out_path = os.path.abspath(out_path)

    print("[1/3] rendering 1024x1024 (supersampled) ...")
    big, n = render_big()

    print("[2/3] downsampling ...")
    base, _ = box_down(big, n, SS)             # 1024 -> 256
    sizes = [16, 32, 48, 64, 128, 256]
    images = []
    cache = {256: base}
    for s in sizes:
        if s == 256:
            images.append((s, base))
            continue
        # 找最近的更大源（256 或 128）
        src_n = 256 if s > 64 else 128
        src = base if src_n == 256 else box_down(base, 256, 2)[0]
        images.append((s, bilinear_down(src, src_n, s)))

    print("[3/3] writing ICO ...")
    total = write_ico(out_path, images)
    print("OK -> %s (%d bytes, sizes: %s)" % (out_path, total,
                                              ",".join(str(s) for s in sizes)))


if __name__ == "__main__":
    main()
