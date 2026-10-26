# QR 矩阵逐层诊断：读 PBM -> 验证 format info BCH -> 提取码字 -> RS 校验
# 用于定位自研编码器哪一层出错
import sys
import numpy as np

def read_pbm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P4\n<w> <h>\n<binary>
    parts = data.split(b'\n', 2)
    assert parts[0] == b'P4', parts[0]
    w, h = map(int, parts[1].split())
    body = parts[2]
    rowbytes = (w + 7) // 8
    m = np.zeros((h, w), dtype=bool)
    for y in range(h):
        row = np.frombuffer(body[y*rowbytes:(y+1)*rowbytes], dtype=np.uint8)
        bits = np.unpackbits(row)[:w]
        m[y] = bits.astype(bool)
    return m

def strip_quiet(m):
    # 找黑模块范围
    rows = np.where(m.any(axis=1))[0]
    cols = np.where(m.any(axis=0))[0]
    return m[rows.min():rows.max()+1, cols.min():cols.max()+1]

def format_info_from(m):
    n = m.shape[0]
    bits = []
    # copy 2: bits LSB 0..7 at (row 8, col n-1-i); bits 8..14 at (row n-15+i, col 8)
    for i in range(8):
        bits.append(int(m[8, n-1-i]))
    for i in range(8, 15):
        bits.append(int(m[n-15+i, 8]))
    val = 0
    for b in bits:
        val = (val >> 1) | (b << 14)   # LSB first -> pack
    return val

def bch15_ok(fmt15):
    # strip mask 0x5412, check remainder of (data<<10 | bch) mod 0x537 == 0
    v = fmt15 ^ 0x5412
    rem = v
    for _ in range(15):
        # polynomial division check: compute v mod g
        pass
    # simpler: recompute expected bch from 5 data bits
    data = v >> 10
    rem = data
    for _ in range(10):
        rem = (rem << 1) ^ ((rem >> 9) * 0x537)
    expected = ((data << 10) | (rem & 0x3FF)) ^ 0x5412
    return expected == fmt15, data >> 3, data & 7   # ok, ecc, mask

def align_centers(ver):
    return {1:[],2:[6,18],3:[6,22],4:[6,26],5:[6,30],6:[6,34],
            7:[6,22,38],8:[6,24,42],9:[6,26,46],10:[6,28,50]}[ver]

VERS = {21:1,25:2,29:3,33:4,37:5,41:6,45:7,49:8,53:9,57:10}
SPEC = {  # ver: (total, ecPerBlock, blocks, dataCw)
    1:(26,10,1,16), 2:(44,16,1,28), 3:(70,26,1,44), 4:(100,18,2,64),
    5:(134,24,2,86), 6:(172,16,4,108), 7:(196,18,4,124), 8:(242,22,4,154),
    9:(292,22,6,182), 10:(346,26,6,216),
}

def function_map(n, ver):
    f = np.zeros((n, n), dtype=bool)
    def setm(r, c):
        if 0 <= r < n and 0 <= c < n: f[r, c] = True
    for i in range(n):
        setm(6, i); setm(i, 6)
    for r0, c0 in ((0,0), (0,n-7), (n-7,0)):
        for dr in range(-1, 8):
            for dc in range(-1, 8):
                setm(r0+dr, c0+dc)
    ap = align_centers(ver)
    for i in range(len(ap)):
        for j in range(len(ap)):
            if i == 0 and (j == 0 or j == len(ap)-1): continue
            if j == 0 and i == len(ap)-1: continue
            for dr in range(-2, 3):
                for dc in range(-2, 3):
                    setm(ap[i]+dr, ap[j]+dc)
    # format info
    for i in range(6): setm(8, i)
    setm(8, 7); setm(8, 8); setm(7, 8)
    for i in range(9, 15): setm(14-i, 8)
    for i in range(8): setm(8, n-1-i)
    for i in range(8, 15): setm(n-15+i, 8)
    setm(n-8, 8)
    if ver >= 7:
        bits = ver
        rem = ver
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        vb = (ver << 12) | (rem & 0xFFF)
        for i in range(18):
            a = n-11 + i % 3; b = i // 3
            setm(b, a); setm(a, b)
    return f

def mask_bit(mid, r, c):
    if mid == 0: return (r + c) % 2 == 0
    if mid == 1: return r % 2 == 0
    if mid == 2: return c % 3 == 0
    if mid == 3: return (r + c) % 3 == 0
    if mid == 4: return (r // 2 + c // 3) % 2 == 0
    if mid == 5: return (r*c) % 2 + (r*c) % 3 == 0
    if mid == 6: return ((r*c) % 2 + (r*c) % 3) % 2 == 0
    return ((r+c) % 2 + (r*c) % 3) % 2 == 0

def zigzag(n, f):
    order = []
    for right in range(n-1, 0, -2):
        if right == 6: right = 5
        upward = ((right + 1) & 2) == 0
        cols = (right, right-1)
        rows = range(n-1, -1, -1) if upward else range(n)
        for r in rows:
            for c in cols:
                if not f[r, c]:
                    order.append((r, c))
    return order

def gf_mul(a, b, exp, log):
    if a == 0 or b == 0: return 0
    return exp[log[a] + log[b]]

def rs_gen(degree, exp, log):
    g = [1]
    for d in range(degree):
        ng = [0]*(len(g)+1)
        for i, c in enumerate(g):
            ng[i] ^= gf_mul(c, exp[d], exp, log)
            ng[i+1] ^= c
        g = ng
    return g[1:]

def main():
    m = strip_quiet(read_pbm(sys.argv[1]))
    n = m.shape[0]
    ver = VERS[n]
    print("side=%d version=%d" % (n, ver))
    fmt = format_info_from(m)
    ok, ecc, mid = bch15_ok(fmt)
    print("format copy2 = 0x%04X  bch_ok=%s ecc=%d mask=%d" % (fmt, ok, ecc, mid))
    # also copy 1
    b1 = [int(m[8, i]) for i in range(6)] + [int(m[8,7]), int(m[8,8]), int(m[7,8])] + [int(m[14-i,8]) for i in range(9,15)]
    v1 = 0
    for b in b1:
        v1 = (v1 >> 1) | (b << 14)
    ok1, _, _ = bch15_ok(v1)
    print("format copy1 = 0x%04X  bch_ok=%s" % (v1, ok1))

    total, ec, blocks, dcw = SPEC[ver]
    f = function_map(n, ver)
    order = zigzag(n, f)
    print("data modules = %d, expected bits = %d" % (len(order), total*8))
    # extract
    bits = []
    for r, c in order:
        bits.append(int(m[r, c]) ^ int(mask_bit(mid, r, c)))
    cw = []
    for i in range(total):
        v = 0
        for k in range(8):
            v = (v << 1) | bits[i*8+k]
        cw.append(v)
    print("first 12 codewords:", cw[:12])

    # RS check: split blocks
    nb = blocks
    base = dcw // nb
    longer = dcw % nb
    dcs, ecs = [], []
    pos = 0
    for b in range(nb):
        ln = base + (1 if b >= nb - longer else 0)
        dcs.append(cw[pos:pos+ln]); pos += ln
    for k in range(ec):
        for b in range(nb):
            ecs.append(cw[pos + k*nb + b])
    # recompute ecc per block
    exp = [0]*512; log = [0]*256
    x = 1
    for i in range(255):
        exp[i] = x; log[x] = i
        x <<= 1
        if x & 0x100: x ^= 0x11D
    for i in range(255, 512): exp[i] = exp[i-255]
    gen = rs_gen(ec, exp, log)
    for b in range(min(nb, 3)):
        d = dcs[b]
        res = [0]*ec
        for by in d:
            factor = by ^ res[0]
            res = res[1:] + [0]
            for i in range(ec):
                res[i] ^= gf_mul(gen[i], factor, exp, log)
        match = res == ecs[b*ec:(b+1)*ec]
        print("block %d ecc match=%s" % (b, match))

    # header check: mode nibble + count
    mode = cw[0] >> 4
    cbits = 8 if ver <= 9 else 16
    cnt = ((cw[0] & 0xF) << (cbits-4)) | (cw[1] >> 4 if cbits > 8 else 0)
    if cbits == 8:
        cnt = cw[0] & 0xF
    print("mode nibble=%d (expect 4), count=%d" % (mode, cnt))

main()
