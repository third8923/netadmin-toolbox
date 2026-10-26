// ============================================================================
//  清理结果模型 + 智能删除引擎 —— 实现（见 cleanresult.h 头注释）
// ============================================================================
#include "cleanresult.h"

#include <algorithm>

namespace mz {
namespace {

// 归一化路径：全路径展开 + 小写 + 去尾分隔符（危险路径判定用）
wstring NormPathForCheck(const wstring& raw) {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = GetFullPathNameW(raw.c_str(), MAX_PATH * 2, buf, nullptr);
    wstring s = (n > 0 && n < MAX_PATH * 2) ? wstring(buf, n) : raw;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    for (auto& c : s) c = towlower(c);
    return s;
}

wstring EnvStr(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? wstring(buf, n) : wstring();
}

// 长路径前缀：超过 248 字符的路径加 \\?\（UNC 加 \\?\UNC\），
// 使 FindFirstFile/DeleteFile/RemoveDirectory 突破 MAX_PATH 限制
wstring LongPath(const wstring& p) {
    if (p.size() < 240 || p.size() >= 32760) return p;
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC" + p.substr(1);
    return L"\\\\?\\" + p;
}

wstring JoinPath(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    wstring s = a;
    if (s.back() != L'\\') s.push_back(L'\\');
    return s + b;
}

// 清除文件自身的 只读/隐藏/系统 属性（L2 第一步）
void StripFileAttrs(const wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    DWORD want = a & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                       FILE_ATTRIBUTE_SYSTEM);
    if (want != a) SetFileAttributesW(path.c_str(), want);
}

// 清除父目录的 只读/隐藏/系统 属性（L2：目录只读会连累其下文件删除）
void StripParentDirAttrs(const wstring& path) {
    size_t slash = path.find_last_of(L'\\');
    if (slash == wstring::npos || slash == 0) return;
    StripFileAttrs(path.substr(0, slash));
}

// 可中断的退避等待（50ms 步进，取消立即返回 false）
bool BackoffSleep(const CancelFn& cancel, int totalMs) {
    for (int waited = 0; waited < totalMs; waited += 50) {
        if (cancel && cancel()) return false;
        Sleep(50);
    }
    return !(cancel && cancel());
}

// 递归清空目录内容（内部实现）：不更新 report.freedBytes（由顶层按差值统一计），
// 逐文件成败写入 report。支持 \\?\ 长路径。
unsigned long long CleanDirInner(const wstring& dir, const CancelFn& cancel,
                                 CleanReport& report, int* rebootBudget) {
    unsigned long long freed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LongPath(dir) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (cancel && cancel()) break;
        wstring full = JoinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                RemoveDirectoryW(LongPath(full).c_str());
            } else {
                freed += CleanDirInner(full, cancel, report, rebootBudget);
                RemoveDirectoryW(LongPath(full).c_str());
            }
        } else {
            unsigned long long sz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (DeleteFileSmart(full, cancel, report, rebootBudget))
                freed += sz;   // 按扫描到的实际大小累计（不冒充）
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return freed;
}

} // namespace

// ---------------------------------------------------------------- ErrorTextZh

wstring ErrorTextZh(DWORD code) {
    switch (code) {
    case 0:      return L"成功";
    case 2:      return L"文件不存在";
    case 3:      return L"路径不存在";
    case 5:      return L"需要管理员权限";
    case 32:     return L"被其他程序占用";
    case 33:     return L"被其他程序锁定";
    case 80:     return L"文件已存在";
    case 87:     return L"参数无效";
    case 110:    return L"文件已存在";
    case 206:    return L"路径过长";
    case 267:    return L"目标不是目录";
    case 1392:   return L"文件或目录已损坏";
    case 1223:   return L"操作被用户取消";
    // ---- 服务控制常见错误码（T03 服务管理共用）----
    case 1051:   return L"必须先停止依赖此服务的其他服务";
    case 1053:   return L"服务未及时响应启动或控制请求";
    case 1056:   return L"服务已在运行";
    case 1058:   return L"服务已被禁用";
    case 1060:   return L"指定的服务不存在";
    case 1061:   return L"服务此时不接受该控制操作";
    case 1062:   return L"服务尚未启动";
    case 1072:   return L"服务已标记为待删除";
    case 0x80070005: return L"需要管理员权限";
    case 0x80070020: return L"被其他程序占用";
    default:     return L"错误码 " + std::to_wstring(code);
    }
}

// ---------------------------------------------------------------- CleanReport

wstring CleanReport::FailSummary() const {
    wstring out;
    for (auto& kv : failCodes) {
        if (!out.empty()) out += L"、";
        out += ErrorTextZh(kv.first) + L" ×" + std::to_wstring(kv.second);
    }
    return out;
}

// ------------------------------------------------------------ DeleteFileSmart

bool DeleteFileSmart(const wstring& path, const CancelFn& cancel,
                     CleanReport& report, int* rebootBudget) {
    if (cancel && cancel()) return false;

    // L1：常规删除（超长路径自动加 \\?\ 前缀）
    if (DeleteFileW(LongPath(path).c_str())) { ++report.fileOk; return true; }
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        // 目标已不存在：视为成功（幂等）
        ++report.fileOk;
        return true;
    }

    // L2：权限不足 → 清 文件 + 父目录 的 只读/隐藏/系统 属性后重试
    if (err == ERROR_ACCESS_DENIED) {
        StripFileAttrs(path);
        StripParentDirAttrs(path);
        if (DeleteFileW(LongPath(path).c_str())) { ++report.fileOk; return true; }
        err = GetLastError();
    }

    // L3：被占用/锁定 → 退避重试 ×3（100/300/600ms；Defender 实时扫描瞬时锁）
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
        static const int delays[] = { 100, 300, 600 };
        for (int d : delays) {
            if (!BackoffSleep(cancel, d)) return false;   // 用户取消，不打失败标
            if (DeleteFileW(LongPath(path).c_str())) { ++report.fileOk; return true; }
            err = GetLastError();
        }
    }

    // L4：占位删除（重启后自动消失）；受预算限制防滥用
    if (rebootBudget && *rebootBudget > 0 &&
        MoveFileExW(LongPath(path).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        --*rebootBudget;
        ++report.rebootPending;
        return true;
    }

    report.AddFail(path, err);
    return false;
}

// --------------------------------------------------- CleanDirectoryContentsEx

unsigned long long CleanDirectoryContentsEx(const wstring& dir, const CancelFn& cancel,
                                            CleanReport& report, int* rebootBudget) {
    if (!IsDangerousCleanRoot(dir)) {
        int budget = rebootBudget ? *rebootBudget : 50;
        unsigned long long freed = CleanDirInner(dir, cancel, report, &budget);
        if (rebootBudget) *rebootBudget = budget;
        report.freedBytes += freed;
        return freed;
    }
    // 顶层危险路径：设计性拒绝（防御纵深），由上层标记「仅统计」
    return 0;
}

// -------------------------------------------------------- IsDangerousCleanRoot

bool IsDangerousCleanRoot(const wstring& dir) {
    wstring p = NormPathForCheck(dir);
    if (p.size() <= 3) return true;                  // 盘根（C:\ 等）
    if (p.size() < 2 || p[1] != L':') return true;   // 非本地绝对路径，保守拒绝

    wstring windir = EnvStr(L"WINDIR");    if (windir.empty()) windir = L"C:\\Windows";
    wstring pf = EnvStr(L"ProgramFiles");
    if (pf.empty()) pf = L"C:\\Program Files";
    wstring pf86 = EnvStr(L"ProgramFiles(x86)");
    if (pf86.empty()) pf86 = L"C:\\Program Files (x86)";
    wstring pd = EnvStr(L"ProgramData");   if (pd.empty()) pd = L"C:\\ProgramData";
    // C:\Users 根：借 PUBLIC（C:\Users\Public）取父目录，不硬编码盘符
    wstring usersRoot = EnvStr(L"PUBLIC");
    if (usersRoot.size() > 3) {
        size_t slash = usersRoot.find_last_of(L'\\');
        usersRoot = (slash != wstring::npos && slash > 2) ? usersRoot.substr(0, slash)
                                                          : L"C:\\Users";
    } else {
        usersRoot = L"C:\\Users";
    }

    wstring nw = NormPathForCheck(windir);
    wstring npf = NormPathForCheck(pf);
    wstring npf86 = NormPathForCheck(pf86);
    wstring npd = NormPathForCheck(pd);
    wstring nu = NormPathForCheck(usersRoot);

    // ① Windows 本体与关键子目录（自身/整树不允许清空）
    static const wchar_t* winCritical[] = {
        L"", L"system32", L"syswow64", L"sysnative", L"driverstore",
        L"config", L"registration", L"winsxs", L"boot", L"fonts", L"installer",
    };
    for (auto sub : winCritical) {
        wstring full = nw;
        if (*sub) full = nw + L"\\" + sub;
        if (p == full) return true;
        if (p.size() > full.size() && p.compare(0, full.size(), full) == 0 &&
            p[full.size()] == L'\\')
            return true;   // 关键目录的子树也拒绝（DriverStore 等）
    }

    // ② Program Files 整树：无任何合法清理目标
    for (wstring root : { npf, npf86 }) {
        if (p == root) return true;
        if (p.size() > root.size() && p.compare(0, root.size(), root) == 0 &&
            p[root.size()] == L'\\')
            return true;
    }

    // ③ ProgramData 本体与第一层子目录拒绝；第二层起放行（Defender Scans 等）
    if (p == npd) return true;
    if (p.size() > npd.size() && p.compare(0, npd.size(), npd) == 0 &&
        p[npd.size()] == L'\\') {
        if (p.find(L'\\', npd.size() + 1) == wstring::npos) return true;   // 第一层
    }

    // ④ 用户根目录（C:\Users\<user> 本身）拒绝；AppData 等深层放行
    if (p == nu) return true;
    if (p.size() > nu.size() && p.compare(0, nu.size(), nu) == 0 &&
        p[nu.size()] == L'\\') {
        if (p.find(L'\\', nu.size() + 1) == wstring::npos) return true;    // <user> 根
    }

    // ⑤ 任意盘上的系统特殊目录
    for (auto tail : { L"$recycle.bin", L"system volume information" }) {
        size_t tl = wcslen(tail);
        if (p == tail) return true;
        if (p.size() > tl && p.compare(p.size() - tl, tl, tail) == 0 &&
            p[p.size() - tl - 1] == L'\\')
            return true;
    }
    return false;
}

} // namespace mz
