// ============================================================================
//  T02 清理引擎验收测试（不进主程序，仅测试用）
//  构造 %TEMP%\mz_t02_sandbox 沙箱，验证 DeleteFileSmart L1~L4 /
//  CleanDirectoryContentsEx 真实释放量 / IsDangerousCleanRoot / ErrorTextZh
//  构建：g++ -std=c++20 -DUNICODE -D_UNICODE -municode -I src
//        tools/t02_clean_test.cpp src/cleanresult.cpp src/cleaner.cpp
//        src/common.cpp -o build/t02_check.exe -lshell32 -lole32 -luuid
// ============================================================================
#include "../src/cleanresult.h"
#include "../src/cleaner.h"

#include <windows.h>
#include <cstdio>
#include <string>

using namespace mz;

static int gPass = 0, gFail = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { ++gPass; wprintf(L"  [PASS] %s\n", msg); }              \
        else { ++gFail; wprintf(L"  [FAIL] %s\n", msg); }                   \
    } while (0)

static wstring EnvStr2(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? wstring(buf, n) : wstring();
}

static wstring Join2(const wstring& a, const wstring& b) {
    wstring s = a;
    if (!s.empty() && s.back() != L'\\') s.push_back(L'\\');
    return s + b;
}

// 测试侧长路径前缀（引擎内部同样处理，这里只是让测试脚手架能创建超长路径）
static wstring LongP(const wstring& p) {
    if (p.size() < 240) return p;
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    return L"\\\\?\\" + p;
}

static bool MakeDir(const wstring& p) {
    if (CreateDirectoryW(LongP(p).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool WriteFileSimple(const wstring& path, const std::string& content, DWORD extraAttr = 0) {
    HANDLE h = CreateFileW(LongP(path).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | extraAttr, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    return written == content.size();
}

static unsigned long long FileSz(const wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(LongP(path).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static bool Exists(const wstring& p) {
    return GetFileAttributesW(LongP(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

int wmain() {
    wprintf(L"=== T02 清理引擎测试 ===\n\n");

    // ---------- 0. ErrorTextZh ----------
    wprintf(L"[组0] ErrorTextZh 中文映射\n");
    CHECK(ErrorTextZh(32) == L"被其他程序占用", L"错误码 32 → 被其他程序占用");
    CHECK(ErrorTextZh(5) == L"需要管理员权限", L"错误码 5 → 需要管理员权限");
    CHECK(ErrorTextZh(206) == L"路径过长", L"错误码 206 → 路径过长");
    CHECK(ErrorTextZh(0x80070020) == L"被其他程序占用", L"HRESULT 0x80070020 → 被其他程序占用");

    // ---------- 1. IsDangerousCleanRoot ----------
    wprintf(L"\n[组1] IsDangerousCleanRoot 危险路径终检\n");
    wstring sandbox = Join2(EnvStr2(L"TEMP"), L"mz_t02_sandbox");
    CHECK(IsDangerousCleanRoot(L"C:\\"),                    L"盘根 C:\\ 拒绝");
    CHECK(IsDangerousCleanRoot(L"C:\\Windows"),             L"Windows 本体拒绝");
    CHECK(IsDangerousCleanRoot(L"C:\\Windows\\System32\\DriverStore\\FileRepository"),
                                                          L"DriverStore\\FileRepository 拒绝");
    CHECK(IsDangerousCleanRoot(L"C:\\Windows\\WinSxS"),     L"WinSxS 拒绝");
    CHECK(IsDangerousCleanRoot(L"C:\\Program Files\\SomeApp"), L"Program Files 整树拒绝");
    CHECK(IsDangerousCleanRoot(L"C:\\ProgramData\\Microsoft"), L"ProgramData 第一层拒绝");
    CHECK(!IsDangerousCleanRoot(
        L"C:\\ProgramData\\Microsoft\\Windows Defender\\Scans"), L"ProgramData 深层放行");
    CHECK(IsDangerousCleanRoot(EnvStr2(L"USERPROFILE")),    L"用户根目录拒绝");
    CHECK(!IsDangerousCleanRoot(
        Join2(EnvStr2(L"USERPROFILE"), L"AppData\\Local\\Temp")), L"AppData\\Local\\Temp 放行");
    CHECK(IsDangerousCleanRoot(L"C:\\$Recycle.Bin"),        L"回收站目录拒绝");
    CHECK(!IsDangerousCleanRoot(sandbox),                   L"沙箱目录放行");

    // ---------- 2. 沙箱构造 ----------
    wprintf(L"\n[组2] 沙箱构造（%%TEMP%%\\mz_t02_sandbox）\n");
    // 清掉旧沙箱
    if (Exists(sandbox)) {
        CleanReport dummy;
        int budget = 0;
        CleanDirectoryContentsEx(sandbox, nullptr, dummy, &budget);
        RemoveDirectoryW(sandbox.c_str());
    }
    CHECK(MakeDir(sandbox), L"创建沙箱根目录");
    wstring normalF  = Join2(sandbox, L"normal.txt");
    wstring roF      = Join2(sandbox, L"readonly.dat");
    wstring hiddenF  = Join2(sandbox, L"hidden.sys");
    wstring lockedF  = Join2(sandbox, L"locked.bin");
    CHECK(WriteFileSimple(normalF, std::string(4096, 'A')), L"写入普通文件 normal.txt (4KB)");
    CHECK(WriteFileSimple(roF, std::string(2048, 'B'), FILE_ATTRIBUTE_READONLY),
          L"写入只读文件 readonly.dat (2KB)");
    CHECK(WriteFileSimple(hiddenF, std::string(1024, 'C'),
                          FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
          L"写入 隐藏+系统 文件 hidden.sys (1KB)");
    CHECK(WriteFileSimple(lockedF, std::string(8192, 'D')), L"写入将加锁文件 locked.bin (8KB)");
    // >260 字符深路径（每层 ~50 字符 × 6 层 + 文件名 ≈ 320 字符）
    wstring deepDir = sandbox;
    for (int i = 0; i < 6; ++i) {
        deepDir = Join2(deepDir, L"very_long_directory_name_level_" +
                                      std::to_wstring(i) + L"_padding_padding");
        CHECK(MakeDir(deepDir), (L"创建深路径第 " + std::to_wstring(i) + L" 层").c_str());
    }
    wstring deepF = Join2(deepDir, L"deep_file_beyond_maxpath.txt");
    CHECK(WriteFileSimple(deepF, std::string(1536, 'E')), L"写入深路径文件 (1.5KB)");
    wprintf(L"  深路径长度 = %zu 字符\n", deepF.size());
    CHECK(deepF.size() > 260, L"深路径确实超过 260 字符");

    unsigned long long expectFreed = 4096 + 2048 + 1024 + 1536;   // locked.bin 除外

    // ---------- 3. 清理执行 ----------
    wprintf(L"\n[组3] CleanDirectoryContentsEx 逐文件智能删除\n");
    // 打开不共享 DELETE 的句柄锁定 locked.bin
    HANDLE hLock = CreateFileW(lockedF.c_str(), GENERIC_READ,
                               FILE_SHARE_READ,   // 不给 FILE_SHARE_DELETE
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(hLock != INVALID_HANDLE_VALUE, L"打开句柄锁定 locked.bin（非提权下 L4 不可用）");

    CleanReport report;
    int rebootBudget = 50;
    unsigned long long freed =
        CleanDirectoryContentsEx(sandbox, nullptr, report, &rebootBudget);

    if (hLock != INVALID_HANDLE_VALUE) CloseHandle(hLock);

    wprintf(L"  fileOk=%d fileFail=%d rebootPending=%d freed=%llu (期望 %llu)\n",
            report.fileOk, report.fileFail, report.rebootPending, freed, expectFreed);
    CHECK(report.fileOk >= 3, L"普通/只读/隐藏+系统 文件至少 3 个删除成功");
    CHECK(report.fileFail >= 1, L"被锁定文件失败被如实记录（不再吞掉）");
    CHECK(report.fails.size() >= 1 && report.fails[0].path == lockedF,
          L"失败明细包含 locked.bin 完整路径");
    CHECK(!report.failCodes.empty(), L"失败原因分布（错误码→数量）已生成");
    CHECK(!report.FailSummary().empty(), L"FailSummary 生成中文原因汇总");
    CHECK(freed == expectFreed,
          L"真实释放量 = 实际删除文件大小之和（4KB+2KB+1KB+1.5KB，不冒充）");

    // 删除成功项验证（G5：已成功项不再出现）
    CHECK(!Exists(normalF), L"G5: normal.txt 已删除");
    CHECK(!Exists(roF),     L"G5: readonly.dat 已删除（L2 属性剥离生效）");
    CHECK(!Exists(hiddenF), L"G5: hidden.sys 已删除（L2 属性剥离生效）");
    CHECK(!Exists(deepF),   L"G5: >260 字符深路径文件已删除（\\?\\ 长路径生效）");
    CHECK(Exists(lockedF),  L"G5: locked.bin 仍在（失败项如实保留）");

    // ---------- 4. 解锁后二次清理 ----------
    wprintf(L"\n[组4] 解锁后二次清理（失败可恢复）\n");
    CleanReport report2;
    int budget2 = 50;
    unsigned long long freed2 = CleanDirectoryContentsEx(sandbox, nullptr, report2, &budget2);
    CHECK(report2.fileOk >= 1 && report2.fileFail == 0, L"解锁后 locked.bin 清理成功");
    CHECK(freed2 == 8192, L"二次释放量 = 8KB");
    CHECK(!Exists(lockedF), L"G5: 二次清理后 locked.bin 不再出现");

    // ---------- 5. 取消响应 ----------
    wprintf(L"\n[组5] 取消中断\n");
    CHECK(MakeDir(sandbox), L"重建沙箱");
    for (int i = 0; i < 20; ++i)
        WriteFileSimple(Join2(sandbox, L"f" + std::to_wstring(i) + L".tmp"),
                        std::string(100, 'x'));
    CleanReport report3;
    int budget3 = 50;
    bool cancelled = true;   // 立即取消
    unsigned long long freed3 =
        CleanDirectoryContentsEx(sandbox, [&]() { return cancelled; }, report3, &budget3);
    (void)freed3;
    int remaining = 0;
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join2(sandbox, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) ++remaining;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    CHECK(remaining > 0, L"取消后文件保留（立即停止，未继续删除）");

    // 清理沙箱
    CleanReport report4;
    int budget4 = 50;
    CleanDirectoryContentsEx(sandbox, nullptr, report4, &budget4);
    RemoveDirectoryW(sandbox.c_str());

    wprintf(L"\n=== 结果: %d PASS / %d FAIL ===\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
