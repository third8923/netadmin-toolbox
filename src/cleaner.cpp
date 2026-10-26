#include "cleaner.h"
#include "cleanresult.h"
#include <shlobj.h>
#include <winreg.h>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>

namespace mz {

// ------------------------------------------------------------------ 工具

static wstring GetEnv(const wchar_t* name) {
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

static bool IsDir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static bool IsAnyDir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool IsLink(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static vector<wstring> ListDir(const wstring& dir) {
    vector<wstring> out;
    WIN32_FIND_DATAW fd;
    wstring pat = Join(dir, L"*");
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

unsigned long long DirSize(const wstring& dir) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    wstring pat = Join(dir, L"*");
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wstring full = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                total += DirSize(full);
        } else {
            total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// 带时间预算的目录大小统计：超过 deadline 立即停止递归，返回已统计的大小。
// timedOut 置 true 表示结果只是部分统计（用于 Windows.old / WinSxS 这类超大目录）。
static unsigned long long DirSizeCapped(const wstring& dir, unsigned long long deadline,
                                        bool* timedOut) {
    if (timedOut && *timedOut) return 0;
    if (GetTickCount64() > deadline) { if (timedOut) *timedOut = true; return 0; }
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (GetTickCount64() > deadline) { if (timedOut) *timedOut = true; break; }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wstring full = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                total += DirSizeCapped(full, deadline, timedOut);
        } else {
            total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// 预算 budgetMs 毫秒；超时则返回已统计部分并置 *partial = true
static unsigned long long DirSizeWithBudget(const wstring& dir, unsigned long long budgetMs,
                                            bool* partial) {
    bool timedOut = false;
    unsigned long long sz = DirSizeCapped(dir, GetTickCount64() + budgetMs, &timedOut);
    if (partial) *partial = timedOut;
    return sz;
}

static unsigned long long FileSizeOf(const wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

// 通过句柄取文件大小。pagefile.sys 等受保护的系统文件：
// GetFileAttributesExW 可能失败（ERROR_SHARING_VIOLATION），
// 此时改用 FindFirstFileW 走目录枚举取大小（对该文件有效）。
// 全部失败时返回 0（调用方静默跳过，不报错）。
static unsigned long long FileSizeByHandle(const wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        unsigned long long v = GetFileSizeEx(h, &sz) ? (unsigned long long)sz.QuadPart : 0;
        CloseHandle(h);
        if (v > 0) return v;
    }
    unsigned long long v = FileSizeOf(p);   // 兜底 1：属性查询
    if (v > 0) return v;

    WIN32_FIND_DATAW fd;                    // 兜底 2：目录枚举
    HANDLE fh = FindFirstFileW(p.c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;
    FindClose(fh);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
}

// 统计目录下指定扩展名的文件总大小（不递归、不统计子目录）
static unsigned long long SumFilesByExt(const wstring& dir,
                                        std::initializer_list<const wchar_t*> exts) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot) continue;
        for (auto e : exts) {
            if (_wcsicmp(dot, e) == 0) {
                total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// 本机所有固定磁盘根目录（如 "C:\\"），用 GetLogicalDrives 枚举
static vector<wstring> FixedDriveRoots() {
    vector<wstring> roots;
    DWORD mask = GetLogicalDrives();
    for (wchar_t d = L'A'; d <= L'Z'; ++d) {
        if (!(mask & (1u << (d - L'A')))) continue;
        wstring root;
        root.push_back(d);
        root += L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) roots.push_back(root);
    }
    return roots;
}

// 回收站真实大小：SHQueryRecycleBinW 结果与目录大小取较大值；查询失败则退回 DirSize
static unsigned long long RecycleBinSize(const wstring& root, const wstring& recDir) {
    unsigned long long sz = DirSize(recDir);
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHQueryRecycleBinW(root.c_str(), &info)) && info.i64Size > 0) {
        unsigned long long q = (unsigned long long)info.i64Size;
        if (q > sz) sz = q;
    }
    return sz;
}

// 递归遍历（回调返回 false 停止）
// visitFile(fullPath, size, name)
static void WalkDir(const wstring& root, const CancelFn& cancel,
                    const std::function<bool(const wstring&, unsigned long long, const wstring&)>& visitFile) {
    struct Frame { wstring path; };
    vector<wstring> stack{ root };
    while (!stack.empty()) {
        if (cancel && cancel()) return;
        wstring dir = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wstring name = fd.cFileName;
            wstring full = Join(dir, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (name[0] == L'$') continue;
                if (_wcsicmp(name.c_str(), L"System Volume Information") == 0) continue;
                stack.push_back(full);
            } else {
                unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (!visitFile(full, sz, name)) return;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static bool AddItem(ScanOutcome& out, const wstring& name, const wstring& path,
                    unsigned long long size, const wstring& cat, bool isFile = false,
                    const wstring& note = wstring(), const wstring& risk = wstring()) {
    if (size == 0) return false;
    ScanItem it;
    it.name = name;
    it.path = path;
    it.size = size;
    it.category = cat;
    it.isFile = isFile;
    it.note = note;
    it.risk = risk;
    out.items.push_back(it);
    out.totalSize += size;
    return true;
}

// 给刚添加的项打「仅统计、禁止直接删除」标记（AddItem 返回 true 时调用）
static void MarkReportOnly(ScanOutcome& out, const wstring& hint, bool partial = false) {
    if (out.items.empty()) return;
    ScanItem& it = out.items.back();
    it.reportOnly = true;
    it.hint = hint;
    it.partial = partial;
}

// ------------------------------------------------------------------ 各扫描实现

// 前置声明：从注册表读取用户 Shell 文件夹真实路径（定义见文件后部）
static wstring ShellFolderPath(const wchar_t* regName);
// 前置声明：路径归一化（定义见文件后部，迁移引擎一节）
static wstring NormPath(const wstring& p);

// 多用户支持：枚举参与清理扫描的用户 AppData\Local 目录。
//   管理员权限下：遍历 C:\Users\*（跳过 Default / Default User / Public /
//   All Users 及联接残留），返回每个真实用户的 Local 目录——解决 v2.7
//   「管理员运行却只清了当前用户」的覆盖缺口；
//   非管理员：仅当前用户（其他用户目录多无读权限，扫描结果反而残缺）。
// *note 返回覆盖范围说明，由上层写入 ScanOutcome::note。
struct UserLocalDir { wstring label; wstring local; };

static vector<UserLocalDir> EnumUserLocalDirs(wstring* note) {
    vector<UserLocalDir> out;
    wstring cur = GetEnv(L"USERPROFILE");
    if (!IsElevated()) {
        out.push_back({ L"当前用户", Join(cur, L"AppData\\Local") });
        if (note) *note = L"仅扫描当前用户（提权运行后可覆盖所有用户）";
        return out;
    }
    wstring usersRoot = L"C:\\Users";
    wstring publicDir = GetEnv(L"PUBLIC");
    if (publicDir.size() > 3) {
        size_t slash = publicDir.find_last_of(L'\\');
        if (slash != wstring::npos && slash > 2) usersRoot = publicDir.substr(0, slash);
    }
    static const wchar_t* skip[] = {
        L"Default", L"Default User", L"Public", L"All Users",
    };
    for (auto& u : ListDir(usersRoot)) {
        bool skipped = false;
        for (auto s : skip) {
            if (_wcsicmp(u.c_str(), s) == 0) { skipped = true; break; }
        }
        if (skipped) continue;
        wstring local = Join(Join(usersRoot, u), L"AppData\\Local");
        if (!IsAnyDir(local)) continue;
        out.push_back({ u, local });
    }
    if (out.empty()) out.push_back({ L"当前用户", Join(cur, L"AppData\\Local") });
    if (note) *note = L"管理员模式：已覆盖 " + std::to_wstring(out.size()) + L" 个用户配置";
    return out;
}

// 父子目录去重剪枝：同一扫描结果中若同时存在可清理父目录 A 与其子孙目录 A\B，
// 只保留父项 A——清空 A 已覆盖子孙；两者并存既重复统计又重复清理。
// 典型重叠：Windows\Logs ⊃ Windows\Logs\CBS、Windows\Logs\DISM、Windows\Logs\SIH。
// 仅处理目录项（isFile=false）且双方均可清理（非 reportOnly）。
// 实现采用「标记后统一删除」：不依赖条目先后顺序（CBS 在 targets 表中先于父项 Logs）。
// 返回剪掉的条数；ScanOutcome::totalSize 同步回退。
static int PruneNestedDirs(ScanOutcome* out) {
    vector<ScanItem>& items = out->items;
    vector<bool> remove(items.size(), false);
    int removed = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (remove[i] || items[i].reportOnly || items[i].isFile) continue;
        wstring pa = NormPath(items[i].path);
        if (pa.empty()) continue;
        for (size_t j = 0; j < items.size(); ++j) {
            if (j == i || remove[j]) continue;
            if (items[j].reportOnly || items[j].isFile) continue;
            wstring pb = NormPath(items[j].path);
            bool samePath = (pb == pa);
            bool nested = pb.size() > pa.size() &&
                          pb.compare(0, pa.size(), pa) == 0 && pb[pa.size()] == L'\\';
            if (samePath || nested) { remove[j] = true; ++removed; }
        }
    }
    if (removed > 0) {
        vector<ScanItem> kept;
        kept.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            if (remove[i]) {
                out->totalSize = (out->totalSize > items[i].size)
                                     ? out->totalSize - items[i].size : 0;
            } else {
                kept.push_back(items[i]);
            }
        }
        items.swap(kept);
    }
    return removed;
}

static void ScanBasic(ScanOutcome& out, const ProgressFn& progress) {
    wstring userProfile = GetEnv(L"USERPROFILE");
    wstring windir = GetEnv(L"WINDIR");
    if (windir.empty()) windir = L"C:\\Windows";
    wstring programData = GetEnv(L"ProgramData");
    if (programData.empty()) programData = L"C:\\ProgramData";
    wstring local = Join(userProfile, L"AppData\\Local");
    wstring roaming = Join(userProfile, L"AppData\\Roaming");

    // 多用户覆盖范围（管理员下枚举所有真实用户；说明写入 out.note）
    wstring userNote;
    vector<UserLocalDir> userLocals = EnumUserLocalDirs(&userNote);
    if (!userNote.empty()) out.note = userNote;

    progress(3, L"扫描临时文件...");
    wstring temp = GetEnv(L"TEMP");
    if (IsAnyDir(temp)) AddItem(out, L"临时文件(Temp)", temp, DirSize(temp), L"临时文件");

    // 管理员下补扫其他用户的 Temp（当前用户的 TEMP 已由上一条覆盖）
    progress(5, L"扫描其他用户临时文件...");
    for (auto& ud : userLocals) {
        if (_wcsicmp(ud.label.c_str(), L"当前用户") == 0) continue;
        wstring p = Join(ud.local, L"Temp");
        if (IsAnyDir(p))
            AddItem(out, L"临时文件(Temp@" + ud.label + L")", p, DirSize(p), L"临时文件");
    }

    progress(8, L"扫描系统临时文件...");
    wstring sysTemp = Join(windir, L"Temp");
    if (IsAnyDir(sysTemp)) AddItem(out, L"系统临时文件", sysTemp, DirSize(sysTemp), L"临时文件");

    progress(12, L"扫描预读取文件...");
    wstring prefetch = Join(windir, L"Prefetch");
    if (IsAnyDir(prefetch)) AddItem(out, L"预读取文件(Prefetch)", prefetch, DirSize(prefetch), L"系统缓存");

    progress(16, L"扫描最近文档记录...");
    wstring recent = Join(userProfile, L"AppData\\Roaming\\Microsoft\\Windows\\Recent");
    if (IsAnyDir(recent)) AddItem(out, L"最近文档记录", recent, DirSize(recent), L"系统记录");

    progress(20, L"扫描回收站...");
    for (auto& root : FixedDriveRoots()) {
        wstring rec = root + L"$Recycle.Bin";
        if (!IsAnyDir(rec)) continue;
        wstring name = L"回收站(";
        name.push_back((wchar_t)towupper(root[0]));
        name += L")";
        // v2.8：回收站项打专用标记，清理时走 SHEmptyRecycleBinW（系统回收站语义，
        // 保留 $Recycle.Bin 元数据），不再暴力删除目录内容破坏「还原」功能
        if (AddItem(out, name, rec, RecycleBinSize(root, rec), L"回收站")) {
            out.items.back().useEmptyRecycleBin = true;
            out.items.back().note = L"走系统回收站语义清空，不影响回收站元数据";
        }
    }

    progress(28, L"扫描浏览器缓存...");
    // Chromium 系浏览器：按「用户 → 数据根 → Profile → 缓存子目录」四维枚举，
    // 覆盖 Default / Profile N 多配置与全部缓存类型（v2.7 只扫了 Default\Cache）。
    struct BrowserDef { const wchar_t* name; const wchar_t* userDataRel; };
    static const BrowserDef chromiumBrowsers[] = {
        { L"Chrome",  L"Google\\Chrome\\User Data" },
        { L"Edge",    L"Microsoft\\Edge\\User Data" },
        { L"Brave",   L"BraveSoftware\\Brave-Browser\\User Data" },
        { L"Vivaldi", L"Vivaldi\\User Data" },
    };
    static const wchar_t* profileCacheSubs[] = {
        L"Cache", L"Code Cache", L"GPUCache", L"Service Worker\\CacheStorage",
        L"GrShaderCache", L"Media Cache", L"DawnGraphiteCache", L"DawnWebGPUCache",
    };
    for (auto& ud : userLocals) {
        for (auto& b : chromiumBrowsers) {
            wstring userDataRoot = Join(ud.local, b.userDataRel);
            if (!IsAnyDir(userDataRoot)) continue;
            for (auto& prof : ListDir(userDataRoot)) {
                // 只认真实用户配置：Default / Profile N（跳过 System Profile 等系统项）
                bool isProfile = _wcsicmp(prof.c_str(), L"Default") == 0 ||
                                 prof.rfind(L"Profile ", 0) == 0;
                if (!isProfile) continue;
                wstring profPath = Join(userDataRoot, prof);
                if (!IsDir(profPath)) continue;
                for (auto* sub : profileCacheSubs) {
                    wstring p = Join(profPath, sub);
                    if (!IsAnyDir(p)) continue;
                    wstring nm = wstring(b.name) + L"缓存 " + sub +
                                 L"(" + prof + L"@" + ud.label + L")";
                    AddItem(out, nm, p, DirSize(p), L"浏览器缓存");
                }
            }
        }
    }
    // 非 Chromium 固定路径浏览器（保持原有路径）
    struct { const wchar_t* name; const wchar_t* rel; } legacyBrowsers[] = {
        { L"Opera缓存",      L"Opera Software\\Opera Stable\\Cache" },
        { L"WebView2缓存",   L"Microsoft\\EdgeWebView\\Default\\Cache" },
        { L"火狐缓存(本地)", L"Mozilla\\Firefox\\Profiles" },
    };
    for (auto& b : legacyBrowsers) {
        wstring p = Join(local, b.rel);
        if (IsAnyDir(p)) AddItem(out, b.name, p, DirSize(p), L"浏览器缓存");
    }

    progress(34, L"扫描 Windows Defender 缓存...");
    wstring defScans = Join(programData, L"Microsoft\\Windows Defender\\Scans");
    if (IsAnyDir(defScans))
        AddItem(out, L"Defender 扫描缓存", defScans, DirSize(defScans), L"系统缓存");
    wstring defDefs = Join(programData, L"Microsoft\\Windows Defender\\Definition Updates");
    if (IsAnyDir(defDefs))
        AddItem(out, L"Defender 定义更新缓存", defDefs, DirSize(defDefs), L"系统缓存");

    progress(40, L"扫描应用商店与通知缓存...");
    wstring storeCache = Join(local,
        L"Packages\\Microsoft.WindowsStore_8wekyb3d8bbwe\\LocalCache");
    if (IsAnyDir(storeCache))
        AddItem(out, L"Windows Store 缓存", storeCache, DirSize(storeCache), L"应用程序");
    wstring notifyCache = Join(local, L"Microsoft\\Windows\\Notifications");
    if (IsAnyDir(notifyCache))
        AddItem(out, L"Windows 通知缓存", notifyCache, DirSize(notifyCache), L"系统缓存");

    progress(45, L"扫描搜索索引临时文件...");
    wstring searchTemp = Join(programData, L"Microsoft\\Search\\Data\\Temp");
    if (IsAnyDir(searchTemp))
        AddItem(out, L"搜索索引临时文件", searchTemp, DirSize(searchTemp), L"系统缓存");

    progress(50, L"扫描 Windows Installer 缓存...");
    wstring patchCache = Join(windir, L"Installer\\$PatchCache$");
    if (IsAnyDir(patchCache))
        AddItem(out, L"安装程序缓存(PatchCache)", patchCache, DirSize(patchCache), L"系统缓存");
    wstring installerDir = Join(windir, L"Installer");
    if (IsAnyDir(installerDir)) {
        unsigned long long msiSize = SumFilesByExt(installerDir, { L".msi", L".msp" });
        if (AddItem(out, L"Windows Installer 安装包", installerDir, msiSize, L"系统缓存", true,
                    L"仅统计 .msi/.msp 文件，不删除 Installer 目录"))
            MarkReportOnly(out, L"删除会导致程序无法修复/卸载，请通过「设置→应用」卸载程序");
    }

    progress(56, L"扫描媒体与打印机缓存...");
    wstring mediaCache = Join(local, L"Microsoft\\Media Player\\Cache");
    if (IsAnyDir(mediaCache))
        AddItem(out, L"媒体播放器缓存", mediaCache, DirSize(mediaCache), L"应用程序");
    wstring printers = Join(windir, L"System32\\spool\\PRINTERS");
    if (IsAnyDir(printers))
        AddItem(out, L"打印机临时文件", printers, DirSize(printers), L"系统缓存");
    wstring deviceTemp = Join(windir, L"SystemTemp");
    if (IsAnyDir(deviceTemp))
        AddItem(out, L"设备临时文件", deviceTemp, DirSize(deviceTemp), L"系统缓存");

    progress(62, L"扫描崩溃转储与错误报告...");
    wstring crashDumps = Join(local, L"CrashDumps");
    if (IsAnyDir(crashDumps))
        AddItem(out, L"应用程序崩溃转储", crashDumps, DirSize(crashDumps), L"应用程序");
    wstring werDir = Join(local, L"Microsoft\\Windows\\WER");
    wstring werArchive = Join(werDir, L"ReportArchive");
    if (IsAnyDir(werArchive))
        AddItem(out, L"错误报告存档", werArchive, DirSize(werArchive), L"系统日志");
    wstring werQueue = Join(werDir, L"ReportQueue");
    if (IsAnyDir(werQueue))
        AddItem(out, L"错误报告队列", werQueue, DirSize(werQueue), L"系统日志");

    progress(68, L"扫描系统错误报告队列...");
    wstring sysWer = Join(windir,
        L"System32\\config\\systemprofile\\AppData\\Local\\Microsoft\\Windows\\WER");
    if (IsAnyDir(sysWer))
        AddItem(out, L"系统错误报告队列", sysWer, DirSize(sysWer), L"系统日志");

    progress(72, L"扫描 OneDrive 日志缓存...");
    wstring odSetupLogs = Join(local, L"Microsoft\\OneDrive\\setup\\logs");
    if (IsAnyDir(odSetupLogs))
        AddItem(out, L"OneDrive 安装日志", odSetupLogs, DirSize(odSetupLogs), L"应用程序");
    wstring odLogs = Join(local, L"Microsoft\\OneDrive\\logs");
    if (IsAnyDir(odLogs))
        AddItem(out, L"OneDrive 日志缓存", odLogs, DirSize(odLogs), L"应用程序");

    progress(78, L"扫描火狐配置缓存...");
    wstring ffRoaming = Join(roaming, L"Mozilla\\Firefox\\Profiles");
    if (IsAnyDir(ffRoaming))
        AddItem(out, L"火狐配置缓存", ffRoaming, DirSize(ffRoaming), L"浏览器缓存");

    progress(86, L"扫描下载文件夹...");
    wstring downloads = ShellFolderPath(L"{374DE290-123F-4565-9164-39C4925E467B}");
    if (!downloads.empty() && IsAnyDir(downloads) &&
        AddItem(out, L"下载文件夹", downloads, DirSize(downloads), L"用户目录", false,
                L"用户下载内容，清理前请确认", L"中"))
        MarkReportOnly(out, L"用户下载内容，请手动检查后再删除，本程序不会自动清理");

    progress(100, L"基础清理扫描完成");
}

static void ScanSystem(ScanOutcome& out, const ProgressFn& progress) {
    wstring windir = GetEnv(L"WINDIR");
    if (windir.empty()) windir = L"C:\\Windows";
    wstring userProfile = GetEnv(L"USERPROFILE");

    struct Target { const wchar_t* name; wstring path; const wchar_t* cat; bool file;
                    const wchar_t* roHint; } targets[] = {
        { L"Windows更新缓存",  Join(windir, L"SoftwareDistribution\\Download"), L"系统更新", false, nullptr },
        { L"传递优化文件",     Join(windir, L"SoftwareDistribution\\DeliveryOptimization"), L"系统更新", false, nullptr },
        { L"CBS系统日志",      Join(windir, L"Logs\\CBS"), L"系统日志", false, nullptr },
        { L"Windows日志目录",  Join(windir, L"Logs"), L"系统日志", false, nullptr },
        { L"WinSxS清单缓存",   Join(windir, L"WinSxS\\ManifestCache"), L"系统缓存", false, nullptr },
        // 关键缺陷修复（v2.7 clean.cpp:401）：FileRepository 是驱动仓库，原版被标为
        // 可清理，直接清空会导致已装驱动文件缺失、设备异常。改为「仅统计」+ 指引。
        { L"驱动备份仓库",     Join(windir, L"System32\\DriverStore\\FileRepository"), L"系统备份", false,
          L"驱动仓库，请用设备管理器或 pnputil 清理旧驱动" },
        { L"字体缓存",         Join(userProfile, L"AppData\\Local\\Microsoft\\Windows\\FontCache"), L"系统缓存", false, nullptr },
        { L"内存转储文件",     Join(windir, L"MEMORY.DMP"), L"系统文件", true, nullptr },
        { L"系统错误报告",     Join(userProfile, L"AppData\\Local\\Microsoft\\Windows\\WER"), L"系统日志", false, nullptr },
        { L"缩略图缓存",       Join(userProfile, L"AppData\\Local\\Microsoft\\Windows\\Explorer"), L"系统缓存", false, nullptr },
        // ---- v2.8 新增扫描项 ----
        { L"内核实时报告",     Join(windir, L"LiveKernelReports"), L"系统日志", false, nullptr },
        { L"系统日志-Scm",     Join(windir, L"System32\\LogFiles\\Scm"), L"系统日志", false, nullptr },
        { L"系统日志-SMSS",    Join(windir, L"System32\\LogFiles\\SMSS"), L"系统日志", false, nullptr },
        { L"系统日志-WMI",     Join(windir, L"System32\\LogFiles\\WMI"), L"系统日志", false, nullptr },
        { L"SIH 日志",         Join(windir, L"Logs\\SIH"), L"系统日志", false, nullptr },
        { L"小型内存转储",     Join(windir, L"Minidump"), L"系统文件", false, nullptr },
        { L"NVIDIA 驱动缓存",  L"C:\\NVIDIA\\CacheDriver", L"系统缓存", false, nullptr },
    };

    int i = 0, n = (int)(sizeof(targets) / sizeof(targets[0]));
    for (auto& t : targets) {
        progress(1 + i * 38 / n, t.name);
        if (t.file) {
            if (!IsAnyDir(t.path)) AddItem(out, t.name, t.path, FileSizeOf(t.path), t.cat, true);
        } else {
            if (IsAnyDir(t.path) &&
                AddItem(out, t.name, t.path, DirSize(t.path), t.cat)) {
                if (t.roHint) MarkReportOnly(out, t.roHint);
            }
        }
        ++i;
    }

    progress(41, L"扫描系统安装日志(Panther)...");
    wstring panther = Join(windir, L"Panther");
    if (IsAnyDir(panther) &&
        AddItem(out, L"系统安装日志(Panther)", panther, DirSize(panther), L"系统日志"))
        MarkReportOnly(out, L"Windows 安装/升级记录，可用于排查升级故障，请勿直接删除");

    progress(42, L"扫描服务包备份...");
    wstring spFiles = Join(windir, L"ServicePackFiles");
    if (IsAnyDir(spFiles))
        AddItem(out, L"服务包备份", spFiles, DirSize(spFiles), L"系统备份");

    progress(46, L"扫描磁盘清理日志...");
    wstring cbsLog = Join(windir, L"System32\\LogFiles\\CBS");
    if (IsAnyDir(cbsLog))
        AddItem(out, L"CBS 清理日志", cbsLog, DirSize(cbsLog), L"系统日志");
    wstring dismLog = Join(windir, L"Logs\\DISM");
    if (IsAnyDir(dismLog))
        AddItem(out, L"DISM 日志", dismLog, DirSize(dismLog), L"系统日志",
                false, L"位于 Windows\\Logs 内，与 Windows日志目录 统计有重叠");

    progress(52, L"扫描旧 Windows 文件...");
    for (auto& root : FixedDriveRoots()) {
        wstring old = Join(root, L"Windows.old");
        if (!IsAnyDir(old)) continue;
        bool partial = false;
        unsigned long long sz = DirSizeWithBudget(old, 25000, &partial);
        wstring nm = L"旧 Windows 文件(";
        nm.push_back((wchar_t)towupper(root[0]));
        nm += L")";
        if (AddItem(out, nm, old, sz, L"系统升级", false,
                    L"删除后无法回滚到旧版本 Windows", L"中"))
            MarkReportOnly(out, L"请用「设置→系统→存储→临时文件」或 cleanmgr 删除", partial);
    }

    progress(62, L"扫描系统升级残留...");
    static const wchar_t* upgradeResidues[] = { L"$WINDOWS.~BT", L"$WINDOWS.~WS", L"$GetCurrent" };
    for (auto& root : FixedDriveRoots()) {
        for (auto residue : upgradeResidues) {
            wstring p = Join(root, residue);
            if (!IsAnyDir(p)) continue;
            bool partial = false;
            unsigned long long sz = DirSizeWithBudget(p, 25000, &partial);
            wstring nm = residue;
            nm += L"(";
            nm.push_back((wchar_t)towupper(root[0]));
            nm += L")";
            if (AddItem(out, nm, p, sz, L"系统升级"))
                MarkReportOnly(out, L"请用「设置→系统→存储→临时文件」或 cleanmgr 删除", partial);
        }
    }

    progress(72, L"扫描 WinSxS 临时与备份...");
    wstring wsTemp = Join(windir, L"WinSxS\\Temp");
    if (IsAnyDir(wsTemp))
        AddItem(out, L"WinSxS 临时文件", wsTemp, DirSize(wsTemp), L"系统缓存", false,
                L"位于 WinSxS 内，与组件存储统计有重叠");
    wstring wsBackup = Join(windir, L"WinSxS\\Backup");
    if (IsAnyDir(wsBackup) &&
        AddItem(out, L"WinSxS 备份文件", wsBackup, DirSize(wsBackup), L"系统备份", false,
                L"位于 WinSxS 内，与组件存储统计有重叠"))
        MarkReportOnly(out, L"请用「DISM.exe /Online /Cleanup-Image /StartComponentCleanup」清理");

    progress(80, L"扫描虚拟内存与休眠文件...");
    // pagefile.sys / hiberfil.sys 受系统保护，GetFileAttributesExW 常失败，改用句柄/枚举取大小
    wstring pagefile = L"C:\\pagefile.sys";
    unsigned long long pfSize = FileSizeByHandle(pagefile);
    if (AddItem(out, L"虚拟内存文件(pagefile.sys)", pagefile, pfSize, L"系统文件", true,
                L"系统托管文件，请通过系统属性调整", L"高"))
        MarkReportOnly(out, L"请用「系统属性→高级→性能→虚拟内存」调整");

    wstring hiberfil = L"C:\\hiberfil.sys";
    unsigned long long hbSize = FileSizeByHandle(hiberfil);
    if (AddItem(out, L"休眠文件(hiberfil.sys)", hiberfil, hbSize, L"系统文件", true,
                L"休眠/快速启动使用，请勿直接删除", L"高"))
        MarkReportOnly(out, L"请用「powercfg /h off」关闭休眠");

    progress(86, L"扫描组件存储(预估)...");
    wstring winSxS = Join(windir, L"WinSxS");
    if (IsAnyDir(winSxS)) {
        progress(90, L"正在统计 WinSxS（较慢）...");
        bool partial = false;
        unsigned long long sz = DirSizeWithBudget(winSxS, 25000, &partial);
        if (AddItem(out, L"组件存储(WinSxS)", winSxS, sz, L"组件存储", false,
                    L"仅为预估，实际清理需 DISM", L"高"))
            MarkReportOnly(out,
                L"请用「DISM.exe /Online /Cleanup-Image /StartComponentCleanup」清理", partial);
    }
    // 父子目录去重：Windows\Logs 覆盖 Logs\CBS / Logs\DISM / Logs\SIH 等子项
    {
        int pruned = PruneNestedDirs(&out);
        if (pruned > 0) {
            wstring msg = L"父子目录去重：剪除 " + std::to_wstring(pruned) +
                          L" 个已被父项覆盖的子项";
            out.note = out.note.empty() ? msg : out.note + L"；" + msg;
        }
    }
    progress(100, L"系统清理扫描完成");
}

static void ScanWeChat(ScanOutcome& out, const ProgressFn& progress) {
    wstring userProfile = GetEnv(L"USERPROFILE");
    wstring docs = Join(userProfile, L"Documents");
    struct Sub { const wchar_t* name; const wchar_t* label; };
    Sub subs[] = {
        { L"Image", L"图片缓存" }, { L"Video", L"视频缓存" },
        { L"File", L"收发文件" }, { L"MsgAttach", L"聊天附件" },
        { L"CustomEmotion", L"表情缓存" }, { L"Temp", L"临时文件" },
    };
    bool found = false;
    const wchar_t* bases[] = { L"xwechat_files", L"WeChat Files" };
    for (auto base : bases) {
        wstring b = Join(docs, base);
        if (!IsAnyDir(b)) continue;
        found = true;
        for (auto& wx : ListDir(b)) {
            wstring wxPath = Join(b, wx);
            if (!IsAnyDir(wxPath) || IsLink(wxPath)) continue;
            vector<wstring> roots = { Join(wxPath, L"FileStorage"), Join(wxPath, L"msg\\attach") };
            for (auto& r : roots) {
                if (!IsAnyDir(r)) continue;
                for (auto& s : subs) {
                    wstring p = Join(r, s.name);
                    if (!IsAnyDir(p)) continue;
                    wstring nm = wstring(L"微信-") + s.label + L"(" + wx + L")";
                    AddItem(out, nm, p, DirSize(p), L"微信缓存");
                }
            }
        }
    }
    progress(100, found ? L"微信清理扫描完成" : L"未找到微信数据目录");
}

static void ScanQQ(ScanOutcome& out, const ProgressFn& progress) {
    wstring userProfile = GetEnv(L"USERPROFILE");
    wstring docs = Join(userProfile, L"Documents");
    struct Sub { const wchar_t* name; const wchar_t* label; };
    Sub subs[] = {
        { L"Image", L"图片" }, { L"Video", L"视频" }, { L"FileRecv", L"接收文件" },
        { L"Audio", L"语音消息" }, { L"Pic", L"聊天图片" },
    };
    const wchar_t* bases[] = { L"Tencent Files", L"QQ" };
    for (auto base : bases) {
        wstring b = Join(docs, base);
        if (!IsAnyDir(b)) continue;
        for (auto& qq : ListDir(b)) {
            wstring qp = Join(b, qq);
            if (!IsAnyDir(qp) || IsLink(qp)) continue;
            for (auto& s : subs) {
                wstring p = Join(qp, s.name);
                if (!IsAnyDir(p)) continue;
                wstring nm = wstring(L"QQ-") + s.label + L"(" + qq + L")";
                AddItem(out, nm, p, DirSize(p), L"QQ缓存");
            }
        }
    }
    progress(100, L"QQ清理扫描完成");
}

static void ScanLarge(ScanOutcome& out, const ScanOptions& opt,
                      const ProgressFn& progress, const CancelFn& cancel) {
    unsigned long long minSize = (unsigned long long)opt.minSizeMB * 1024 * 1024;
    unsigned long long scanned = 0;
    WalkDir(opt.rootPath, cancel, [&](const wstring& full, unsigned long long sz, const wstring& name) {
        ++scanned;
        if (scanned % 2000 == 0) progress(-1, L"已扫描 " + std::to_wstring(scanned) + L" 个文件...");
        if (sz >= minSize) {
            ScanItem it; it.name = name; it.path = full; it.size = sz;
            it.category = L"大文件"; it.isFile = true;
            out.items.push_back(it);
            out.totalSize += sz;
        }
        return true;
    });
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"大文件查找完成");
}

// MD5（Win32 CryptoAPI）
static bool FileMd5(const wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CloseHandle(h); return false;
    }
    bool ok = false;
    if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        vector<BYTE> buf(65536);
        DWORD read = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
            CryptHashData(hash, buf.data(), read, 0);
        }
        BYTE digest[16]; DWORD len = 16;
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            char hex[33];
            for (int i = 0; i < 16; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
            *out = std::string(hex, 32);
            ok = true;
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
}

static void ScanDuplicates(ScanOutcome& out, const ScanOptions& opt,
                           const ProgressFn& progress, const CancelFn& cancel) {
    std::map<unsigned long long, vector<wstring>> bySize;
    WalkDir(opt.rootPath, cancel, [&](const wstring& full, unsigned long long sz, const wstring&) {
        if (sz >= 1024 && sz <= 4096ULL * 1024 * 1024) bySize[sz].push_back(full);
        return true;
    });
    progress(40, L"正在比对哈希值...");

    int groups = 0, total = 0;
    for (auto& kv : bySize) if (kv.second.size() >= 2) ++total;

    for (auto& kv : bySize) {
        if (cancel && cancel()) { out.cancelled = true; return; }
        if (kv.second.size() < 2) continue;
        ++groups;
        progress(40 + groups * 55 / (total > 0 ? total : 1), L"比对哈希 " + std::to_wstring(groups) + L"/" + std::to_wstring(total));

        std::map<std::string, vector<wstring>> byHash;
        for (auto& f : kv.second) {
            std::string h;
            if (FileMd5(f, &h)) byHash[h].push_back(f);
        }
        for (auto& hk : byHash) {
            if (hk.second.size() < 2) continue;
            // 保留第一个，其余标记为重复
            for (size_t i = 1; i < hk.second.size(); ++i) {
                ScanItem it;
                it.path = hk.second[i];
                size_t pos = it.path.find_last_of(L'\\');
                it.name = (pos == wstring::npos) ? it.path : it.path.substr(pos + 1);
                it.size = kv.first;
                it.category = L"重复文件";
                it.isFile = true;
                it.note = L"保留: " + hk.second[0];
                out.items.push_back(it);
                out.totalSize += it.size;
            }
        }
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"重复文件查找完成");
}

static wstring NormalizeName(const wstring& filename) {
    size_t dot = filename.find_last_of(L'.');
    wstring base = (dot == wstring::npos) ? filename : filename.substr(0, dot);
    // 去除括号内容
    wstring cleaned;
    int depth = 0;
    for (wchar_t c : base) {
        if (c == L'(' || c == 0xFF08) { depth++; continue; }
        if (c == L')' || c == 0xFF09) { if (depth > 0) depth--; continue; }
        if (depth == 0) cleaned.push_back(c);
    }
    // 去尾数字
    while (!cleaned.empty() && (iswdigit(cleaned.back()) || cleaned.back() == L' ' ||
                                cleaned.back() == L'_' || cleaned.back() == L'-'))
        cleaned.pop_back();
    for (auto& c : cleaned) c = towlower(c);
    return cleaned;
}

static void ScanSimilar(ScanOutcome& out, const ScanOptions& opt,
                        const ProgressFn& progress, const CancelFn& cancel) {
    static const std::set<wstring> exts = {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".mp4", L".avi", L".mkv",
        L".mov", L".mp3", L".wav", L".flac", L".doc", L".docx", L".pdf",
        L".xlsx", L".pptx", L".zip", L".rar", L".7z"
    };
    std::map<std::pair<wstring, unsigned long long>, vector<wstring>> groups;
    WalkDir(opt.rootPath, cancel, [&](const wstring& full, unsigned long long sz, const wstring& name) {
        size_t dot = name.find_last_of(L'.');
        if (dot == wstring::npos) return true;
        wstring ext = name.substr(dot);
        for (auto& c : ext) c = towlower(c);
        if (!exts.count(ext)) return true;
        if (sz <= 1024 || sz >= 10ULL * 1024 * 1024) return true;
        groups[{NormalizeName(name), sz}].push_back(full);
        return true;
    });
    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;
        for (auto& f : kv.second) {
            ScanItem it; it.path = f;
            size_t p = f.find_last_of(L'\\');
            it.name = (p == wstring::npos) ? f : f.substr(p + 1);
            it.size = kv.first.second;
            it.category = L"相似文件";
            it.isFile = true;
            out.items.push_back(it);
            out.totalSize += it.size;
        }
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"相似文件查找完成");
}

static void ScanDirSizes(ScanOutcome& out, const ScanOptions& opt, const ProgressFn& progress) {
    for (auto& name : ListDir(opt.rootPath)) {
        wstring p = Join(opt.rootPath, name);
        if (!IsDir(p)) continue;
        progress(-1, L"正在计算: " + name);
        unsigned long long sz = DirSize(p);
        ScanItem it; it.name = name; it.path = p; it.size = sz;
        it.category = L"目录"; it.isFile = false;
        out.items.push_back(it);
        out.totalSize += sz;
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"目录大小查看完成");
}

// ---- 已安装软件 ----

struct InstalledApp {
    wstring name;
    wstring location;
    wstring uninstall;
};

static vector<InstalledApp> EnumInstalledApps() {
    vector<InstalledApp> apps;
    struct { HKEY hive; const wchar_t* path; } roots[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (auto& r : roots) {
        HKEY hk;
        if (RegOpenKeyExW(r.hive, r.path, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        DWORD subCount = 0;
        RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr, &subCount, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr);
        for (DWORD i = 0; i < subCount; ++i) {
            wchar_t subName[512];
            DWORD len = 512;
            if (RegEnumKeyExW(hk, i, subName, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                continue;
            HKEY sk;
            if (RegOpenKeyExW(hk, subName, 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;

            auto readStr = [&](const wchar_t* vname, wstring* out) -> bool {
                DWORD type = 0, size = 0;
                if (RegQueryValueExW(sk, vname, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
                    return false;
                if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
                vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
                if (RegQueryValueExW(sk, vname, nullptr, &type, (BYTE*)buf.data(), &size) != ERROR_SUCCESS)
                    return false;
                *out = buf.data();
                return true;
            };
            InstalledApp a;
            if (!readStr(L"DisplayName", &a.name)) { RegCloseKey(sk); continue; }
            readStr(L"InstallLocation", &a.location);
            readStr(L"UninstallString", &a.uninstall);
            RegCloseKey(sk);
            apps.push_back(a);
        }
        RegCloseKey(hk);
    }
    return apps;
}

static void AssessRisk(const wstring& name, wstring* risk, wstring* note) {
    struct { const wchar_t* key; const wchar_t* msg; const wchar_t* level; } rules[] = {
        { L"sql server",      L"SQL Server 为系统服务，数据目录与实例路径强绑定，请勿直接搬家。", L"极高" },
        { L"office",          L"Office 采用 Click-to-Run 机制，不支持目录搬家。", L"极高" },
        { L"visual studio",   L"Visual Studio 包含大量绝对路径与服务，搬家风险极高。", L"高" },
        { L"photoshop",       L"Photoshop 依赖 Creative Cloud 与系统服务，搬家后可能无法启动。", L"高" },
        { L"adobe",           L"Adobe 系列使用绝对路径与授权组件，搬家后可能需重新激活。", L"高" },
        { L"android studio",  L"依赖 SDK/AVD 绝对路径，搬家后需重新配置。", L"高" },
        { L"steam",           L"建议使用 Steam 自带的「移动安装文件夹」功能。", L"中" },
    };
    wstring low = name;
    for (auto& c : low) c = towlower(c);
    for (auto& r : rules) {
        if (low.find(r.key) != wstring::npos) {
            *risk = r.level; *note = r.msg; return;
        }
    }
    *risk = L"低";
}

static void AddAppItems(ScanOutcome& out, const ProgressFn& progress, int minSizeMB) {
    auto apps = EnumInstalledApps();
    int i = 0, n = (int)apps.size();
    unsigned long long minSize = (unsigned long long)minSizeMB * 1024 * 1024;
    std::set<wstring> seen;
    for (auto& a : apps) {
        progress(10 + i * 85 / (n > 0 ? n : 1), L"正在统计: " + a.name);
        ++i;
        wstring loc = a.location;
        // 去引号与空白
        while (!loc.empty() && (loc.back() == L' ' || loc.back() == L'"')) loc.pop_back();
        while (!loc.empty() && loc.front() == L'"') loc.erase(0, 1);
        if (!loc.empty() && !IsAnyDir(loc)) loc.clear();
        if (loc.empty()) continue;

        wstring low = loc;
        for (auto& c : low) c = towlower(c);
        if (seen.count(low)) continue;
        seen.insert(low);

        unsigned long long sz = DirSize(loc);
        if (sz == 0 || sz < minSize) continue;

        ScanItem it;
        it.name = a.name; it.path = loc; it.size = sz;
        it.category = L"已安装软件"; it.isFile = false;
        AssessRisk(a.name, &it.risk, &it.note);
        it.migratable = (towupper(loc[0]) == L'C');
        out.items.push_back(it);
        out.totalSize += sz;
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"软件扫描完成");
}

static wstring ShellFolderPath(const wchar_t* regName) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD size = sizeof(buf), type = 0;
    wstring out;
    if (RegQueryValueExW(k, regName, nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS) {
        DWORD n = ExpandEnvironmentStringsW(buf, nullptr, 0);
        if (n > 0) {
            vector<wchar_t> ex(n);
            ExpandEnvironmentStringsW(buf, ex.data(), n);
            out = ex.data();
        }
    }
    RegCloseKey(k);
    return out;
}

static void ScanUserDirs(ScanOutcome& out, const ProgressFn& progress) {
    struct { const wchar_t* label; const wchar_t* regName; } dirs[] = {
        { L"桌面", L"Desktop" },
        { L"文档", L"Personal" },
        { L"下载", L"{374DE290-123F-4565-9164-39C4925E467B}" },
        { L"图片", L"My Pictures" },
        { L"音乐", L"My Music" },
        { L"视频", L"My Video" },
        { L"收藏夹", L"Favorites" },
    };
    int i = 0;
    for (auto& d : dirs) {
        progress(5 + i++ * 90 / 7, wstring(L"正在计算: ") + d.label);
        wstring p = ShellFolderPath(d.regName);
        if (p.empty() || !IsAnyDir(p)) continue;
        unsigned long long sz = DirSize(p);
        ScanItem it;
        it.name = d.label; it.path = p; it.size = sz;
        it.category = L"用户目录"; it.isFile = false;
        it.migratable = (towupper(p[0]) == L'C');
        out.items.push_back(it);
        out.totalSize += sz;
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const ScanItem& a, const ScanItem& b) { return a.size > b.size; });
    progress(100, L"用户目录扫描完成");
}

// ------------------------------------------------------------------ 入口

// 安全调用进度回调：progress 可能为空（CLI 静默模式）
static inline void Report(const ProgressFn& progress, int pct, const wstring& msg) {
    if (progress) progress(pct, msg);
}

ScanOutcome RunScan(ScanKind kind, const ScanOptions& opt,
                    const ProgressFn& progress, const CancelFn& cancel) {
    ScanOutcome out;
    // 包一层，确保内部所有 progress 调用都判空
    ProgressFn safe = [&progress](int p, const wstring& s) {
        if (progress) progress(p, s);
    };
    switch (kind) {
        case ScanKind::BasicClean:    ScanBasic(out, safe); break;
        case ScanKind::SystemClean:   ScanSystem(out, safe); break;
        case ScanKind::WeChatClean:   ScanWeChat(out, safe); break;
        case ScanKind::QQClean:       ScanQQ(out, safe); break;
        case ScanKind::LargeFiles:    ScanLarge(out, opt, safe, cancel); break;
        case ScanKind::DuplicateFiles:ScanDuplicates(out, opt, safe, cancel); break;
        case ScanKind::SimilarFiles:  ScanSimilar(out, opt, safe, cancel); break;
        case ScanKind::DirSizes:      ScanDirSizes(out, opt, safe); break;
        case ScanKind::SoftwareUsage: AddAppItems(out, safe, 0); break;
        case ScanKind::SoftwareMove:  AddAppItems(out, safe, 50); break;
        case ScanKind::UserDirMigrate:ScanUserDirs(out, safe); break;
    }
    if (cancel && cancel()) out.cancelled = true;
    return out;
}

// ------------------------------------------------------------------ 清理

static bool RemoveTree(const wstring& dir) {
    vector<wstring> names = ListDir(dir);
    for (auto& n : names) {
        wstring full = Join(dir, n);
        DWORD attr = GetFileAttributesW(full.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
                RemoveDirectoryW(full.c_str());
            } else {
                RemoveTree(full);
                RemoveDirectoryW(full.c_str());
            }
        } else {
            if (attr & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(full.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
            DeleteFileW(full.c_str());
        }
    }
    return true;
}

bool DeleteSingleFile(const wstring& file, wstring* err) {
    DWORD attr = GetFileAttributesW(file.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true; // 幂等
        if (err) *err = L"文件不存在或无法访问";
        return false;
    }
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(file.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileW(file.c_str())) return true;
    if (err) *err = L"删除失败，错误码 " + std::to_wstring(GetLastError());
    return false;
}

// v2.8：转发 CleanDirectoryContentsEx（cleanresult.cpp）。
// 旧实现忽略全部删除返回值、用「删除前估值」冒充释放量——已废弃。
// 逐文件智能删除 L1~L4 + 顶层危险路径终检 + 真实释放量（按实际删除文件大小累计）。
unsigned long long CleanDirectoryContents(const wstring& dir, const CancelFn& cancel, wstring* err) {
    if (!IsAnyDir(dir)) {
        if (err) *err = L"目录不存在";
        return 0;
    }
    CleanReport report;
    int rebootBudget = 50;
    CleanDirectoryContentsEx(dir, cancel, report, &rebootBudget);
    if (err) *err = report.FailSummary();   // 无失败时为空串
    return report.freedBytes;
}

// ------------------------------------------------------------------ 迁移

static wstring NormPath(const wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    wstring s = (n > 0) ? wstring(buf, n) : p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    for (auto& c : s) c = towlower(c);
    return s;
}

static bool IsSystemPath(const wstring& p) {
    static const wchar_t* blocked[] = {
        L"c:\\windows", L"c:\\programdata", L"c:\\program files",
        L"c:\\program files (x86)", L"c:\\users\\default", L"c:\\$recycle.bin",
        L"c:\\system volume information",
    };
    wstring n = NormPath(p);
    for (auto b : blocked) {
        size_t bl = wcslen(b);
        if (n.compare(0, bl, b) == 0 && (n.size() == bl || n[bl] == L'\\')) return true;
    }
    return false;
}

TransferResult PlanTransfer(const wstring& source, const wstring& target) {
    TransferResult r;
    if (!IsAnyDir(source)) { r.message = L"源目录不存在"; return r; }
    wstring ns = NormPath(source), nt = NormPath(target);
    if (ns == nt || nt.compare(0, ns.size() + 1, ns + L"\\") == 0) {
        r.message = L"目标目录不能位于源目录内部"; return r;
    }
    if (ns.compare(0, nt.size() + 1, nt + L"\\") == 0) {
        r.message = L"源目录不能位于目标目录内部"; return r;
    }
    if (IsSystemPath(source)) { r.message = L"系统关键目录不允许迁移"; return r; }

    wchar_t ds = towupper(source[0]), dt = towupper(target[0]);
    if (ds == dt) { r.message = L"源与目标位于同一磁盘，迁移不会释放空间"; return r; }
    wstring driveRoot; driveRoot.push_back(dt); driveRoot += L":\\";
    if (!IsAnyDir(driveRoot)) { r.message = L"目标磁盘不可用"; return r; }
    if (IsAnyDir(target) && !ListDir(target).empty()) {
        r.message = L"目标目录已存在且非空"; return r;
    }
    r.movedBytes = DirSize(source);
    ULARGE_INTEGER freeBytes;
    if (GetDiskFreeSpaceExW(driveRoot.c_str(), &freeBytes, nullptr, nullptr)) {
        if (freeBytes.QuadPart < (unsigned long long)(r.movedBytes * 1.1)) {
            r.message = L"目标磁盘可用空间不足，需要 " + FormatSize((unsigned long long)(r.movedBytes * 1.1));
            return r;
        }
    }
    r.success = true;
    return r;
}

static bool CopyTree(const wstring& src, const wstring& dst) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(src, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wstring s = Join(src, fd.cFileName);
        wstring d = Join(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            CopyTree(s, d);
        } else {
            if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
                // 清掉只读再试
                DWORD a = GetFileAttributesW(s.c_str());
                if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(s.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) return false;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

TransferResult ExecuteTransfer(const wstring& source, const wstring& target,
                               bool dryRun, const ProgressFn& progress) {
    TransferResult plan = PlanTransfer(source, target);
    if (!plan.success) return plan;

    if (dryRun) {
        plan.message = L"[预演] 将迁移 " + FormatSize(plan.movedBytes) +
                       L" 到 " + target + L"，并创建目录联接";
        return plan;
    }

    // 1) 源目录改名为暂存，腾空原路径
    wstring staging = source + L".mz_staging";
    DWORD pid = GetCurrentProcessId();
    staging += std::to_wstring(pid);
    progress(5, L"正在准备迁移...");
    if (!MoveFileExW(source.c_str(), staging.c_str(), 0)) {
        TransferResult f; f.message = L"无法锁定源目录（可能被占用），错误码 " +
                                     std::to_wstring(GetLastError());
        return f;
    }

    // 2) 复制
    progress(20, L"正在复制文件...");
    if (!CopyTree(staging, target)) {
        MoveFileExW(staging.c_str(), source.c_str(), 0);   // 回滚
        TransferResult f; f.message = L"复制失败，源目录已还原";
        return f;
    }

    // 3) 校验
    progress(70, L"正在校验...");
    unsigned long long srcSize = DirSize(staging);
    unsigned long long dstSize = DirSize(target);
    if (srcSize != dstSize) {
        MoveFileExW(staging.c_str(), source.c_str(), 0);
        TransferResult f;
        f.message = L"校验失败（源 " + FormatSize(srcSize) + L" / 目标 " +
                    FormatSize(dstSize) + L"），已还原源目录";
        return f;
    }

    // 4) 建立 Junction
    progress(85, L"正在创建目录联接...");
    wstring cmd = L"cmd /c mklink /J \"" + source + L"\" \"" + target + L"\"";
    // CreateProcess 需要可写缓冲
    vector<wchar_t> cbuf(cmd.begin(), cmd.end());
    cbuf.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    bool junctionOk = false;
    if (CreateProcessW(nullptr, cbuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        junctionOk = (code == 0) && IsAnyDir(source);
    }
    if (!junctionOk) {
        MoveFileExW(staging.c_str(), source.c_str(), 0);
        TransferResult f;
        f.message = L"复制成功但创建联接失败，源目录已还原（请以管理员身份重试）";
        return f;
    }

    // 5) 删除暂存
    progress(95, L"正在清理原目录...");
    RemoveTree(staging);
    RemoveDirectoryW(staging.c_str());

    progress(100, L"迁移完成");
    TransferResult r;
    r.success = true;
    r.movedBytes = srcSize;
    r.junctionCreated = true;
    r.message = L"迁移成功，释放 " + FormatSize(srcSize);
    return r;
}

} // namespace mz
