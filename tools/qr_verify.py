# QR 编码器验证：位图 -> 解码 -> 比对原串
# 优先 zxing-cpp（权威），cv2 QRCodeDetector 兜底
# 用法: python qr_verify.py <image> <expected-payload>
import sys
import cv2
import numpy as np

pbm, expected = sys.argv[1], sys.argv[2]
img = cv2.imread(pbm, cv2.IMREAD_GRAYSCALE)
if img is None:
    print("READ_FAIL")
    sys.exit(1)

decoded = None
try:
    import zxingcpp
    for r in zxingcpp.read_barcodes(img):
        decoded = r.text
        break
    if not decoded:
        for scale in (4, 8):
            big = cv2.resize(img, None, fx=scale, fy=scale,
                             interpolation=cv2.INTER_NEAREST)
            for r in zxingcpp.read_barcodes(big):
                decoded = r.text
                break
            if decoded:
                break
except ImportError:
    pass

if not decoded:
    det = cv2.QRCodeDetector()
    for scale in (4, 2, 1, 8):
        big = img if scale == 1 else cv2.resize(img, None, fx=scale, fy=scale,
                                                interpolation=cv2.INTER_NEAREST)
        data, pts, _ = det.detectAndDecode(big)
        if data:
            decoded = data
            break

if not decoded:
    print("DECODE_FAIL")
    sys.exit(1)

status = "PASS" if decoded == expected else "MISMATCH"
print("%s decoded=%r expected=%r" % (status, decoded, expected))
sys.exit(0 if decoded == expected else 1)
