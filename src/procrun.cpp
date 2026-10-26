// ============================================================================
//  流式进程执行器 —— 实现（见 procrun.h 头注释）
// ============================================================================
#include "procrun.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace mz {
namespace {

// 源行转 UTF-16：控制台工具（DISM/chkdsk/netsh）重定向输出为 OEM 编码，
// 故 OEMCP 优先；带 UTF-8 BOM 的按 UTF-8；再失败回落 ANSI。
// 特例：sfc /scannow 的重定向输出是 UTF-16LE（字符间夹 NUL 字节），
// 以「存在内部 NUL 字节」判定并优先按宽字符解码，避免 OEM 解码成乱码。
// 切行安全性：UTF-16LE 下 0x0D/0x0A 只作为 ASCII 换行出现（CJK 码位不含
// 0x0Axx/0x0Dxx 段常用字符），字节层 \r\n 切分依旧成立。
wstring DecodeLine(const std::string& bytes) {
    if (bytes.empty()) return wstring();
    bool hasInteriorNul = false;
    for (size_t i = 1; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == '\0') { hasInteriorNul = true; break; }
    }
    if (hasInteriorNul) {
        std::string s = bytes;
        if (s.size() % 2) s.pop_back();               // 对齐 UTF-16 单元
        while (s.size() >= 2 && s[s.size() - 1] == '\0' && s[s.size() - 2] == '\0')
            s.resize(s.size() - 2);                   // 去尾部 NUL 填充
        if (s.empty()) return wstring();
        return wstring((const wchar_t*)s.data(), s.size() / 2);
    }
    UINT cp = CP_UTF8;
    // 严格 UTF-8 优先（MB_ERR_INVALID_CHARS）：GBK 字节序列几乎必然校验失败
    // 而回落 OEMCP；控制台 CP=65001 下子进程输出 UTF-8 则直接命中；
    // 纯 ASCII 两者等价。
    int wn = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(),
                                 (int)bytes.size(), nullptr, 0);
    if (wn <= 0) { cp = CP_OEMCP; wn = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0); }
    if (wn <= 0) { cp = CP_ACP;   wn = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0); }
    wstring line(wn > 0 ? wn : 0, L'\0');
    if (wn > 0) MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), line.data(), wn);
    return line;
}

} // namespace

int RunProcess(const wstring& cmdline, const ProcRunOptions& opt, wstring* fullOut) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi = {};
    wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return -1;
    }
    // 父进程写端必须立即关闭，否则读端永远等不到 EOF
    CloseHandle(hWrite);

    using clock = std::chrono::steady_clock;

    // 读线程：阻塞 ReadFile 排空管道（防管道缓冲区写满导致子进程卡死），
    // 成行即转码回调。\n 结尾 = 普通行；仅 \r 结尾 = 进度刷新行（sfc 的
    // 百分比会以 \r 反复重写同一行），做合并 + 300ms 节流，避免刷爆日志面板。
    // 字节层切行安全性：GBK 双字节第二字节（0x40~0xFE）与 UTF-8 续字节
    // （0x80~0xBF）都不含 0x0A/0x0D，因此 \r\n 切分不会切断多字节字符。
    vector<wstring> outLines;             // 全量输出（join 后给 fullOut）
    bool lastWasProgress = false;
    auto lastEmit = clock::now() - std::chrono::seconds(10);

    std::thread reader([&]() {
        auto emit = [&](const std::string& bytes, bool progress) {
            if (bytes.empty()) return;
            wstring line = DecodeLine(bytes);
            if (line.empty()) return;
            if (progress) {
                // 进度刷新：合并到同一行（全量输出保留最后一次）
                if (lastWasProgress && !outLines.empty()) outLines.back() = line;
                else outLines.push_back(line);
                lastWasProgress = true;
                auto now = clock::now();
                if (now - lastEmit < std::chrono::milliseconds(300)) return;  // 节流
                lastEmit = now;
            } else {
                outLines.push_back(line);
                lastWasProgress = false;
                lastEmit = clock::now();
            }
            if (opt.onLine) opt.onLine(line);
        };

        std::string pending;   // 未成行的残留字节
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pending.append(buf, n);
            size_t lineStart = 0;
            size_t i = 0;
            while (i < pending.size()) {
                if (pending[i] != '\n' && pending[i] != '\r') { ++i; continue; }
                // 消费整段换行符；这段里没有 \n 视为进度刷新（仅 \r）
                size_t j = i;
                bool hasLF = false;
                while (j < pending.size() && (pending[j] == '\n' || pending[j] == '\r')) {
                    if (pending[j] == '\n') hasLF = true;
                    ++j;
                }
                emit(pending.substr(lineStart, i - lineStart), !hasLF);
                lineStart = j;
                i = j;
            }
            pending.erase(0, lineStart);
        }
        // 无换行结尾的尾巴（进程最后一行可能不带换行）
        if (!pending.empty()) emit(pending, false);
        CloseHandle(hRead);
    });

    // 主线程：以 150ms 步进轮询 取消 / 超时 / 退出，触发即杀（读线程随后因
    // 管道写端关闭拿到 EOF 自然退出，不会挂死）。
    int exitCode = 0;
    auto deadline = clock::now() + std::chrono::milliseconds(opt.timeoutMs);
    for (;;) {
        if (opt.cancel && opt.cancel()) {
            TerminateProcess(pi.hProcess, 1);
            exitCode = -3;
            break;
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 150);
        if (w == WAIT_OBJECT_0) break;
        if (clock::now() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            exitCode = -2;
            break;
        }
    }

    if (reader.joinable()) reader.join();

    if (exitCode != -2 && exitCode != -3) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        exitCode = (int)code;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (fullOut) {
        wstring all;
        for (size_t i = 0; i < outLines.size(); ++i) {
            if (i) all += L"\n";
            all += outLines[i];
        }
        *fullOut = std::move(all);
    }
    return exitCode;
}

} // namespace mz
