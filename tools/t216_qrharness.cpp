// 独立验收测试壳（团队 lead 手写）：调用 qr::Encode 导出 PBM 位图
// 用法: t216_qrharness.exe <out.pbm> <payload...>   （payload 为 UTF-8 argv 拼接）
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include "qr.h"

// 独立存根：避免链入 common.cpp（其依赖 COM/taskschd）
namespace mz {
std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}
} // namespace mz

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: harness <out.pbm> <payload>\n"); return 1; }
    std::string payload = argv[2];
    auto m = qr::EncodeBytes(payload);
    if (m.empty()) { fprintf(stderr, "ENCODE_FAILED\n"); return 2; }
    FILE* f = fopen(argv[1], "wb");
    fprintf(f, "P1\n%d %d\n", (int)m[0].size(), (int)m.size());
    for (const auto& row : m) {
        for (bool b : row) fputc(b ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "PBM written %zux%zu\n", m[0].size(), m.size());
    return 0;
}
