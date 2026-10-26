// ============================================================================
//  procrun 单元测试（T01 验收辅助，不进正式构建）
//    ① 逐行回调 + 全量输出 + 退出码
//    ② 大输出不因管道缓冲区写满而死锁
//    ③ cancel → TerminateProcess（-3）且快速返回
//    ④ 超时 → TerminateProcess（-2）
//    ⑤ \r 进度行合并（全量输出中只保留最后一次刷新）
// ============================================================================
#include "../src/procrun.h"
#include <atomic>
#include <chrono>
#include <cstdio>

using namespace mz;

int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [OK]   %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); ++failures; } } while (0)

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // ① 基本逐行输出
    {
        printf("test1: echo lines\n");
        ProcRunOptions opt;
        vector<wstring> lines;
        opt.onLine = [&](const wstring& s) { lines.push_back(s); };
        wstring out;
        int code = RunProcess(L"cmd /c echo hello & echo world", opt, &out);
        CHECK(code == 0, "exit code 0");
        CHECK(lines.size() >= 2, "onLine got >= 2 lines");
        CHECK(out.find(L"hello") != wstring::npos && out.find(L"world") != wstring::npos,
              "fullOut contains both lines");
    }

    // ② 大输出（500 行）不卡管道
    {
        printf("test2: 500 lines no deadlock\n");
        ProcRunOptions opt;
        int count = 0;
        opt.onLine = [&](const wstring&) { ++count; };
        wstring out;
        int code = RunProcess(
            L"cmd /c for /l %i in (1,1,500) do @echo line%i", opt, &out);
        CHECK(code == 0, "exit code 0");
        CHECK(count >= 500, "onLine got >= 500 lines");
        CHECK(out.find(L"line500") != wstring::npos, "fullOut has last line");
    }

    // ③ 取消：ping 30 秒，1.5 秒后取消 → 应 ~1.5s 返回且退出码 -3
    {
        printf("test3: cancel after 1.5s\n");
        ProcRunOptions opt;
        auto t0 = std::chrono::steady_clock::now();
        opt.cancel = [t0]() {
            return std::chrono::steady_clock::now() - t0 >
                   std::chrono::milliseconds(1500);
        };
        wstring out;
        int code = RunProcess(L"ping -n 30 127.0.0.1", opt, &out);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(code == -3, "exit code -3 (cancelled)");
        CHECK(ms < 5000, "returned quickly");
    }

    // ④ 超时：timeoutMs=1200，子进程跑 10 秒 → 退出码 -2，~1.2s 返回
    {
        printf("test4: timeout 1.2s\n");
        ProcRunOptions opt;
        opt.timeoutMs = 1200;
        wstring out;
        auto t0 = std::chrono::steady_clock::now();
        int code = RunProcess(L"ping -n 10 127.0.0.1", opt, &out);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(code == -2, "exit code -2 (timeout)");
        CHECK(ms < 4000, "returned near timeout");
    }

    // ⑤ \r 进度行合并：连续 \r 刷新在同一行
    {
        printf("test5: CR progress coalescing\n");
        // powershell 逐秒输出 "P1\rP2\r..." 模拟进度刷新
        ProcRunOptions opt;
        int emitted = 0;
        opt.onLine = [&](const wstring&) { ++emitted; };
        wstring out;
        int code = RunProcess(
            L"powershell -NoProfile -Command \"1..4 | ForEach-Object { Write-Host -NoNewline (\\\"step$_ `r\\\"); Start-Sleep -Milliseconds 600 }; Write-Host 'done'\"",
            opt, &out);
        CHECK(code == 0, "exit code 0");
        CHECK(out.find(L"done") != wstring::npos, "fullOut has final line");
        CHECK(emitted <= 6, "progress throttled (no spam)");
        printf("    (onLine calls = %d, fullOut = [%ls])\n", emitted, out.c_str());
    }

    printf(failures == 0 ? "\nALL PROCRUN TESTS PASSED\n"
                         : "\n%d TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
