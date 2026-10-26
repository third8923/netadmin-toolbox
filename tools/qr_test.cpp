// ============================================================================
//  QR 编码器独立测试壳（不进主程序；构建产物到 build/qr_test.exe）
//  用法：qr_test.exe <payload> <out.pbm> [--dump <seq.txt>]
//    * stdout 输出控制台块字符二维码（UTF-8）
//    * 写 PBM(P4) 位图（含 quiet zone 4，1 = 黑）供 Python OpenCV 解码验证
//    * --dump <file> 把交织后码字序列逐行写入文件（对照独立实现）
// ============================================================================
#include "../src/qr.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 测试钩子：qr.cpp 编译期开启 QR_TEST_DUMP 时，通过 qr::TestDumpSeq 回调导出
namespace qr {
extern void (*g_testDumpSeq)(const unsigned char* seq, int len);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: qr_test <payload> <out.pbm> [--dump f]\n"); return 2; }
    std::string payload = argv[1];
    static std::string dumpData;
    if (argc >= 5 && std::string(argv[3]) == "--dump") {
        qr::g_testDumpSeq = [](const unsigned char* seq, int len) {
            dumpData.assign((const char*)seq, (size_t)len);
        };
    }
    std::vector<std::vector<bool>> m = qr::EncodeBytes(payload);
    if (m.empty()) { fprintf(stderr, "ENCODE FAILED (empty payload or over capacity %zu bytes)\n", payload.size()); return 1; }
    int n = (int)m.size();
    fprintf(stderr, "encoded %zu bytes -> version side %d\n", payload.size(), n);

    if (!dumpData.empty()) {
        FILE* df = fopen(argv[4], "wb");
        if (df) {
            for (unsigned char c : dumpData) fprintf(df, "%u\n", (unsigned)c);
            fclose(df);
        }
    }

    // stdout：控制台块字符
    std::string txt = qr::ToConsoleUtf8(m);
    fwrite(txt.data(), 1, txt.size(), stdout);
    fflush(stdout);

    // PBM P4（quiet zone 4）
    int W = n + 8, H = n + 8;
    FILE* f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fprintf(f, "P4\n%d %d\n", W, H);
    std::vector<unsigned char> row((W + 7) / 8, 0);
    for (int y = 0; y < H; ++y) {
        std::fill(row.begin(), row.end(), 0);
        for (int x = 0; x < W; ++x) {
            bool dark = false;
            int qr = y - 4, qc = x - 4;
            if (qr >= 0 && qr < n && qc >= 0 && qc < n) dark = m[qr][qc];
            if (dark) row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return 0;
}
