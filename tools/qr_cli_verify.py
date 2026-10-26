# -*- coding: utf-8 -*-
"""Rebuild QR matrix from CLI terminal text (half-block chars) and verify.

No row/column filtering: every text line encodes 2 matrix rows (upper/lower).
Spaces are white modules and are significant (quiet zone is spaces).
"""
import sys, io

TXT = "build/qr_cli.txt"
REF = "build/tc.pbm"      # qr_test.exe PBM for the same payload (zxing-verified)
OUT = "build/qr_rebuild.pbm"

BLOCKS = {"\u2588": (1, 1), "\u2580": (1, 0), "\u2584": (0, 1), " ": (0, 0)}

def read_lines(path):
    with open(path, "rb") as f:
        data = f.read()
    text = data.decode("utf-8")
    # normalize \r\r\n / \r\n / \n
    text = text.replace("\r\n", "\n").replace("\r", "")
    return text.split("\n")

def parse_cli_matrix(path):
    lines = read_lines(path)
    # QR lines = lines containing at least one block char (solid/half);
    # every line inside the QR region has >=1 black module so this cannot
    # drop an interior line. Quiet-zone lines are all spaces -> skipped,
    # quiet zone is re-added when decoding, which is semantically identical.
    qr_lines = [ln for ln in lines if any(ch in BLOCKS and ch != " " for ch in ln)]
    if not qr_lines:
        raise SystemExit("no QR lines found")
    rows = []
    for ln in qr_lines:
        for ch in ln:
            if ch not in BLOCKS:
                raise SystemExit("unexpected char in QR line: %r" % ch)
        up = [BLOCKS[ch][0] for ch in ln]
        lo = [BLOCKS[ch][1] for ch in ln]
        rows.append(up)
        rows.append(lo)
    # normalize width (pad white on the right)
    w = max(len(r) for r in rows)
    rows = [r + [0] * (w - len(r)) for r in rows]
    # drop trailing all-white padding row (odd row count from line pairing)
    while rows and not any(rows[-1]):
        rows.pop()
    return rows

def read_pbm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P4", "not P4"
    # parse two integer fields after magic
    idx = 2
    vals = []
    while len(vals) < 2:
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        # skip comments
        if data[idx:idx+1] == b"#":
            while data[idx:idx+1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1  # single whitespace after last field
    w, h = vals
    rowbytes = (w + 7) // 8
    px = []
    for r in range(h):
        rowdata = data[idx + r * rowbytes: idx + (r + 1) * rowbytes]
        row = []
        for c in range(w):
            byte = rowdata[c // 8]
            bit = (byte >> (7 - (c % 8))) & 1
            row.append(bit)
        px.append(row)
    return px

def write_pbm(path, px):
    h = len(px)
    w = len(px[0])
    rowbytes = (w + 7) // 8
    with open(path, "wb") as f:
        f.write(b"P4\n%d %d\n" % (w, h))
        for row in px:
            buf = bytearray(rowbytes)
            for c, v in enumerate(row):
                if v:
                    buf[c // 8] |= 0x80 >> (c % 8)
            f.write(bytes(buf))

def add_quiet(px, q=4):
    w = len(px[0])
    blank = [0] * (w + 2 * q)
    out = [blank[:] for _ in range(q)]
    for r in px:
        out.append([0] * q + r + [0] * q)
    out += [blank[:] for _ in range(q)]
    return out

def diff(a, b):
    if len(a) != len(b):
        return ["row count %d vs %d" % (len(a), len(b))]
    msgs = []
    mm = 0
    for r, (ra, rb) in enumerate(zip(a, b)):
        if len(ra) != len(rb):
            msgs.append("row %d width %d vs %d" % (r, len(ra), len(rb)))
            continue
        for c, (va, vb) in enumerate(zip(ra, rb)):
            if va != vb:
                mm += 1
                if mm <= 10:
                    msgs.append("(%d,%d) cli=%d ref=%d" % (r, c, va, vb))
    msgs.insert(0, "mismatch: %d" % mm)
    return msgs

def main():
    cli = parse_cli_matrix(TXT)
    ref = read_pbm(REF)
    print("cli matrix: %dx%d" % (len(cli[0]), len(cli)))
    print("ref  matrix: %dx%d" % (len(ref[0]), len(ref)))
    # full diff against ref (ref includes quiet zone 4)
    cli_q = add_quiet(cli, 4)
    for m in diff(cli_q, ref):
        print("DIFF", m)
    # also compare WITHOUT quiet zone by trimming ref
    core = [row[4:-4] for row in ref[4:-4]]
    for m in diff(cli, core):
        print("CORE-DIFF", m)
    # write rebuilt PBM with quiet zone and decode
    out = add_quiet(cli, 4)
    write_pbm(OUT, out)
    # sanity: re-read our own PBM
    back = read_pbm(OUT)
    for m in diff(back, out)[:5]:
        print("PBM-SELF", m)
    print("wrote", OUT)

if __name__ == "__main__":
    main()
