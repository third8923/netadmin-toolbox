// ============================================================================
//  Winapp2.ini 规则引擎实现
//  仅统计，不删除。
// ============================================================================
#include "winapp2.h"

#include <shlobj.h>
#include <winreg.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace mz {
namespace {

constexpr size_t k_MaxIniBytes     = 64u * 1024 * 1024;
constexpr size_t k_MaxPathsPerItem = 10000;   // 每项最多保留的命中路径条数
constexpr size_t k_MaxDirsPerSpec  = 2048;    // 单条规则通配展开的目录上限

// ------------------------------------------------------------------ 基础工具

static inline void Report(const ProgressFn& progress, int pct, const wstring& msg) {
    if (progress) progress(pct, msg);
}

static wstring EnvValue(const wchar_t* name) {
    wchar_t buf[32767];
    DWORD n = GetEnvironmentVariableW(name, buf, 32767);
    return (n > 0 && n < 32767) ? wstring(buf, n) : wstring();
}

static wstring Join(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    wstring s = a;
    if (s.back() != L'\\') s.push_back(L'\\');
    return s + b;
}

static wstring Trim(const wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' ||
                     s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

static wstring Lower(const wstring& s) {
    wstring r = s;
    for (auto& c : r) c = towlower(c);
    return r;
}

static wstring Upper(const wstring& s) {
    wstring r = s;
    for (auto& c : r) c = towupper(c);
    return r;
}

static vector<wstring> Split(const wstring& s, wchar_t sep) {
    vector<wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == wstring::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

static bool HasWildcard(const wstring& s) {
    return s.find(L'*') != wstring::npos || s.find(L'?') != wstring::npos;
}

// 通配匹配（大小写不敏感；'*' 可跨 '\\'，仅用于过滤，不影响删除）
static bool WildMatch(const wstring& str, const wstring& pat) {
    const wchar_t* s = str.c_str();
    const wchar_t* p = pat.c_str();
    const wchar_t* star = nullptr;
    const wchar_t* ss = nullptr;
    while (*s) {
        if (*p == L'?' || (*p && towlower(*p) == towlower(*s))) {
            ++s; ++p;
        } else if (*p == L'*') {
            star = p++;
            ss = s;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*p == L'*') ++p;
    return *p == 0;
}

static bool IsRealDir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// ------------------------------------------------------------------ 环境变量展开

static wstring MyDocumentsPath() {
    wchar_t buf[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr,
                                   SHGFP_TYPE_CURRENT, buf)) && buf[0]) {
        return buf;
    }
    wstring up = EnvValue(L"USERPROFILE");
    return up.empty() ? wstring() : Join(up, L"Documents");
}

static wstring LookupToken(const wstring& token) {
    if (token == L"localappdata") {
        wstring v = EnvValue(L"LOCALAPPDATA");
        if (v.empty()) {
            wstring up = EnvValue(L"USERPROFILE");
            if (!up.empty()) v = Join(up, L"AppData\\Local");
        }
        return v;
    }
    if (token == L"appdata" || token == L"roamingappdata") return EnvValue(L"APPDATA");
    if (token == L"programdata" || token == L"commonappdata" || token == L"allusersprofile") {
        wstring v = EnvValue(L"PROGRAMDATA");
        if (v.empty()) v = EnvValue(L"ALLUSERSPROFILE");
        return v;
    }
    if (token == L"userprofile" || token == L"profile") return EnvValue(L"USERPROFILE");
    if (token == L"documents" || token == L"mydocuments" || token == L"personal") return MyDocumentsPath();
    if (token == L"systemroot" || token == L"windir") {
        wchar_t b[MAX_PATH] = { 0 };
        UINT n = GetWindowsDirectoryW(b, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? wstring(b, n) : EnvValue(L"WINDIR");
    }
    if (token == L"systemdrive") {
        wchar_t b[MAX_PATH] = { 0 };
        UINT n = GetWindowsDirectoryW(b, MAX_PATH);
        if (n >= 2 && b[1] == L':') return wstring(b, 2);
        return EnvValue(L"SystemDrive");
    }
    if (token == L"temp" || token == L"tmp") {
        wchar_t b[MAX_PATH + 1] = { 0 };
        DWORD n = GetTempPathW(MAX_PATH + 1, b);
        return (n > 0 && n < MAX_PATH + 1) ? wstring(b, n) : EnvValue(L"TEMP");
    }
    if (token == L"public") return EnvValue(L"PUBLIC");
    if (token == L"programfiles") return EnvValue(L"ProgramFiles");
    if (token == L"programfiles(x86)") return EnvValue(L"ProgramFiles(x86)");
    if (token == L"commonprogramfiles") return EnvValue(L"CommonProgramFiles");
    return L"";
}

// 展开 %LocalAppData% 等 Winapp2 变量，再把剩余 %VAR% 交给系统展开。
// 无法展开的变量原样保留（调用方可据此跳过）。
static wstring ExpandWa2Env(const wstring& in) {
    wstring out;
    out.reserve(in.size() + 32);
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != L'%') { out.push_back(in[i++]); continue; }
        size_t j = in.find(L'%', i + 1);
        if (j == wstring::npos) { out.append(in.substr(i)); break; }
        wstring token = Lower(in.substr(i + 1, j - i - 1));
        wstring val = LookupToken(token);
        if (!val.empty()) out += val;
        else out.append(in.substr(i, j - i + 1));   // 未知变量保持原样
        i = j + 1;
    }
    DWORD need = ExpandEnvironmentStringsW(out.c_str(), nullptr, 0);
    if (need > 1) {
        vector<wchar_t> buf(need + 2, 0);
        DWORD got = ExpandEnvironmentStringsW(out.c_str(), buf.data(), (DWORD)buf.size());
        if (got > 0 && got <= buf.size()) out.assign(buf.data());
    }
    return out;
}

// ------------------------------------------------------------------ 路径规范化 / 安全

// 归一化：'/' -> '\\'、去 . / ..、绝对化（相对路径按当前目录）、去尾反斜杠。
// 保留通配符。盘符根返回 "C:"，UNC 根返回 "\\\\server\\share"。
static wstring NormalizePath(const wstring& in) {
    wstring s = in;
    for (auto& c : s) { if (c == L'/') c = L'\\'; }
    if (s.empty()) return L"";

    bool abs = (s.size() >= 2 && s[1] == L':') ||
               (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') ||
               (s[0] == L'\\');
    if (!abs) {
        wchar_t cwd[MAX_PATH * 2] = { 0 };
        DWORD n = GetCurrentDirectoryW(MAX_PATH * 2, cwd);
        if (n == 0 || n >= MAX_PATH * 2) return L"";
        s = Join(wstring(cwd, n), s);
    }

    wstring root;
    size_t pos = 0;
    if (s.size() >= 2 && s[1] == L':') {
        root = s.substr(0, 2);
        pos = 2;
    } else if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') {
        size_t p1 = s.find(L'\\', 2);
        if (p1 == wstring::npos) return L"";
        size_t p2 = s.find(L'\\', p1 + 1);
        root = (p2 == wstring::npos) ? s : s.substr(0, p2);
        pos = root.size();
    } else if (!s.empty() && s[0] == L'\\') {
        root = L"\\";
        pos = 1;
    } else {
        return L"";
    }

    vector<wstring> parts;
    while (pos < s.size()) {
        size_t next = s.find(L'\\', pos);
        wstring seg = (next == wstring::npos) ? s.substr(pos) : s.substr(pos, next - pos);
        pos = (next == wstring::npos) ? s.size() : next + 1;
        if (seg.empty() || seg == L".") continue;
        if (seg == L"..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(seg);
    }

    wstring out = root;
    for (auto& p : parts) out = Join(out, p);
    return out;
}

// 归一化路径的组件（不含根）
static vector<wstring> Components(const wstring& norm) {
    vector<wstring> comps;
    size_t pos = 0;
    if (norm.size() >= 2 && norm[1] == L':') {
        pos = 2;
    } else if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
        size_t p1 = norm.find(L'\\', 2);
        if (p1 == wstring::npos) return comps;
        size_t p2 = norm.find(L'\\', p1 + 1);
        pos = (p2 == wstring::npos) ? norm.size() : p2;
    } else {
        return comps;
    }
    while (pos < norm.size()) {
        if (norm[pos] == L'\\') { ++pos; continue; }
        size_t next = norm.find(L'\\', pos);
        wstring seg = (next == wstring::npos) ? norm.substr(pos) : norm.substr(pos, next - pos);
        comps.push_back(seg);
        if (next == wstring::npos) break;
        pos = next + 1;
    }
    return comps;
}

static bool IsDriveRoot(const wstring& norm) {
    return norm.size() == 2 && norm[1] == L':';
}

static bool IsProtectedFirst(const wstring& firstLower) {
    static const wchar_t* kList[] = {
        L"windows", L"program files", L"program files (x86)", L"programdata",
        L"users", L"$recycle.bin", L"system volume information",
        L"recovery", L"perflogs", L"boot", L"windows.old",
        L"documents and settings", L"$windows.~bt", L"$windows.~ws",
    };
    for (auto* s : kList) if (firstLower == s) return true;
    return false;
}

// 判断“展开后的具体目录”是否属于禁止扫描的位置
static bool IsDangerousDir(const wstring& normDir) {
    wstring norm = normDir;
    while (!norm.empty() && norm.back() == L'\\' && norm.size() > 2) norm.pop_back();
    if (norm.empty() || IsDriveRoot(norm)) return true;
    vector<wstring> comps = Components(norm);
    if (comps.empty()) return true;
    wstring first = Lower(comps[0]);
    // $Recycle.Bin / System Volume Information 整树跳过
    if (first == L"$recycle.bin" || first == L"system volume information") return true;
    // 保护目录顶层本身
    if (comps.size() == 1 && IsProtectedFirst(first)) return true;
    // 用户目录根（C:\Users\xxx）
    if (comps.size() == 2 && first == L"users") return true;
    // 关键系统子目录：只拦截目录「本身」，不拦截更深层的合法子目录
    // （例如 C:\Windows\System32 本身禁止，但 System32\LogFiles、
    //   System32\spool\PRINTERS 是合理的清理目标）。
    static const wchar_t* kExact[] = {
        L"windows\\system32", L"windows\\syswow64", L"windows\\winsxs",
        L"windows\\assembly", L"windows\\fonts", L"windows\\servicing",
        L"windows\\system32\\config",
    };
    for (auto* e : kExact) {
        // 把模式拆成组件数，仅当路径组件数与模式完全一致时才算命中
        wstring pat = e;
        size_t patComps = 1;
        for (wchar_t c : pat) if (c == L'\\') ++patComps;
        if (comps.size() != patComps) continue;
        wstring joined;
        for (size_t i = 0; i < comps.size(); ++i) {
            if (i) joined += L"\\";
            joined += Lower(comps[i]);
        }
        if (joined == pat) return true;
    }
    return false;
}

// “容器目录”：其下一层的通配会横跨多个不相关应用（如 %LocalAppData%\*）
static bool IsContainerComponent(const wstring& lowerName) {
    static const wchar_t* kList[] = {
        L"users", L"appdata", L"local", L"roaming", L"locallow", L"programs",
        L"programdata", L"windows", L"public", L"packages",
        L"documents", L"desktop", L"downloads", L"pictures", L"music",
        L"videos", L"favorites", L"onedrive",
        L"$recycle.bin", L"system volume information",
    };
    for (auto* s : kList) if (lowerName == s) return true;
    return false;
}

// 判断“展开前的通配模式”是否过宽/危险：
//   C:\*  /  \\srv\share\*
//   <容器目录>\*           如 %LocalAppData%\*、%AppData%\*、%ProgramData%\*
//   C:\Users\<用户>\*      用户目录根
//   C:\Windows\* 等保护目录下第一层通配
// 注意：通配位于中间且后面还有具体子目录（如 ...\User Data\*\Cache、
//       ...\Packages\*\LocalCache）属于正常 Winapp2 写法，允许。
static bool IsDangerousPattern(const wstring& norm) {
    vector<wstring> comps = Components(norm);
    if (comps.empty()) return true;                 // 盘符根 / UNC 根

    size_t wi = comps.size();                       // 第一个通配组件下标
    for (size_t i = 0; i < comps.size(); ++i) {
        if (HasWildcard(comps[i])) { wi = i; break; }
    }
    wstring first = Lower(comps[0]);

    if (wi == comps.size()) {                       // 无通配
        if (comps.size() == 1) return IsProtectedFirst(first);
        return false;
    }
    if (wi == 0) return true;                       // C:\* 之类
    if (IsProtectedFirst(first) && wi == 1) return true;

    // 用户目录根：C:\Users\<name>\*
    if (wi == 2 && first == L"users") return true;

    // 父目录是容器目录，且通配正好吃掉了容器内容的整个层级
    if (wi == comps.size() - 1 && IsContainerComponent(Lower(comps[wi - 1]))) return true;
    return false;
}

// ------------------------------------------------------------------ FileKey 解析

struct Spec {
    wstring path;             // 已截出目录部分（未展开环境变量）
    vector<wstring> masks;    // 文件名通配（已按 ';' 拆分）
    bool recurse = false;
    bool removeSelf = false;
};

static bool ParseSpec(const wstring& raw, Spec* out) {
    *out = Spec{};
    vector<wstring> parts = Split(raw, L'|');
    wstring path = Trim(parts[0]);
    if (path.empty()) return false;

    wstring mask;
    if (parts.size() >= 2) mask = Trim(parts[1]);

    wstring flagsText;
    for (size_t i = 2; i < parts.size(); ++i) {
        flagsText += L'|';
        flagsText += parts[i];
    }
    wstring flagsUp = Upper(flagsText);
    out->recurse    = flagsUp.find(L"RECURSE") != wstring::npos;
    out->removeSelf = flagsUp.find(L"REMOVESELF") != wstring::npos;

    // 没有 mask 且路径最后一段含通配符 -> 拆成 目录 + 文件名通配
    if (mask.empty()) {
        size_t bs = path.find_last_of(L'\\');
        wstring last = (bs == wstring::npos) ? path : path.substr(bs + 1);
        if (bs != wstring::npos && HasWildcard(last)) {
            mask = last;
            path = path.substr(0, bs);
        }
    }
    if (mask.empty()) mask = L"*.*";

    for (auto& m : Split(mask, L';')) {
        wstring t = Trim(m);
        if (!t.empty()) out->masks.push_back(t);
    }
    if (out->masks.empty()) out->masks.push_back(L"*.*");
    out->path = path;
    return true;
}

// 排除项 -> 全路径通配模式（'*' 可跨分隔符，只可能少报，不会多删）
static bool BuildExcludePattern(const wstring& raw, wstring* out) {
    vector<wstring> parts = Split(raw, L'|');
    if (!parts.empty()) {
        wstring p0 = Lower(Trim(parts[0]));
        if (p0 == L"reg") return false;                    // 注册表排除本阶段不处理
        if (p0 == L"file" || p0 == L"path" || p0 == L"dir") parts.erase(parts.begin());
    }
    if (parts.empty()) return false;
    wstring path = Trim(parts[0]);
    if (path.empty()) return false;

    wstring mask = (parts.size() >= 2) ? Trim(parts[1]) : wstring();
    wstring flagsText;
    for (size_t i = 2; i < parts.size(); ++i) { flagsText += L'|'; flagsText += parts[i]; }
    bool recurse = Upper(flagsText).find(L"RECURSE") != wstring::npos;

    wstring expanded = ExpandWa2Env(path);
    if (expanded.find(L'%') != wstring::npos) return false;

    if (mask.empty()) {
        size_t bs = expanded.find_last_of(L'\\');
        wstring last = (bs == wstring::npos) ? expanded : expanded.substr(bs + 1);
        if (bs != wstring::npos && HasWildcard(last)) {
            mask = last;
            expanded = expanded.substr(0, bs);
        }
    }
    wstring norm = NormalizePath(expanded);
    if (norm.empty()) return false;

    if (mask.empty()) {
        // 整条路径指向一个已存在的文件 -> 精确排除；否则当目录
        DWORD a = GetFileAttributesW(norm.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && !HasWildcard(norm)) {
            *out = norm;
            return true;
        }
        mask = L"*.*";
    }
    if (mask == L"*.*") mask = L"*";   // Windows 语义，避免漏掉无扩展名文件
    *out = recurse ? (norm + L"\\*\\" + mask) : (norm + L"\\" + mask);
    return true;
}

// ------------------------------------------------------------------ 目录通配展开

// 把已归一化、可能含通配段的目录模式展开为具体目录列表
static void ExpandDirPatterns(const wstring& normPattern, vector<wstring>* outDirs,
                              const CancelFn& cancel) {
    outDirs->clear();
    wstring root;
    size_t pos = 0;
    if (normPattern.size() >= 2 && normPattern[1] == L':') {
        root = normPattern.substr(0, 2);
        pos = 2;
    } else if (normPattern.size() >= 2 && normPattern[0] == L'\\' && normPattern[1] == L'\\') {
        size_t p1 = normPattern.find(L'\\', 2);
        if (p1 == wstring::npos) return;
        size_t p2 = normPattern.find(L'\\', p1 + 1);
        root = (p2 == wstring::npos) ? normPattern : normPattern.substr(0, p2);
        pos = root.size();
    } else {
        return;
    }

    vector<wstring> segs;
    while (pos < normPattern.size()) {
        size_t next = normPattern.find(L'\\', pos);
        wstring seg = (next == wstring::npos) ? normPattern.substr(pos)
                                              : normPattern.substr(pos, next - pos);
        pos = (next == wstring::npos) ? normPattern.size() : next + 1;
        if (seg.empty() || seg == L".") continue;
        if (seg == L"..") { if (!segs.empty()) segs.pop_back(); continue; }
        segs.push_back(seg);
    }

    vector<wstring> cur{ root };
    for (auto& seg : segs) {
        if (cancel && cancel()) return;
        vector<wstring> nextDirs;
        if (HasWildcard(seg)) {
            for (auto& d : cur) {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(Join(d, seg).c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    nextDirs.push_back(Join(d, fd.cFileName));
                    if (nextDirs.size() >= k_MaxDirsPerSpec) break;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                if (nextDirs.size() >= k_MaxDirsPerSpec) break;
            }
        } else {
            for (auto& d : cur) {
                wstring p = Join(d, seg);
                if (IsRealDir(p)) nextDirs.push_back(p);
            }
        }
        cur.swap(nextDirs);
        if (cur.empty()) return;
        if (cur.size() > k_MaxDirsPerSpec) cur.resize(k_MaxDirsPerSpec);
    }
    *outDirs = std::move(cur);
}

// ------------------------------------------------------------------ 文件统计

static bool MatchAnyMask(const wstring& name, const vector<wstring>& masks) {
    if (masks.empty()) return true;
    for (auto& m : masks) {
        // Windows 语义：'*.*' / '*' 命中所有文件（含无扩展名文件）
        if (m == L"*" || m == L"*.*") return true;
        if (WildMatch(name, m)) return true;
        // 'foo.*' 也应命中无扩展名的 'foo'
        if (m.size() >= 2 && m.compare(m.size() - 2, 2, L".*") == 0 &&
            name.find(L'.') == wstring::npos) {
            if (WildMatch(name, m.substr(0, m.size() - 2))) return true;
        }
    }
    return false;
}

static bool IsExcluded(const wstring& full, const vector<wstring>& patterns) {
    if (patterns.empty()) return false;
    for (auto& p : patterns) if (WildMatch(full, p)) return true;
    return false;
}

static unsigned long long ScanOneDir(const wstring& dir, const Spec& spec,
                                     const vector<wstring>& exclPatterns,
                                     std::set<wstring>& seen,
                                     vector<wstring>* matched,
                                     const CancelFn& cancel, bool* cancelled) {
    unsigned long long total = 0;
    vector<wstring> stack{ dir };
    while (!stack.empty()) {
        if (cancel && cancel()) { *cancelled = true; return total; }
        wstring d = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(d, L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!spec.recurse) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                stack.push_back(Join(d, fd.cFileName));
                continue;
            }
            wstring name = fd.cFileName;
            if (!MatchAnyMask(name, spec.masks)) continue;
            wstring full = Join(d, name);
            if (IsExcluded(full, exclPatterns)) continue;
            // 全局去重：同一文件被多条规则命中只计一次，避免总量重复
            if (!seen.insert(Lower(full)).second) continue;
            unsigned long long sz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            total += sz;
            if (matched && matched->size() < k_MaxPathsPerItem) matched->push_back(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return total;
}

// ------------------------------------------------------------------ Detect 检测

static HKEY MapRegRoot(const wstring& nameLower) {
    if (nameLower == L"hkcu" || nameLower == L"hkey_current_user") return HKEY_CURRENT_USER;
    if (nameLower == L"hklm" || nameLower == L"hkey_local_machine") return HKEY_LOCAL_MACHINE;
    if (nameLower == L"hkcr" || nameLower == L"hkey_classes_root") return HKEY_CLASSES_ROOT;
    if (nameLower == L"hku"  || nameLower == L"hkey_users") return HKEY_USERS;
    if (nameLower == L"hkcc" || nameLower == L"hkey_current_config") return HKEY_CURRENT_CONFIG;
    return nullptr;
}

static bool RegValueExists(HKEY root, const wstring& sub) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(hk, nullptr, nullptr, &type, nullptr, &size);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

// 检测注册表路径（键或值）是否存在
static bool RegDetectExists(const wstring& spec) {
    wstring s = Trim(spec);
    if (s.empty()) return false;
    wstring low = Lower(s);
    if (low.rfind(L"reg|", 0) == 0) { s = Trim(s.substr(4)); low = Lower(s); }
    size_t bs = s.find(L'\\');
    wstring rootName = (bs == wstring::npos) ? low : Lower(s.substr(0, bs));
    HKEY root = MapRegRoot(rootName);
    if (!root) return false;
    if (bs == wstring::npos) return true;   // 只给了根，视为存在
    wstring sub = s.substr(bs + 1);

    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegCloseKey(hk);
        return true;
    }
    // 可能给的是值名：拆最后一段查值
    size_t p = sub.find_last_of(L'\\');
    if (p != wstring::npos) {
        wstring parent = sub.substr(0, p);
        wstring val = sub.substr(p + 1);
        if (!parent.empty() && !val.empty() && RegValueExists(root, parent)) {
            // 键存在不代表值存在，单独查
            HKEY ph = nullptr;
            if (RegOpenKeyExW(root, parent.c_str(), 0, KEY_READ, &ph) == ERROR_SUCCESS) {
                DWORD type = 0, size = 0;
                LONG r = RegQueryValueExW(ph, val.c_str(), nullptr, &type, nullptr, &size);
                RegCloseKey(ph);
                if (r == ERROR_SUCCESS) return true;
            }
        }
    } else if (!sub.empty()) {
        HKEY rh = nullptr;
        if (RegOpenKeyExW(root, nullptr, 0, KEY_READ, &rh) == ERROR_SUCCESS) {
            DWORD type = 0, size = 0;
            LONG r = RegQueryValueExW(rh, sub.c_str(), nullptr, &type, nullptr, &size);
            RegCloseKey(rh);
            if (r == ERROR_SUCCESS) return true;
        }
    }
    return false;
}

static bool FileDetectExists(const wstring& spec) {
    wstring expanded = ExpandWa2Env(Trim(spec));
    if (expanded.empty() || expanded.find(L'%') != wstring::npos) return false;
    wstring norm = NormalizePath(expanded);
    if (norm.empty()) return false;
    if (HasWildcard(norm)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(norm.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        return true;
    }
    return GetFileAttributesW(norm.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Detect / DetectFile：多条任一命中即通过（OR）。
// DetectOS 本阶段一律视为通过（工具只跑 Win7+）。
static bool DetectPasses(const Wa2Entry& e) {
    if (!e.detectOS.empty()) {
        // 简化：Windows 10/11 环境，全部通过
    }
    if (e.detect.empty() && e.detectFile.empty()) return true;
    for (auto& d : e.detect) if (RegDetectExists(d)) return true;
    for (auto& f : e.detectFile) if (FileDetectExists(f)) return true;
    return false;
}

// ------------------------------------------------------------------ INI 解析

static bool KeyLike(const wstring& keyLower, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    if (keyLower.size() < n) return false;
    if (keyLower.compare(0, n, prefix) != 0) return false;
    for (size_t i = n; i < keyLower.size(); ++i) {
        if (keyLower[i] < L'0' || keyLower[i] > L'9') return false;
    }
    return true;
}

static wstring DecodeIni(const char* data, size_t len) {
    if (len >= 2) {
        unsigned char b0 = (unsigned char)data[0], b1 = (unsigned char)data[1];
        if (b0 == 0xFF && b1 == 0xFE) {          // UTF-16 LE
            wstring s;
            s.reserve((len - 2) / 2);
            for (size_t i = 2; i + 1 < len; i += 2) {
                s.push_back((wchar_t)((unsigned char)data[i] |
                                      ((unsigned char)data[i + 1] << 8)));
            }
            return s;
        }
        if (b0 == 0xFE && b1 == 0xFF) {          // UTF-16 BE
            wstring s;
            s.reserve((len - 2) / 2);
            for (size_t i = 2; i + 1 < len; i += 2) {
                s.push_back((wchar_t)(((unsigned char)data[i] << 8) |
                                      (unsigned char)data[i + 1]));
            }
            return s;
        }
    }
    size_t off = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        off = 3;   // UTF-8 BOM
    }
    return Utf8ToW(std::string(data + off, len - off));
}

static void ParseIni(const wstring& text, Wa2RuleSet& rs) {
    Wa2Entry cur;
    bool have = false;

    auto flush = [&]() {
        if (!have) return;
        if (!cur.section.empty() && (!cur.files.empty() || !cur.regs.empty())) {
            rs.entries.push_back(cur);
        }
        cur = Wa2Entry{};
        have = false;
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        wstring line = (nl == wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (nl == wstring::npos) pos = text.size() + 1;
        else pos = nl + 1;

        line = Trim(line);
        if (line.empty()) continue;
        wchar_t c0 = line[0];
        if (c0 == L';' || c0 == L'#') continue;

        if (c0 == L'[') {
            size_t rb = line.find(L']');
            if (rb == wstring::npos) continue;
            flush();
            wstring name = Trim(line.substr(1, rb - 1));
            // 节名尾部 '*' 表示需要管理员/非默认，统计时无意义，去掉
            while (!name.empty() && name.back() == L'*') name.pop_back();
            name = Trim(name);
            cur = Wa2Entry{};
            cur.section = name;
            have = true;
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq == wstring::npos || !have) continue;
        wstring key = Lower(Trim(line.substr(0, eq)));
        wstring val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (KeyLike(key, L"filekey")) {
            if (!val.empty()) cur.files.push_back(val);
        } else if (KeyLike(key, L"regkey")) {
            if (!val.empty()) cur.regs.push_back(val);
        } else if (KeyLike(key, L"excludekey")) {
            if (!val.empty()) cur.excludes.push_back(val);
        } else if (KeyLike(key, L"detectfile")) {
            if (!val.empty()) cur.detectFile.push_back(val);
        } else if (key == L"detectos") {
            cur.detectOS = val;
        } else if (KeyLike(key, L"detect")) {
            if (!val.empty()) cur.detect.push_back(val);
        } else if (key == L"section") {
            cur.sectionShort = val;
        } else if (key == L"langsecref") {
            cur.langSecRef = val;
        } else if (key == L"default") {
            wstring v = Lower(val);
            cur.isDefault = !(v == L"false" || v == L"0" || v == L"no");
        } else if (key == L"warning") {
            cur.warning = true;
        }
        // 其余键静默忽略
    }
    flush();
}

} // namespace

// ================================================================== 公共接口

bool Wa2RuleSet::LoadFromMemory(const char* data, size_t len, wstring* err) {
    entries.clear();
    if (!data || len == 0) {
        if (err) *err = L"规则数据为空";
        return false;
    }
    if (len > k_MaxIniBytes) {
        if (err) *err = L"规则文件过大";
        return false;
    }
    wstring text = DecodeIni(data, len);
    ParseIni(text, *this);
    return true;
}

bool Wa2RuleSet::LoadFromFile(const wstring& path, wstring* err) {
    entries.clear();
    if (path.empty()) {
        if (err) *err = L"规则文件路径为空";
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法打开规则文件: " + path;
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        if (err) *err = L"规则文件为空";
        return false;
    }
    if ((unsigned long long)sz.QuadPart > k_MaxIniBytes) {
        CloseHandle(h);
        if (err) *err = L"规则文件过大";
        return false;
    }
    std::string buf;
    buf.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < buf.size()) {
        DWORD read = 0;
        DWORD want = (DWORD)std::min<size_t>(buf.size() - got, 1u << 20);
        if (!ReadFile(h, buf.data() + got, want, &read, nullptr) || read == 0) break;
        got += read;
    }
    CloseHandle(h);
    if (got == 0) {
        if (err) *err = L"读取规则文件失败: " + path;
        return false;
    }
    buf.resize(got);
    return LoadFromMemory(buf.data(), buf.size(), err);
}

wstring DefaultWinapp2Path() {
    wchar_t buf[32768] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n == 0 || n >= 32768) return L"";
    wstring p(buf, n);
    size_t pos = p.find_last_of(L'\\');
    if (pos == wstring::npos) return L"";
    wstring cand = p.substr(0, pos + 1) + L"Winapp2.ini";
    DWORD a = GetFileAttributesW(cand.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return L"";
    return cand;
}

bool LoadWinapp2FromExeDir(Wa2RuleSet& rs) {
    rs.entries.clear();
    wstring p = DefaultWinapp2Path();
    if (p.empty()) return true;   // 文件不存在：空规则集，不报错
    wstring err;
    if (!rs.LoadFromFile(p, &err)) {
        rs.entries.clear();
        return false;
    }
    return true;
}

vector<Wa2ScanItem> ScanWa2(const Wa2RuleSet& rs, const ProgressFn& progress,
                            const CancelFn& cancel) {
    vector<Wa2ScanItem> out;
    const int n = (int)rs.entries.size();
    Report(progress, 0, L"Winapp2: 共 " + std::to_wstring(n) + L" 条规则");

    std::set<wstring> seen;     // 全局去重（小写全路径）
    int done = 0;
    bool stop = false;

    for (const auto& e : rs.entries) {
        if (stop || (cancel && cancel())) break;
        int pct = (n > 0) ? (5 + done * 90 / n) : 5;
        ++done;
        Report(progress, pct, L"Winapp2: " + e.section);

        if (e.warning) continue;                 // 危险规则跳过
        if (!DetectPasses(e)) continue;          // 检测不通过跳过

        vector<wstring> exclPatterns;
        for (auto& x : e.excludes) {
            wstring pat;
            if (BuildExcludePattern(x, &pat)) exclPatterns.push_back(pat);
        }

        for (auto& raw : e.files) {
            if (stop || (cancel && cancel())) { stop = true; break; }

            Spec spec;
            if (!ParseSpec(raw, &spec)) continue;

            wstring expanded = ExpandWa2Env(spec.path);
            if (expanded.empty()) continue;
            if (expanded.find(L'%') != wstring::npos) {
                Report(progress, -1, L"[winapp2] 环境变量无法展开，跳过: " + raw);
                continue;
            }
            wstring norm = NormalizePath(expanded);
            if (norm.empty()) continue;
            if (IsDangerousPattern(norm)) {
                Report(progress, -1, L"[winapp2] 危险路径已跳过: " + raw);
                continue;
            }

            vector<wstring> dirs;
            ExpandDirPatterns(norm, &dirs, cancel);
            if (cancel && cancel()) { stop = true; break; }

            Wa2ScanItem item;
            item.section = e.section;
            item.pattern = raw;

            bool cancelled = false;
            for (auto& d : dirs) {
                if (cancel && cancel()) { cancelled = true; break; }
                if (IsDangerousDir(d)) {
                    Report(progress, -1, L"[winapp2] 危险目录已跳过: " + d);
                    continue;
                }
                item.size += ScanOneDir(d, spec, exclPatterns, seen,
                                        &item.matchedPaths, cancel, &cancelled);
                if (cancelled) break;
            }
            if (item.size > 0) out.push_back(std::move(item));
            if (cancelled) { stop = true; break; }
        }
    }

    std::sort(out.begin(), out.end(), [](const Wa2ScanItem& a, const Wa2ScanItem& b) {
        if (a.size != b.size) return a.size > b.size;
        return a.section < b.section;
    });

    Report(progress, 100, L"Winapp2 扫描完成");
    return out;
}

} // namespace mz
