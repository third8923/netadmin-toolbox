# -*- coding: utf-8 -*-
"""ztest_stress.py — INFLATE 压力测试：9 个压缩等级 × 3 种数据形态，对比 zlib 基准"""
import zlib, random, subprocess

random.seed(42)
text = ('NETAdmin toolbox stress test with mixed content 中国文字 abc 0123456789\r\n' * 40).encode('utf-8')
binary = bytes(random.randrange(256) for _ in range(60000))
repeat = b'AB' * 30000

fails = 0
total = 0
for name, data in [('text', text), ('binary', binary), ('repeat', repeat)]:
    for lvl in range(1, 10):
        total += 1
        co = zlib.compressobj(lvl, zlib.DEFLATED, -15)
        stream = co.compress(data) + co.flush()
        open('build/stress.deflate', 'wb').write(stream)
        r = subprocess.run(['build/ztest.exe', 'build/stress.deflate'],
                           capture_output=True, text=True, timeout=60)
        want = 'err=0 size=%d crc=0x%08X' % (len(data), zlib.crc32(data) & 0xFFFFFFFF)
        if r.stdout.strip() != want:
            fails += 1
            print('FAIL', name, 'lvl', lvl, '->', r.stdout.strip(), 'want', want)
print('stress fails = %d / %d' % (fails, total))
