# -*- coding: utf-8 -*-
"""make_test_zips.py — 生成 ZipCrypto 加密测试 ZIP（stdlib 无法写加密 zip，手写加密）
用法: python make_test_zips.py <输出目录>
"""
import os, struct, sys, zlib

def _keys_init():
    return [0x12345678, 0x23456789, 0x34567890]

def _crc_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (0xEDB88320 ^ (c >> 1)) if (c & 1) else (c >> 1)
        tab.append(c)
    return tab
_TAB = _crc_table()

def _update(keys, b):
    keys[0] = _TAB[(keys[0] ^ b) & 0xFF] ^ (keys[0] >> 8)
    keys[1] = (keys[1] + (keys[0] & 0xFF)) & 0xFFFFFFFF
    keys[1] = (keys[1] * 134775813 + 1) & 0xFFFFFFFF
    # key2 用 key1 高字节（7-Zip/CPython 实测标准）
    keys[2] = _TAB[(keys[2] ^ ((keys[1] >> 24) & 0xFF)) & 0xFF] ^ (keys[2] >> 8)

def _dec_byte(keys):
    t = (keys[2] | 2) & 0xFFFF
    return ((t * (t ^ 1)) >> 8) & 0xFF

def encrypt_chunk(keys, data):
    """ZipCrypto 加密：cipher = plain ^ decrypt_byte()，密钥用【明文字节】更新（PKWARE APPNOTE）"""
    out = bytearray()
    for b in data:
        c = b ^ _dec_byte(keys)
        _update(keys, b)
        out.append(c)
    return bytes(out)

def make_zip(path, entries, password):
    """entries: [(name, data_bytes, method)]  method: 0=stored 8=deflate"""
    password_bytes = password.encode('gbk')
    locals_, centrals, offset = [], [], 0
    for name, data, method in entries:
        nb = name.encode('utf-8')
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if method == 8:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = co.compress(data) + co.flush()
        else:
            comp = data
        mod_time, mod_date = 0x4800, 0x5A4C  # 固定 DOS 时间（非 data-descriptor 场景）
        keys = _keys_init()
        for b in password_bytes:
            _update(keys, b)
        header = bytearray(os.urandom(11))
        header.append((crc >> 24) & 0xFF)
        enc_header = encrypt_chunk(keys, bytes(header))
        enc_data = encrypt_chunk(keys, comp)
        blob = enc_header + enc_data
        lh = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 1, method, mod_time,
                         mod_date, crc, len(blob), len(data), len(nb), 0)
        local = lh + nb + blob
        locals_.append(local)
        ch = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 1, method,
                         mod_time, mod_date, crc, len(blob), len(data),
                         len(nb), 0, 0, 0, 0, 0, offset)
        centrals.append(ch + nb)
        offset += len(local)
    cd = b''.join(centrals)
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(centrals),
                       len(centrals), len(cd), offset, 0)
    with open(path, 'wb') as f:
        f.write(b''.join(locals_)); f.write(cd); f.write(eocd)
    print('OK', path)

out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)
big = (b'The quick brown fox jumps over the lazy dog. 0123456789\r\n' * 120)
make_zip(os.path.join(out, 't_num.zip'),    [('report.txt', big, 8)], '314159')
make_zip(os.path.join(out, 't_alpha.zip'),  [('note.txt', b'secret content ' * 8, 0)], 'abc9')
make_zip(os.path.join(out, 't_gbk.zip'),    [('data.txt', big, 8)], '密码123')
# 未加密对照
with open(os.path.join(out, 't_plain.zip'), 'wb') as f:
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = co.compress(b'plain zip no password') + co.flush()
    nb = b'x.txt'
    lh = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, 8, 0x4800, 0x5A4C,
                     zlib.crc32(b'plain zip no password'), len(comp), 22, len(nb), 0)
    cd = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0, 8, 0x4800,
                     0x5A4C, zlib.crc32(b'plain zip no password'), len(comp), 22,
                     len(nb), 0, 0, 0, 0, 0, 0) + nb
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, 1, 1, len(cd),
                       len(lh) + len(nb) + len(comp), 0)
    f.write(lh + nb + comp + cd + eocd)
# 假 ZIP（解析失败对照）
with open(os.path.join(out, 't_fake.zip'), 'w') as f:
    f.write('this is not a zip file, just plain text for parse-fail test')
print('ALL DONE')
