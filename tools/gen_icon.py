#!/usr/bin/env python3
# ============================================================================
#  gen_icon.py — 生成 NetAdmin Toolbox 应用图标（VI-D2 蝶形，纯标准库）
#  ---------------------------------------------------------------------------
#  v2.17「蜕变之翼」：与 src/ui/brand.cpp 同源的蝶形坐标表 → 16/24/32/48/256
#  五尺寸 ico（32bpp BGRA BMP-in-ICO，windres 兼容）。
#
#  图标构图：纸底圆角方块（VI 铁律「蝶翅只认纸底」）+ 蝶形 A 变体居中
#  （藤紫上翅 + 金下翅 + 墨蝶身 + 触角 + 金焊盘）。小尺寸（16/24）加粗线宽
#  保证可辨。
#
#  用法：python gen_icon.py [输出路径]
# ============================================================================
import math
import os
import struct
import sys

SS = 8          # 超采样倍数（2048 = 256*8，小尺寸圆头质量关键）
BASE = 256

# ---- VI-D2 身份色 ----
PAPER = (0xFB, 0xF7, 0xEC)
INK = (0x3A, 0x3A, 0x44)
WING = (0x7C, 0x5C, 0xBF)
SIGNAL = (0xF2, 0xB4, 0x41)

# ---- 蝶形画布坐标（240×160，与 brand.cpp 常量表逐项一致；y 向下）----
# 上翅两片 96×62（圆角 18）；下翅两片 96×44（圆角 14）；蝶身胶囊 12×96；
# 触角 45° 仰角；焊盘 OD 12；过孔 r 2.8。
TRACE_W = 3.5
PAD_OD = 12.0
PAD_THK = 3.0
VIA_R = 2.8
ANT_W = 3.5


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def inside_round_rect(x, y, rx, ry, rw, rh, r):
    """圆角矩形内距离（<=0 内部，返回有符号距离近似）"""
    if rw <= 0 or rh <= 0:
        return 1.0
    r = min(r, rw / 2, rh / 2)
    cx2 = rx + rw
    cy2 = ry + rh
    qx = max(rx + r - x, x - (cx2 - r), 0)
    qy = max(ry + r - y, y - (cy2 - r), 0)
    outside = math.hypot(qx, qy)
    inside = min(max(rx + r - x, x - (cx2 - r)), max(ry + r - y, y - (cy2 - r)))
    return max(outside, inside) if outside > 0 else inside


def inside_ellipse(x, y, cx, cy, a, b):
    if a <= 0 or b <= 0:
        return 1.0
    return math.hypot((x - cx) / a, (y - cy) / b) - 1.0


def dist_seg(px, py, ax, ay, bx, by):
    """点到线段距离"""
    dx, dy = bx - ax, by - ay
    if dx == dy == 0:
        return math.hypot(px - ax, py - ay)
    t = clamp01(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def coverage(v, aa):
    """有符号距离 → 覆盖度（1px AA 过渡带，超采样坐标系下按 aa 缩放）"""
    return clamp01(0.5 - v / aa)


def render(scale_boost=1.0):
    """渲染 256×SS 基准画布 → 返回 (pixels, N)"""
    N = BASE * SS
    aa = SS  # 1 逻辑像素的过渡带（超采样坐标）

    # 纸底圆角方块（全画布，圆角 48 → 视觉 256 系的 48/256*4=12? 取 0.19 比例）
    R_BG = 0.19 * N

    def to_canvas(px, py):
        """图标画布 256×256 中心正方形 240×160 蝶形画布映射"""
        # 蝶形画布 240×160 放在图标中央，占宽 82%
        cw, chh = 0.82 * N, 0.82 * N * (160 / 240)
        ox = (N - cw) / 2
        oy = (N - chh) / 2
        return ox + px / 240 * cw, oy + py / 160 * chh

    out = bytearray(N * N * 4)

    # 蝶形几何（画布坐标 240×160）
    shapes_wing = []      # 上翅两片（圆角矩形）
    shapes_gold = []      # 下翅两片
    shapes_body = []      # 蝶身胶囊
    traces = []           # 走线线段 (ax,ay,bx,by,width)
    pads = []             # 焊盘 (cx,cy,od,thk)
    vias = []             # 过孔 (cx,cy,r)

    for sx, x0 in ((1, 24), (-1, 120)):   # 左/右镜像：左翅 x 24..120，右翅 x 120..216
        shapes_wing.append((x0, 28, 96, 62, 18))
    for sx, x0 in ((1, 24), (-1, 120)):
        shapes_gold.append((x0, 88, 96, 44, 14))
    shapes_body.append((114, 34, 12, 96, 6))   # 胶囊 r = w/2
    # 走线：左 (108,84)→(72,48)→(84,36)；右镜像 (132,84)→(168,48)→(156,36)
    traces.append((108, 84, 72, 48, TRACE_W))
    traces.append((72, 48, 84, 36, TRACE_W))
    traces.append((132, 84, 168, 48, TRACE_W))
    traces.append((168, 48, 156, 36, TRACE_W))
    # 触角：左 (117,34)→(101,18)；右 (123,34)→(139,18)
    traces.append((117, 34, 101, 18, ANT_W))
    traces.append((123, 34, 139, 18, ANT_W))
    # 焊盘（走线末端）与翅根过孔
    pads.append((84, 36, PAD_OD, PAD_THK))
    pads.append((156, 36, PAD_OD, PAD_THK))
    vias.append((108, 88, VIA_R))
    vias.append((132, 88, VIA_R))

    for py_i in range(N):
        py = py_i + 0.5
        row = py_i * N * 4
        for px_i in range(N):
            px = px_i + 0.5
            i = row + px_i * 4

            # ---- 背景：纸底圆角方块（圆角外透明）----
            bg_cov = coverage(-inside_round_rect(px, py, 0, 0, N, N, R_BG), aa)
            if bg_cov <= 0:
                continue
            r, g, b, a = *PAPER, int(255 * bg_cov)

            # ---- 蝶形（画布坐标）----
            cx, cy = to_canvas(px, py)

            # 焊盘金（先铺走线与翅，焊盘最上层）
            best = None

            # 蝶身（墨）
            for (rx, ry, rw, rh, rr) in shapes_body:
                v = inside_round_rect(cx, cy, rx, ry, rw, rh, rr)
                if v <= 0:
                    best = (*INK, 255)
                    break

            # 走线与触角（蝶身色墨；只画 45° 段——坐标表保证）
            if best is None:
                for (ax, ay, bx, by, w) in traces:
                    d = dist_seg(cx, cy, ax, ay, bx, by) - (w / 2 + 0.6)
                    if d <= 0:
                        best = (*INK, 255)
                        break

            # 下翅（金）
            if best is None:
                for (rx, ry, rw, rh, rr) in shapes_gold:
                    v = inside_round_rect(cx, cy, rx, ry, rw, rh, rr)
                    if v <= 0:
                        best = (*SIGNAL, 255)
                        break

            # 上翅（藤紫）
            if best is None:
                for (rx, ry, rw, rh, rr) in shapes_wing:
                    v = inside_round_rect(cx, cy, rx, ry, rw, rh, rr)
                    if v <= 0:
                        best = (*WING, 255)
                        break

            # 焊盘金环（压在翅/走线之上）
            if best is None:
                for (pcx, pcy, od, thk) in pads:
                    rr = (od - thk) / 2
                    v = abs(math.hypot(cx - pcx, cy - pcy) - rr) - thk / 2
                    if v <= 0:
                        best = (*SIGNAL, 255)
                        break

            # 过孔（墨点）
            if best is None:
                for (vcx, vcy, vr) in vias:
                    v = math.hypot(cx - vcx, cy - vcy) - vr
                    if v <= 0:
                        best = (*INK, 255)
                        break

            if best is not None:
                r, g, b, a = *best[:3], int(255 * bg_cov)

            out[i] = b
            out[i + 1] = g
            out[i + 2] = r
            out[i + 3] = a
    return out, N


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

    print("[1/3] rendering %dx%d (supersampled butterfly) ..." % (BASE * SS, BASE * SS))
    big, n = render()

    print("[2/3] downsampling ...")
    base, _ = box_down(big, n, SS)             # 2048 -> 256
    sizes = [16, 24, 32, 48, 256]
    images = []
    cache = {256: base}
    for s in sizes:
        if s == 256:
            images.append((s, base))
            continue
        # 找最近的更大源（256 或 96 级联），小尺寸用级联降采样保持圆头
        src_n = 256 if s > 48 else 96
        if src_n not in cache:
            cache[src_n] = bilinear_down(base, 256, src_n)
        images.append((s, bilinear_down(cache[src_n], src_n, s)))

    print("[3/3] writing ICO ...")
    total = write_ico(out_path, images)
    print("OK -> %s (%d bytes, sizes: %s)" % (out_path, total,
                                              ",".join(str(s) for s in sizes)))


if __name__ == "__main__":
    main()
