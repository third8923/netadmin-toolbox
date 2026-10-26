import struct, zlib, os, sys

def read_ico(path):
    d = open(path, 'rb').read()
    count = struct.unpack('<H', d[4:6])[0]
    for i in range(count):
        off = 6 + i * 16
        w, h, _, _, planes, bits, size, offset = struct.unpack('<BBBBHHII', d[off:off + 16])
        if w == 0:
            w = 256
        if w == 256:
            return d[offset:offset + size], w
    return None, 0

def bmp_to_png(bmp, n):
    px = bmp[40:]
    rows = []
    for y in range(n - 1, -1, -1):
        row = bytearray()
        base = y * n * 4
        for x in range(n):
            i = base + x * 4
            row += bytes((px[i + 2], px[i + 1], px[i], px[i + 3]))
        rows.append(bytes(row))
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    return png

bmp, n = read_ico(sys.argv[1])
out = os.path.join(os.environ['TEMP'], 'icon_preview.png')
open(out, 'wb').write(bmp_to_png(bmp, n))
print('saved', out)
