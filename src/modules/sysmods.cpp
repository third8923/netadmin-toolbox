// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 系统信息模块集
//  ---------------------------------------------------------------------------
//  本文件只新增模块，不改动框架与既有文件。注册的 8 个模块：
//    sys.info      系统概览   Inventory   OS/CPU/内存/开机时长/计算机名…
//    sys.disk      磁盘信息   Inventory   盘符/卷标/文件系统/容量/使用率
//    sys.process   进程列表   Inventory   PID/内存/线程数/启动时间/路径
//    sys.startup   启动项     Inventory   注册表 Run 系列 + 启动文件夹
//    sys.services  服务列表   Inventory   状态/启动类型/PID/账户
//    sys.hotfix    已安装补丁 Inventory   从 CBS 组件包解析近期更新(KB)
//    sys.env       环境变量   Inventory   系统 / 用户持久化环境变量
//    sys.hosts     hosts 文件 Console     解析展示 + 图形化编辑（多行编辑器/
//                                添加映射/屏蔽模板，写前备份 hosts.bak，v2.15）
//
//  数据全部来自真实 Windows API / 注册表；任何失败都只降级为 summary 文本，
//  不弹窗、不崩溃；长循环支持 ctx.Cancelled() 中止。
//  依赖库：psapi、advapi32、shell32（另需系统默认的 kernel32/user32）。
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"   // ErrorTextZh（服务/进程错误码中文归因）

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>

#include <psapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

#include "../ui/dpi.h"        // ui::dpi::S（hosts 编辑对话框尺寸，v2.15）

namespace mz {
namespace {

// ===========================================================================
//  通用小工具（格式化 / 错误文本 / 时间）
// ===========================================================================

wstring NumU64(unsigned long long v) {
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"%llu", v);
    return b;
}

wstring NumU32(unsigned long v) {
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"%lu", v);
    return b;
}

wstring FmtPct(double pct) {
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"%.1f%%", pct);
    return b;
}

wstring TrimW(const wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

wstring CapW(const wstring& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + L"…";
}

wstring LowerW(const wstring& s) {
    wstring o = s;
    for (auto& c : o) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return o;
}

wstring Win32Err(DWORD err) {
    if (err == 0) return L"成功";
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    wstring out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
        LocalFree(buf);
    }
    if (out.empty()) out = L"错误码 " + NumU32(err);
    return out;
}

wstring LastErrW() { return Win32Err(GetLastError()); }

// FILETIME（UTC，100ns）→ 本地时间 "YYYY-MM-DD HH:MM:SS"
wstring FmtFileTimeLocal(const FILETIME& ftUtc) {
    FILETIME local = ftUtc;
    if (!FileTimeToLocalFileTime(&ftUtc, &local)) local = ftUtc;
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&local, &st)) return L"-";
    wchar_t b[64] = {};
    swprintf_s(b, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
               (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
               (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    return b;
}

wstring FmtFileTimeLocalTicks(unsigned long long ticks) {
    FILETIME ft = {};
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(ticks >> 32);
    return FmtFileTimeLocal(ft);
}

// Unix 时间戳（秒）→ 本地时间
wstring FmtUnixLocal(DWORD sec) {
    unsigned long long ticks = (unsigned long long)sec * 10000000ULL + 116444736000000000ULL;
    return FmtFileTimeLocalTicks(ticks);
}

// 毫秒时长 → "X 天 X 小时 X 分钟"
wstring FmtDuration(unsigned long long ms) {
    unsigned long long sec = ms / 1000;
    unsigned long long d = sec / 86400; sec %= 86400;
    unsigned long long h = sec / 3600;  sec %= 3600;
    unsigned long long m = sec / 60;
    wstring out;
    if (d) out += NumU64(d) + L" 天 ";
    if (d || h) out += NumU64(h) + L" 小时 ";
    out += NumU64(m) + L" 分钟";
    return out;
}

wstring GetEnvW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return L"";
    wstring buf(n, L'\0');
    DWORD m = GetEnvironmentVariableW(name, buf.data(), n);
    if (m == 0 || m >= n) return L"";
    buf.resize(m);
    return buf;
}

wstring ExpandEnvW(const wstring& s) {
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0) return s;
    wstring buf(n, L'\0');
    DWORD m = ExpandEnvironmentStringsW(s.c_str(), buf.data(), n);
    if (m == 0 || m > n) return s;
    buf.resize(m - 1);
    return buf;
}

// ===========================================================================
//  注册表工具
// ===========================================================================

struct RegValue {
    wstring name;
    wstring data;
    DWORD type = 0;
};

bool RegReadStringW(HKEY root, const wchar_t* sub, const wchar_t* name, wstring& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(hk, name, nullptr, &type, nullptr, &size);
    bool ok = false;
    if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
        wstring buf(size / sizeof(wchar_t), L'\0');
        r = RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)buf.data(), &size);
        if (r == ERROR_SUCCESS) {
            buf.resize(size / sizeof(wchar_t));
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            out = buf;
            ok = true;
        }
    }
    RegCloseKey(hk);
    return ok;
}

bool RegReadDwordW(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = sizeof(DWORD), v = 0;
    LONG r = RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&v, &size);
    RegCloseKey(hk);
    if (r == ERROR_SUCCESS && type == REG_DWORD) {
        out = v;
        return true;
    }
    return false;
}

// 已打开的注册表键：读字符串 / DWORD
bool KeyQueryString(HKEY hk, const wchar_t* name, wstring& out) {
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(hk, name, nullptr, &type, nullptr, &size);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) return false;
    wstring buf(size / sizeof(wchar_t), L'\0');
    r = RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)buf.data(), &size);
    if (r != ERROR_SUCCESS) return false;
    buf.resize(size / sizeof(wchar_t));
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = buf;
    return true;
}

bool KeyQueryDword(HKEY hk, const wchar_t* name, DWORD& out) {
    DWORD type = 0, size = sizeof(DWORD), v = 0;
    LONG r = RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&v, &size);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = v;
    return true;
}

// 枚举键下的字符串值（REG_SZ / REG_EXPAND_SZ / REG_MULTI_SZ，MULTI_SZ 用 " ; " 连接）
bool RegEnumStringValues(HKEY root, const wchar_t* sub, vector<RegValue>& out) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;

    DWORD values = 0, maxName = 0, maxData = 0;
    RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     &values, &maxName, &maxData, nullptr, nullptr);

    wstring nameBuf((size_t)maxName + 2, L'\0');
    vector<BYTE> dataBuf((size_t)maxData + 32);

    DWORD i = 0;
    while (i < values) {
        DWORD nameLen = (DWORD)nameBuf.size();
        DWORD dataLen = (DWORD)dataBuf.size();
        DWORD type = 0;
        LONG r = RegEnumValueW(hk, i, nameBuf.data(), &nameLen, nullptr, &type, dataBuf.data(), &dataLen);
        if (r == ERROR_MORE_DATA) {
            if (dataLen > dataBuf.size()) {
                dataBuf.resize((size_t)dataLen + 32);
                continue;  // 同一索引重试
            }
            ++i;
            continue;
        }
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) { ++i; continue; }

        wstring name(nameBuf.data(), nameLen);
        wstring data;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            data.assign((const wchar_t*)dataBuf.data(), dataLen / sizeof(wchar_t));
            while (!data.empty() && data.back() == L'\0') data.pop_back();
        } else if (type == REG_MULTI_SZ) {
            size_t n = dataLen / sizeof(wchar_t);
            const wchar_t* p = (const wchar_t*)dataBuf.data();
            wstring item;
            auto flush = [&]() {
                if (item.empty()) return;
                if (!data.empty()) data += L" ; ";
                data += item;
                item.clear();
            };
            for (size_t k = 0; k < n; ++k) {
                if (p[k] == L'\0') flush();
                else item += p[k];
            }
            flush();
        } else {
            ++i;
            continue;  // 非字符串类型不展示
        }
        out.push_back({name, data, type});
        ++i;
    }
    RegCloseKey(hk);
    return true;
}

// ===========================================================================
//  系统版本（RtlGetVersion + 注册表补充，规避 manifest 兼容性）
// ===========================================================================

struct SysVer {
    DWORD major = 0, minor = 0, build = 0, ubr = 0;
    wstring productName, displayVersion, editionId, buildLab, csd;

    wstring BuildText() const {
        wstring s = NumU32(major) + L"." + NumU32(minor) + L"." + NumU32(build);
        if (ubr) s += L"." + NumU32(ubr);
        return s;
    }
    wstring NameText() const {
        wstring s = productName.empty() ? (L"Windows " + NumU32(major) + L"." + NumU32(minor)) : productName;
        if (!displayVersion.empty()) s += L" " + displayVersion;
        return s;
    }
};

SysVer QuerySysVer() {
    SysVer v;

    struct RtlOsVersionInfo {
        ULONG size;
        ULONG major;
        ULONG minor;
        ULONG build;
        ULONG platform;
        WCHAR csd[128];
    };
    typedef LONG(WINAPI* RtlGetVersionFn)(RtlOsVersionInfo*);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            RtlOsVersionInfo vi = {};
            vi.size = sizeof(vi);
            if (fn(&vi) == 0) {
                v.major = vi.major;
                v.minor = vi.minor;
                v.build = vi.build;
                v.csd.assign(vi.csd);
            }
        }
    }
    if (v.major == 0) {
        OSVERSIONINFOW oi = {};
        oi.dwOSVersionInfoSize = sizeof(oi);
        if (GetVersionExW(&oi)) {
            v.major = oi.dwMajorVersion;
            v.minor = oi.dwMinorVersion;
            v.build = oi.dwBuildNumber;
            v.csd.assign(oi.szCSDVersion);
        }
    }

    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    wstring s;
    DWORD d = 0;
    if (RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"ProductName", s)) v.productName = s;
    if (RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", s)) v.displayVersion = s;
    if (RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"EditionID", s)) v.editionId = s;
    if (RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"BuildLabEx", s)) v.buildLab = s;
    if (RegReadDwordW(HKEY_LOCAL_MACHINE, cv, L"UBR", d)) v.ubr = d;
    if (v.build == 0) {
        if (RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber", s) ||
            RegReadStringW(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild", s)) {
            v.build = (DWORD)wcstoul(s.c_str(), nullptr, 10);
        }
    }
    return v;
}

wstring ArchText(WORD arch) {
    switch (arch) {
    case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
    case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64";
    case PROCESSOR_ARCHITECTURE_ARM:   return L"ARM";
    case PROCESSOR_ARCHITECTURE_IA64:  return L"IA64";
    case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
    default: return L"未知(" + NumU32(arch) + L")";
    }
}

unsigned CountPhysicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return 0;
    vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        return 0;
    }
    unsigned count = 0;
    size_t off = 0;
    // 注意：单条记录长度(p->Size)可能小于 sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)，
    // 不能用结构体大小做循环条件，否则会漏数物理核。
    while (off + 8 <= len) {
        auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (p->Relationship == RelationProcessorCore) ++count;
        if (p->Size == 0) break;
        off += p->Size;
    }
    return count;
}

wstring CpuName() {
    wstring s;
    if (RegReadStringW(HKEY_LOCAL_MACHINE,
                       L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"ProcessorNameString", s)) {
        s = TrimW(s);
    }
    return s;
}

// ===========================================================================
//  1) sys.info —— 系统概览
// ===========================================================================

class SysInfoModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.info"; }
    const wchar_t* Group() const override { return L"系统信息"; }
    const wchar_t* Title() const override { return L"系统概览"; }
    const wchar_t* Description() const override { return L"操作系统版本、CPU、内存、开机时长与计算机信息一览"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"正在采集系统信息…");

        SysVer v = QuerySysVer();
        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);
        wstring arch = ArchText(si.wProcessorArchitecture);

        unsigned logical = (unsigned)si.dwNumberOfProcessors;
        DWORD apc = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (apc) logical = apc;
        unsigned cores = CountPhysicalCores();
        if (cores == 0) cores = logical;

        wstring cpu = CpuName();
        if (cpu.empty()) cpu = L"未知处理器";

        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        bool memOk = GlobalMemoryStatusEx(&ms) != FALSE;

        wchar_t cname[256] = {};
        DWORD cnsz = 256;
        bool cnOk = GetComputerNameExW(ComputerNameDnsHostname, cname, &cnsz) != FALSE;

        wchar_t uname[256] = {};
        DWORD unsz = 256;
        bool userOk = GetUserNameW(uname, &unsz) != FALSE;
        wstring domain = GetEnvW(L"USERDOMAIN");
        wstring userText = domain.empty() ? uname : domain + L"\\" + uname;

        unsigned long long upMs = GetTickCount64();

        FILETIME nowFt = {};
        GetSystemTimeAsFileTime(&nowFt);
        unsigned long long now100 = ((unsigned long long)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
        unsigned long long start100 = (now100 > upMs * 10000ULL) ? (now100 - upMs * 10000ULL) : 0;
        wstring bootTime = start100 ? FmtFileTimeLocalTicks(start100) : L"-";

        wchar_t wd[1024] = {};
        UINT wdn = GetWindowsDirectoryW(wd, 1024);
        wstring winDir = (wdn > 0 && wdn < 1024) ? wstring(wd, wdn) : L"";

        wstring sysDrive = GetEnvW(L"SystemDrive");
        if (sysDrive.empty() && !winDir.empty()) sysDrive = winDir.substr(0, 2);
        if (!sysDrive.empty() && sysDrive.back() != L'\\') sysDrive += L"\\";

        wstring installText;
        DWORD installTs = 0;
        if (RegReadDwordW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          L"InstallDate", installTs) && installTs > 0) {
            installText = FmtUnixLocal(installTs);
        }

        TIME_ZONE_INFORMATION tzi = {};
        DWORD tzr = GetTimeZoneInformation(&tzi);
        int biasMin = -(int)(tzi.Bias + ((tzr == TIME_ZONE_ID_DAYLIGHT) ? tzi.DaylightBias : 0));
        int absMin = biasMin < 0 ? -biasMin : biasMin;
        wchar_t tzb[64] = {};
        swprintf_s(tzb, 64, L"UTC%c%02d:%02d", biasMin < 0 ? L'-' : L'+', absMin / 60, absMin % 60);
        wstring tzText = tzb;
        if (tzi.StandardName[0]) tzText += L" · " + wstring(tzi.StandardName);

        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        int monitors = GetSystemMetrics(SM_CMONITORS);

        auto add = [&](const wstring& k, const wstring& val, const wstring& cat, const wstring& extra) {
            out.rows.push_back({k, val, cat, extra, 0, false});
        };

        if (ctx.Cancelled()) {
            out.summary = L"已中止";
            return;
        }

        add(L"操作系统", v.NameText(), L"系统", v.editionId);
        add(L"版本号", v.BuildText(), L"系统", v.displayVersion);
        add(L"内部版本", v.buildLab.empty() ? (L"Build " + NumU32(v.build)) : v.buildLab, L"系统", v.csd);
        add(L"系统架构", arch, L"系统", L"原生 " + arch + L" / 进程 " + NumU32((DWORD)(sizeof(void*) * 8)) + L" 位");
        add(L"计算机名", cnOk ? wstring(cname) : L"<无法获取>", L"系统", cnOk ? (L"域名：" + (domain.empty() ? L"-" : domain)) : L"");
        add(L"当前用户", userOk ? userText : L"<无法获取>", L"系统", IsElevated() ? L"管理员权限" : L"标准用户权限");
        add(L"处理器", cpu, L"硬件", NumU32(cores) + L" 物理核 / " + NumU32(logical) + L" 逻辑处理器");
        if (memOk) {
            double usedPct = (double)(ms.ullTotalPhys - ms.ullAvailPhys) * 100.0 / (double)ms.ullTotalPhys;
            add(L"物理内存", FormatSize(ms.ullTotalPhys), L"硬件",
                L"可用 " + FormatSize(ms.ullAvailPhys) + L"（已用 " + FmtPct(usedPct) + L"）");
            add(L"页面文件", FormatSize(ms.ullTotalPageFile), L"硬件",
                L"可用 " + FormatSize(ms.ullAvailPageFile) + L"（含物理内存）");
        } else {
            add(L"物理内存", L"<无法获取>", L"硬件", LastErrW());
        }
        add(L"开机时长", FmtDuration(upMs), L"状态", L"本次启动时间：" + bootTime);
        if (!installText.empty()) add(L"系统安装", installText, L"系统", L"注册表 InstallDate");
        add(L"系统目录", winDir.empty() ? L"<无法获取>" : winDir, L"系统", L"系统盘 " + (sysDrive.empty() ? L"-" : sysDrive));
        add(L"时区", tzText, L"系统", L"");
        if (scrW > 0 && scrH > 0) {
            add(L"屏幕分辨率", NumU32((unsigned)scrW) + L" x " + NumU32((unsigned)scrH), L"显示",
                L"显示器 " + NumU32((unsigned)(monitors > 0 ? monitors : 1)) + L" 台");
        }

        // 汇总
        wstring summary = v.NameText() + L" · " + v.BuildText() + L" · " + arch;
        summary += L" · " + NumU32(cores) + L" 核/" + NumU32(logical) + L" 线程";
        if (memOk) {
            summary += L" · 内存 " + FormatSize(ms.ullTotalPhys) + L"（可用 " + FormatSize(ms.ullAvailPhys) + L"）";
        }
        summary += L" · 已开机 " + FmtDuration(upMs);
        out.summary = summary;
        if (!IsElevated()) {
            out.hint = T(L"提示：当前为标准用户权限，部分受保护信息可能无法读取（以管理员身份运行可获取更完整信息）。");
        }
        ctx.Report(100, L"系统信息采集完成");
    }
};

// ===========================================================================
//  2) sys.disk —— 磁盘信息
// ===========================================================================

wstring DriveTypeText(UINT type) {
    switch (type) {
    case DRIVE_FIXED:     return L"固定磁盘";
    case DRIVE_REMOVABLE: return L"可移动磁盘";
    case DRIVE_REMOTE:    return L"网络驱动器";
    case DRIVE_CDROM:     return L"光驱";
    case DRIVE_RAMDISK:   return L"内存盘";
    case DRIVE_NO_ROOT_DIR: return L"无效盘符";
    default: return L"未知类型";
    }
}

class SysDiskModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.disk"; }
    const wchar_t* Group() const override { return L"系统信息"; }
    const wchar_t* Title() const override { return L"磁盘信息"; }
    const wchar_t* Description() const override { return L"各驱动器卷标、文件系统、总容量、可用空间与使用率"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        DWORD mask = GetLogicalDrives();
        if (mask == 0) {
            out.summary = L"无法枚举逻辑驱动器：" + LastErrW();
            return;
        }
        ctx.Report(-1, L"正在查询磁盘信息…");

        unsigned long long totalAll = 0, freeAll = 0;
        int usable = 0, dangerCount = 0;
        wstring dangerLetters;

        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            if (ctx.Cancelled()) break;

            wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
            wstring drive(root, 2);
            UINT type = GetDriveTypeW(root);
            if (type == DRIVE_NO_ROOT_DIR) continue;

            wstring label, fs;
            if (type != DRIVE_REMOTE) {
                wchar_t lb[MAX_PATH + 1] = {};
                wchar_t fsb[MAX_PATH + 1] = {};
                DWORD serial = 0, maxComp = 0, flags = 0;
                if (GetVolumeInformationW(root, lb, MAX_PATH + 1, &serial, &maxComp, &flags, fsb, MAX_PATH + 1)) {
                    label = lb;
                    fs = fsb;
                }
            }

            wstring c2 = label.empty() ? L"(无卷标)" : label;
            wstring c3 = DriveTypeText(type);
            if (!fs.empty()) c3 += L" · " + fs;

            ULARGE_INTEGER avail = {}, total = {}, totalFree = {};
            wstring c4;
            unsigned long long sortKey = 0;
            bool danger = false;

            if (type == DRIVE_REMOTE) {
                c4 = L"网络映射（为避免超时未查询容量）";
            } else if (GetDiskFreeSpaceExW(root, &avail, &total, &totalFree) && total.QuadPart > 0) {
                unsigned long long used = total.QuadPart - totalFree.QuadPart;
                double pct = (double)used * 100.0 / (double)total.QuadPart;
                totalAll += total.QuadPart;
                freeAll += totalFree.QuadPart;
                ++usable;
                c4 = L"总 " + FormatSize(total.QuadPart) + L" / 可用 " + FormatSize(totalFree.QuadPart) +
                     L"（已用 " + FmtPct(pct) + L"）";
                sortKey = total.QuadPart;
                if (pct > 90.0) {
                    danger = true;
                    ++dangerCount;
                    if (!dangerLetters.empty()) dangerLetters += L"、";
                    dangerLetters += drive;
                }
            } else {
                DWORD err = GetLastError();
                c4 = L"容量不可用（设备未就绪或无介质）";
                if (err) c4 += L"：" + Win32Err(err);
            }

            out.rows.push_back({drive, c2, c3, c4, sortKey, danger});
        }

        if (out.rows.empty()) {
            out.summary = ctx.Cancelled() ? L"已中止" : L"未发现可用驱动器";
            return;
        }
        wstring summary = NumU32((unsigned)out.rows.size()) + L" 个驱动器";
        if (usable > 0) {
            double usedPct = (double)(totalAll - freeAll) * 100.0 / (double)totalAll;
            summary += L" · 总容量 " + FormatSize(totalAll) + L" · 可用 " + FormatSize(freeAll) +
                       L"（已用 " + FmtPct(usedPct) + L"）";
        }
        if (ctx.Cancelled()) summary += L"（已中止）";
        out.summary = summary;
        if (dangerCount > 0) {
            out.hint = T(L"警告：") + dangerLetters + T(L" 使用率超过 90%，建议清理空间。");
        }
        ctx.Report(100, L"磁盘信息查询完成");
    }
};

// ===========================================================================
//  3) sys.process —— 进程管理（T03：列表 + 结束选中进程，黑名单硬拦截）
// ===========================================================================

// 进程保护黑名单：结束这些进程会导致系统崩溃/失稳（dev-plan §1.3）
bool IsProtectedProcess(DWORD pid, const wstring& name) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) return true;
    static const wchar_t* kBlocked[] = {
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"smss.exe",
        L"services.exe", L"lsass.exe", L"memcompression",
        L"system", L"system idle process",
    };
    wstring low = LowerW(name);
    for (auto b : kBlocked) {
        if (low == b) return true;
    }
    return false;
}

class SysProcessModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.process"; }
    const wchar_t* Group() const override { return L"系统管理"; }
    const wchar_t* Title() const override { return L"进程管理"; }
    const wchar_t* Description() const override { return L"进程列表与结束操作：PID、父进程、内存、线程数、启动时间、路径"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            out.summary = L"创建进程快照失败：" + LastErrW();
            return;
        }
        ctx.Report(-1, L"正在枚举进程…");

        // 第一遍：收集全部条目并建立 PID→进程名 映射（父进程反查用）
        vector<PROCESSENTRY32W> entries;
        std::map<DWORD, wstring> pid2name;
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                entries.push_back(pe);
                pid2name[pe.th32ProcessID] = pe.szExeFile;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        pidName_.clear();
        int scanned = 0, noMem = 0, protectedCount = 0;
        unsigned long long totalWs = 0;
        bool cancelled = ctx.Cancelled();

        for (auto& e : entries) {
            if (ctx.Cancelled()) { cancelled = true; break; }

            wstring name = e.szExeFile;
            DWORD pid = e.th32ProcessID;
            unsigned long long ws = 0;
            wstring memText = L"-";
            wstring timeText;
            wstring pathText;

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h && pid != 0) h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (h) {
                PROCESS_MEMORY_COUNTERS_EX pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                    ws = (unsigned long long)pmc.WorkingSetSize;
                    memText = FormatSize(ws);
                    totalWs += ws;
                } else {
                    ++noMem;
                }
                FILETIME ct = {}, et = {}, kt = {}, ut = {};
                if (GetProcessTimes(h, &ct, &et, &kt, &ut)) timeText = FmtFileTimeLocal(ct);
                wchar_t pb[32768] = {};
                DWORD pbsz = 32768;
                if (QueryFullProcessImageNameW(h, 0, pb, &pbsz)) pathText.assign(pb, pbsz);
                CloseHandle(h);
            } else {
                ++noMem;
            }

            wstring c4;
            if (!timeText.empty()) c4 = L"启动 " + timeText;
            if (!pathText.empty()) {
                if (!c4.empty()) c4 += L" · ";
                c4 += pathText;
            }
            if (c4.empty()) c4 = L"（权限不足，无法读取启动时间/路径）";

            wstring c3 = L"PID " + NumU32(pid) + L" · " + NumU32(e.cntThreads) + L" 线程";
            // 父进程名反查（快照内二次映射，PID 已退出时只显示编号）
            auto pit = pid2name.find(e.th32ParentProcessID);
            if (pit != pid2name.end())
                c3 += L" · 父 " + pit->second + L"(" + NumU32(e.th32ParentProcessID) + L")";
            else if (e.th32ParentProcessID != 0)
                c3 += L" · 父 PID " + NumU32(e.th32ParentProcessID);

            bool prot = IsProtectedProcess(pid, name);
            if (prot) ++protectedCount;

            pidName_[pid] = name;   // 供确认框/结束动作使用
            ModuleRow r;
            r.c1 = name; r.c2 = memText; r.c3 = c3; r.c4 = c4;
            r.sortKey = ws; r.danger = prot; r.tag = (int)pid;
            out.rows.push_back(r);
            ++scanned;
            if ((scanned % 40) == 0) ctx.Report(-1, L"已枚举 " + NumU32((unsigned)scanned) + L" 个进程…");
        }

        std::stable_sort(out.rows.begin(), out.rows.end(),
                         [](const ModuleRow& a, const ModuleRow& b) { return a.sortKey > b.sortKey; });

        wstring summary = (cancelled ? L"已中止：" : L"") + NumU32((unsigned)scanned) + L" 个进程";
        if (!out.rows.empty()) {
            unsigned long long maxWs = out.rows.front().sortKey;
            summary += L" · 工作集总计 " + FormatSize(totalWs);
            if (maxWs > 0) summary += L" · 最大 " + out.rows.front().c1 + L" " + FormatSize(maxWs);
        }
        if (noMem > 0) summary += L"（" + NumU32((unsigned)noMem) + L" 个进程因权限无法读取内存）";
        if (protectedCount > 0) summary += L" · 系统关键进程 " + NumU32((unsigned)protectedCount) + L" 个（红标，禁止结束）";
        out.summary = summary;
        if (!IsElevated()) out.hint = T(L"提示：标准用户权限下部分系统进程无法读取内存/路径。");
        out.canClean = true;
        ctx.Report(100, L"进程枚举完成");
    }

    // ---- 扩展动作：结束选中进程 ----
    vector<ModuleAction> Actions() const override {
        return { { L"结束选中进程", nullptr, true, 0xE711 } };   // E711 = Cancel
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)actionIndex;
        if (tags.empty()) return L"未选中任何进程。\n\n请先在列表中勾选要结束的进程。";
        wstring names;
        int protectedCount = 0;
        for (int tag : tags) {
            DWORD pid = (DWORD)tag;
            auto it = pidName_.find(pid);
            wstring nm = (it != pidName_.end()) ? it->second : (L"PID " + NumU32(pid));
            if (IsProtectedProcess(pid, nm)) ++protectedCount;
            names += nm + L"(PID " + NumU32(pid) + L")、";
        }
        if (!names.empty()) names.pop_back();
        wstring body = L"将结束 " + NumU32((unsigned)tags.size()) + L" 个进程：\n" + names +
                       L"\n\n结束进程不可恢复，进程内未保存的数据将丢失。";
        if (protectedCount > 0)
            body += L"\n\n注意：其中 " + NumU32((unsigned)protectedCount) +
                    L" 个为系统关键进程，操作时将被拒绝。";
        body += L"\n\n是否继续？";
        return body;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (selection_.empty()) {
            out.summary = L"未选中任何进程";
            out.canClean = true;
            return;
        }
        int ok = 0, refused = 0, fail = 0;
        for (int tag : selection_) {
            if (ctx.Cancelled()) break;
            DWORD pid = (DWORD)tag;
            wstring name;
            auto it = pidName_.find(pid);
            name = (it != pidName_.end()) ? it->second : (L"PID " + NumU32(pid));

            // 黑名单硬拦截（运行时二次验证，不依赖行数据是否过期）
            if (IsProtectedProcess(pid, name)) {
                ++refused;
                ctx.Log(L"  拒绝: " + name + L"(PID " + NumU32(pid) +
                        L") — 系统关键进程，禁止结束");
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pid);
            if (!h) {
                ++fail;
                ctx.Log(L"  失败: " + name + L"(PID " + NumU32(pid) + L") — " +
                        ErrorTextZh(GetLastError()));
                continue;
            }
            if (TerminateProcess(h, 1)) {
                WaitForSingleObject(h, 2000);   // 短等待确认退出
                ++ok;
                ctx.Log(L"  已结束: " + name + L"(PID " + NumU32(pid) + L")");
            } else {
                ++fail;
                ctx.Log(L"  失败: " + name + L"(PID " + NumU32(pid) + L") — " +
                        ErrorTextZh(GetLastError()));
            }
            CloseHandle(h);
        }
        wchar_t b[256];
        swprintf_s(b, L"结束进程完成：成功 %d，拒绝 %d，失败 %d", ok, refused, fail);
        out.summary = b;
        if (fail > 0 && !IsElevated())
            out.hint = T(L"部分进程结束失败，可能需要管理员权限（请提权后重试）。");
        out.canClean = true;
    }

private:
    vector<int> selection_;              // 选中行 tag（= PID）
    std::map<DWORD, wstring> pidName_;   // 上次扫描的 PID → 进程名
};

// ===========================================================================
//  4) sys.startup —— 启动项
// ===========================================================================

class SysStartupModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.startup"; }
    const wchar_t* Group() const override { return L"系统信息"; }
    const wchar_t* Title() const override { return L"启动项"; }
    const wchar_t* Description() const override { return L"注册表 Run/RunOnce/策略 Run 与用户/公共启动文件夹中的自启动程序"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"正在读取启动项…");

        int regCount = 0, folderCount = 0;

        auto scanKey = [&](HKEY root, const wchar_t* sub, const wchar_t* where, const wchar_t* scope) {
            if (ctx.Cancelled()) return;
            vector<RegValue> vals;
            if (!RegEnumStringValues(root, sub, vals)) return;
            for (auto& v : vals) {
                wstring name = v.name.empty() ? L"(默认)" : v.name;
                out.rows.push_back({name, where, scope, v.data, 0, false});
                ++regCount;
            }
        };

        const wchar_t* kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        const wchar_t* kRunOnce = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        const wchar_t* kPolicyRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run";
        const wchar_t* kRunWow = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";

        // 用户（HKCU）
        scanKey(HKEY_CURRENT_USER, kRun, L"注册表 Run", L"用户");
        scanKey(HKEY_CURRENT_USER, kRunOnce, L"注册表 RunOnce", L"用户");
        scanKey(HKEY_CURRENT_USER, kPolicyRun, L"策略 Run", L"用户");
        // 系统（HKLM）
        scanKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"注册表 Run", L"系统");
        scanKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"注册表 RunOnce", L"系统");
        scanKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", L"策略 Run", L"系统");
        // 32 位视图
        scanKey(HKEY_LOCAL_MACHINE, kRunWow, L"注册表 Run (32位)", L"系统");
        scanKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                L"注册表 RunOnce (32位)", L"系统");

        // 启动文件夹
        auto scanDir = [&](const wstring& dir, const wchar_t* scope) {
            if (dir.empty() || ctx.Cancelled()) return;
            wstring pattern = dir + L"\\*";
            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;  // 文件夹配置，不是启动项
                out.rows.push_back({fd.cFileName, L"启动文件夹", scope, dir + L"\\" + fd.cFileName, 0, false});
                ++folderCount;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        };

        wstring appData = GetEnvW(L"APPDATA");
        if (!appData.empty()) {
            scanDir(appData + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", L"用户");
        }
        wstring programData = GetEnvW(L"ProgramData");
        if (!programData.empty()) {
            scanDir(programData + L"\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp", L"公共");
        }

        if (out.rows.empty()) {
            out.summary = ctx.Cancelled() ? L"已中止" : L"未发现启动项（注册表 Run 系列与启动文件夹均为空）";
            return;
        }
        wstring summary = NumU32((unsigned)out.rows.size()) + L" 个启动项（注册表 " +
                          NumU32((unsigned)regCount) + L" · 启动文件夹 " + NumU32((unsigned)folderCount) + L"）";
        if (ctx.Cancelled()) summary += L"（已中止）";
        out.summary = summary;
        out.hint = T(L"提示：RunOnce 项在用户登录后执行一次即被删除；启动文件夹中的 .lnk 按文件名展示。");
        ctx.Report(100, L"启动项读取完成");
    }
};

// ===========================================================================
//  5) sys.services —— 服务管理（T03：列表 + 启停/暂停/继续/重启/启动类型）
// ===========================================================================

wstring ServiceStateText(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING:        return L"运行中";
    case SERVICE_STOPPED:        return L"已停止";
    case SERVICE_PAUSED:         return L"已暂停";
    case SERVICE_START_PENDING:  return L"启动中";
    case SERVICE_STOP_PENDING:   return L"停止中";
    case SERVICE_CONTINUE_PENDING: return L"恢复中";
    case SERVICE_PAUSE_PENDING:  return L"暂停中";
    default: return L"未知";
    }
}

wstring ServiceStartText(DWORD type, bool delayed) {
    switch (type) {
    case SERVICE_BOOT_START:   return L"引导";
    case SERVICE_SYSTEM_START: return L"系统";
    case SERVICE_AUTO_START:   return delayed ? L"自动(延迟)" : L"自动";
    case SERVICE_DEMAND_START: return L"手动";
    case SERVICE_DISABLED:     return L"禁用";
    default: return L"未知";
    }
}

// 轮询服务进入目标状态（≤10s），finalState 返回最后一次读到的状态
bool PollServiceState(SC_HANDLE svc, DWORD target, DWORD* finalState) {
    for (int i = 0; i < 100; ++i) {
        SERVICE_STATUS st = {};
        if (!QueryServiceStatus(svc, &st)) return false;
        *finalState = st.dwCurrentState;
        if (st.dwCurrentState == target) return true;
        Sleep(100);
    }
    return false;
}

// 读取当前启动类型与延迟自动标志（读不到返回 false，startType 置 0xFFFFFFFF）
bool QueryServiceStartType(SC_HANDLE svc, DWORD* startType, bool* delayed) {
    *startType = 0xFFFFFFFF;
    *delayed = false;
    DWORD needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    vector<BYTE> cfg(needed ? needed : 8192);
    if (!QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)cfg.data(),
                             (DWORD)cfg.size(), &needed))
        return false;
    auto* q = (LPQUERY_SERVICE_CONFIGW)cfg.data();
    *startType = q->dwStartType;
    if (*startType == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO dsi = {};
        DWORD db = 0;
        if (QueryServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                 (LPBYTE)&dsi, sizeof(dsi), &db) && dsi.fDelayedAutostart)
            *delayed = true;
    }
    return true;
}

class SysServicesModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.services"; }
    const wchar_t* Group() const override { return L"系统管理"; }
    const wchar_t* Title() const override { return L"服务管理"; }
    const wchar_t* Description() const override { return L"服务列表与操作：启停/暂停/继续/重启/启动类型调整"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        names_.clear();
        dispNames_.clear();
        // T03：SC_MANAGER_CONNECT（启停控制要用）+ ENUMERATE（枚举要用）
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                       SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
        if (!scm) {
            out.summary = L"无法打开服务控制管理器：" + LastErrW() + L"（可能需要管理员权限）";
            return;
        }
        ctx.Report(-1, L"正在枚举服务…");

        int total = 0, running = 0, stopped = 0, other = 0, autoStart = 0, disabled = 0, cfgUnknown = 0;
        bool cancelled = false;
        bool fatal = false;
        wstring fatalMsg;

        // 服务数据量约 100~300KB，初始给 256KB，必要时按需扩容
        vector<BYTE> buf(256 * 1024);
        DWORD needed = 0, returned = 0, resume = 0;

        for (int guard = 0; guard < 64; ++guard) {
            if (ctx.Cancelled()) { cancelled = true; break; }

            DWORD bufSize = (DWORD)buf.size();
            BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                            buf.data(), bufSize, &needed, &returned, &resume, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_MORE_DATA && needed > buf.size()) {
                    buf.resize(needed);
                    continue;
                }
                if (err == ERROR_MORE_DATA) {
                    // 已返回部分数据，继续处理
                } else {
                    fatal = true;
                    fatalMsg = L"枚举服务失败：" + Win32Err(err);
                    break;
                }
            }

            auto* arr = (LPENUM_SERVICE_STATUS_PROCESSW)buf.data();
            for (DWORD i = 0; i < returned; ++i) {
                if (ctx.Cancelled()) { cancelled = true; break; }
                auto& sp = arr[i];
                wstring name = sp.lpServiceName ? sp.lpServiceName : L"";
                wstring display = sp.lpDisplayName ? sp.lpDisplayName : name;
                if (display.empty()) display = name;
                DWORD state = sp.ServiceStatusProcess.dwCurrentState;
                DWORD pid = sp.ServiceStatusProcess.dwProcessId;

                DWORD startType = 0xFFFFFFFF;
                bool delayed = false;
                wstring account;
                SC_HANDLE sh = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG);
                if (sh) {
                    bool dummyDelay = false;
                    if (QueryServiceStartType(sh, &startType, &dummyDelay)) {
                        delayed = dummyDelay;
                        // 账户信息单独再查一次配置（同一缓冲）
                        DWORD aNeeded = 0;
                        QueryServiceConfigW(sh, nullptr, 0, &aNeeded);
                        vector<BYTE> acfg(aNeeded ? aNeeded : 8192);
                        if (QueryServiceConfigW(sh, (LPQUERY_SERVICE_CONFIGW)acfg.data(),
                                                (DWORD)acfg.size(), &aNeeded)) {
                            auto* q = (LPQUERY_SERVICE_CONFIGW)acfg.data();
                            if (q->lpServiceStartName) account = q->lpServiceStartName;
                        }
                    }
                    CloseServiceHandle(sh);
                }
                if (startType == 0xFFFFFFFF) ++cfgUnknown;

                wstring stateText = ServiceStateText(state);
                wstring startText = ServiceStartText(startType, delayed);
                wstring c4 = name;
                if (pid != 0) c4 += L" · PID " + NumU32(pid);
                if (!account.empty()) c4 += L" · " + account;

                // 运行中的排前面（sortKey 越大越靠前）
                unsigned long long rank = 0;
                if (state == SERVICE_RUNNING) rank = 4;
                else if (state == SERVICE_START_PENDING || state == SERVICE_CONTINUE_PENDING) rank = 3;
                else if (state == SERVICE_PAUSED) rank = 2;
                else if (state == SERVICE_STOPPED) rank = 1;

                // tag = names_ 下标（排序安全回溯）
                ModuleRow r;
                r.c1 = display; r.c2 = stateText; r.c3 = startText; r.c4 = c4;
                r.sortKey = rank; r.danger = false;
                r.tag = (int)names_.size();
                out.rows.push_back(r);
                names_.push_back(name);
                dispNames_.push_back(display);

                ++total;
                if (state == SERVICE_RUNNING) ++running;
                else if (state == SERVICE_STOPPED) ++stopped;
                else ++other;
                if (startType == SERVICE_AUTO_START) ++autoStart;
                if (startType == SERVICE_DISABLED) ++disabled;

                if ((total % 50) == 0) ctx.Report(-1, L"已读取 " + NumU32((unsigned)total) + L" 个服务…");
            }

            if (cancelled || fatal) break;
            if (resume == 0 || returned == 0) break;
        }
        CloseServiceHandle(scm);

        std::stable_sort(out.rows.begin(), out.rows.end(),
                         [](const ModuleRow& a, const ModuleRow& b) { return a.sortKey > b.sortKey; });

        if (fatal) {
            out.summary = fatalMsg;
            return;
        }
        wstring summary = (cancelled ? L"已中止：" : L"") + NumU32((unsigned)total) + L" 个服务";
        summary += L" · 运行 " + NumU32((unsigned)running) + L" · 停止 " + NumU32((unsigned)stopped);
        if (other) summary += L" · 其他 " + NumU32((unsigned)other);
        summary += L" · 自动 " + NumU32((unsigned)autoStart) + L" · 禁用 " + NumU32((unsigned)disabled);
        if (cfgUnknown) summary += L"（" + NumU32((unsigned)cfgUnknown) + L" 个无法读取启动类型）";
        out.summary = summary;
        out.hint = T(L"说明：微软核心服务受 SCM 保护，拒绝操作属正常现象，失败原因会以中文展示。");
        out.canClean = true;
        ctx.Report(100, L"服务枚举完成");
    }

    // ---- 扩展动作（T03）：0 启动 / 1 停止 / 2..8 收进「更多 ▾」菜单 ----
    vector<ModuleAction> Actions() const override {
        return {
            { L"启动选中服务", nullptr, false, 0xE768 },
            { L"停止选中服务", L"停止服务后，依赖它的功能可能暂时不可用。\n\n是否继续？", false, 0xE71A },
            { L"暂停选中服务", nullptr, false, 0xE769 },
            { L"继续选中服务", nullptr, false, 0xE768 },
            { L"重启选中服务", nullptr, false, 0xE72C },
            { L"设为自动启动", nullptr, false, 0 },
            { L"设为自动(延迟)", nullptr, false, 0 },
            { L"设为手动启动", nullptr, false, 0 },
            { L"设为禁用", L"禁用后服务将无法启动，相关功能失效（可随时改回其他启动类型）。\n\n是否继续？", true, 0 },
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        if (tags.empty()) return L"未选中任何服务。\n\n请先在列表中勾选要操作的服务。";
        wstring names;
        for (int t : tags) {
            wstring n = (t >= 0 && t < (int)dispNames_.size())
                            ? dispNames_[t] : (L"内部名索引 " + NumU32((unsigned)t));
            names += n + L"、";
        }
        if (!names.empty()) names.pop_back();
        wstring body = L"将对 " + NumU32((unsigned)tags.size()) + L" 个服务执行该操作：\n" + names;
        // 危险动作追加 ModuleAction.warning 静态警示
        auto acts = Actions();
        if (actionIndex >= 0 && actionIndex < (int)acts.size() && acts[actionIndex].warning)
            body += L"\n\n" + wstring(acts[actionIndex].warning);
        else
            body += L"\n\n是否继续？";
        return body;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex < 0 || actionIndex > 8) return;
        if (selection_.empty()) {
            out.summary = L"未选中任何服务";
            out.canClean = true;
            return;
        }
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            out.summary = L"无法打开服务控制管理器：" + LastErrW();
            return;
        }
        int ok = 0, skip = 0, fail = 0;
        for (int tag : selection_) {
            if (ctx.Cancelled()) break;
            if (tag < 0 || tag >= (int)names_.size()) continue;
            const wstring& name = names_[tag];
            const wstring& disp = dispNames_[tag];
            switch (actionIndex) {
            case 0: StartOne(ctx, scm, name, disp, ok, skip, fail); break;
            case 1: StopOne(ctx, scm, name, disp, ok, skip, fail); break;
            case 2: ControlOne(ctx, scm, name, disp, SERVICE_CONTROL_PAUSE,
                               SERVICE_PAUSED, L"已暂停", ok, skip, fail); break;
            case 3: ControlOne(ctx, scm, name, disp, SERVICE_CONTROL_CONTINUE,
                               SERVICE_RUNNING, L"已继续", ok, skip, fail); break;
            case 4: RestartOne(ctx, scm, name, disp, ok, skip, fail); break;
            case 5: SetStartTypeOne(ctx, scm, name, disp, SERVICE_AUTO_START, false, ok, skip, fail); break;
            case 6: SetStartTypeOne(ctx, scm, name, disp, SERVICE_AUTO_START, true, ok, skip, fail); break;
            case 7: SetStartTypeOne(ctx, scm, name, disp, SERVICE_DEMAND_START, false, ok, skip, fail); break;
            case 8: SetStartTypeOne(ctx, scm, name, disp, SERVICE_DISABLED, false, ok, skip, fail); break;
            }
        }
        CloseServiceHandle(scm);
        static const wchar_t* kNames[] = {
            L"启动服务", L"停止服务", L"暂停服务", L"继续服务", L"重启服务",
            L"设为自动", L"设为自动(延迟)", L"设为手动", L"设为禁用",
        };
        wchar_t b[256];
        swprintf_s(b, L"%s完成：成功 %d，跳过 %d，失败 %d", kNames[actionIndex], ok, skip, fail);
        out.summary = b;
        if (fail > 0 && !IsElevated())
            out.hint = T(L"部分操作失败，可能需要管理员权限（请提权后重试）。");
        out.canClean = true;
    }

private:
    vector<int> selection_;
    vector<wstring> names_;      // tag → 内部服务名
    vector<wstring> dispNames_;  // tag → 显示名

    void StartOne(ModuleContext& ctx, SC_HANDLE scm, const wstring& name,
                  const wstring& disp, int& ok, int& skip, int& fail) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(),
                                     SERVICE_START | SERVICE_QUERY_STATUS);
        if (!svc) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            return;
        }
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
            ++skip;
            ctx.Log(L"  跳过: " + disp + L" — 已在运行");
            CloseServiceHandle(svc);
            return;
        }
        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_ALREADY_RUNNING) {
                ++skip;
                ctx.Log(L"  跳过: " + disp + L" — 已在运行");
            } else {
                ++fail;
                ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(e));
            }
            CloseServiceHandle(svc);
            return;
        }
        DWORD finalState = 0;
        if (PollServiceState(svc, SERVICE_RUNNING, &finalState)) {
            ++ok;
            ctx.Log(L"  已启动: " + disp);
        } else {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — 启动未完成（当前状态：" +
                    ServiceStateText(finalState) + L"）");
        }
        CloseServiceHandle(svc);
    }

    void StopOne(ModuleContext& ctx, SC_HANDLE scm, const wstring& name,
                 const wstring& disp, int& ok, int& skip, int& fail) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(),
                                     SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!svc) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            return;
        }
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED) {
            ++skip;
            ctx.Log(L"  跳过: " + disp + L" — 已停止");
            CloseServiceHandle(svc);
            return;
        }
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_NOT_ACTIVE) {
                ++skip;
                ctx.Log(L"  跳过: " + disp + L" — 服务未在运行");
            } else {
                ++fail;
                ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(e));
            }
            CloseServiceHandle(svc);
            return;
        }
        DWORD finalState = 0;
        if (PollServiceState(svc, SERVICE_STOPPED, &finalState)) {
            ++ok;
            ctx.Log(L"  已停止: " + disp);
        } else {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — 停止未完成（当前状态：" +
                    ServiceStateText(finalState) + L"）");
        }
        CloseServiceHandle(svc);
    }

    // 暂停 / 继续（统一 ControlService + 轮询目标状态）
    void ControlOne(ModuleContext& ctx, SC_HANDLE scm, const wstring& name,
                    const wstring& disp, DWORD control, DWORD targetState,
                    const wchar_t* doneText, int& ok, int& skip, int& fail) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(),
                                     SERVICE_PAUSE_CONTINUE | SERVICE_QUERY_STATUS);
        if (!svc) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            return;
        }
        SERVICE_STATUS st = {};
        if (!ControlService(svc, control, &st)) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            CloseServiceHandle(svc);
            return;
        }
        DWORD finalState = 0;
        if (PollServiceState(svc, targetState, &finalState)) {
            ++ok;
            ctx.Log(wstring(L"  ") + doneText + L": " + disp);
        } else {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — 操作未完成（当前状态：" +
                    ServiceStateText(finalState) + L"）");
        }
        CloseServiceHandle(svc);
    }

    void RestartOne(ModuleContext& ctx, SC_HANDLE scm, const wstring& name,
                    const wstring& disp, int& ok, int& skip, int& fail) {
        int sOk = 0, sSkip = 0, sFail = 0;
        StopOne(ctx, scm, name, disp, sOk, sSkip, sFail);
        if (sFail > 0) {
            ++fail;
            return;
        }
        StartOne(ctx, scm, name, disp, ok, skip, fail);   // 成功/跳过都计入本动作
    }

    void SetStartTypeOne(ModuleContext& ctx, SC_HANDLE scm, const wstring& name,
                         const wstring& disp, DWORD startType, bool delayed,
                         int& ok, int& skip, int& fail) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(),
                                     SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
        if (!svc) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            return;
        }
        // 已是目标状态则跳过（延迟标志参与比较）
        DWORD curType = 0xFFFFFFFF;
        bool curDelayed = false;
        wstring wantText = ServiceStartText(startType, delayed);
        if (QueryServiceStartType(svc, &curType, &curDelayed) &&
            curType == startType && (startType != SERVICE_AUTO_START || curDelayed == delayed)) {
            ++skip;
            ctx.Log(L"  跳过: " + disp + L" — 已是「" + wantText + L"」");
            CloseServiceHandle(svc);
            return;
        }
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
                                  nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr)) {
            ++fail;
            ctx.Log(L"  失败: " + disp + L" — " + ErrorTextZh(GetLastError()));
            CloseServiceHandle(svc);
            return;
        }
        // 自动启动类：同步延迟自动标志（非自动类型延迟标志无意义）
        if (startType == SERVICE_AUTO_START) {
            SERVICE_DELAYED_AUTO_START_INFO di = {};
            di.fDelayedAutostart = delayed ? TRUE : FALSE;
            if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (LPBYTE)&di))
                ctx.Log(L"  提示: " + disp + L" 延迟标志设置失败：" + ErrorTextZh(GetLastError()));
        }
        ++ok;
        ctx.Log(L"  已设为「" + wantText + L"」: " + disp);
        CloseServiceHandle(svc);
    }
};

// ===========================================================================
//  6) sys.hotfix —— 已安装补丁（CBS 组件包解析，真实 KB / 安装时间）
// ===========================================================================

// 从文本中提取 KB 号（KB + 6~7 位数字）
void CollectKBs(const wstring& text, vector<wstring>& out) {
    const size_t n = text.size();
    for (size_t i = 0; i + 3 < n; ++i) {
        wchar_t c0 = text[i], c1 = text[i + 1];
        if ((c0 != L'K' && c0 != L'k') || (c1 != L'B' && c1 != L'b')) continue;
        size_t j = i + 2, digits = 0;
        while (j < n && text[j] >= L'0' && text[j] <= L'9') { ++j; ++digits; }
        if (digits >= 6 && digits <= 7 && (j >= n || text[j] < L'0' || text[j] > L'9')) {
            wstring kb = L"KB" + text.substr(i + 2, digits);
            bool dup = false;
            for (auto& e : out) { if (e == kb) { dup = true; break; } }
            if (!dup) out.push_back(kb);
        }
        i = (j > i) ? (j - 1) : i;
    }
}

// 按包名判断该包在补丁列表中的“代表性”（同一 KB 会出现在大量组件包中）
int PkgScore(const wstring& name) {
    wstring low = LowerW(name);
    if (low.find(L"package_for_rollupfix") != wstring::npos) return 100;
    if (low.find(L"package_for_dotnetrollup") != wstring::npos) return 90;
    if (low.find(L"package_for_servicingstack") != wstring::npos) return 85;
    if (low.find(L"package_for_kb") != wstring::npos) return 80;
    if (low.find(L"rollupfix") != wstring::npos) return 60;
    return 0;
}

wstring ReleaseText(const wstring& release, const wstring& pkg, const wstring& inst) {
    wstring rel = TrimW(release);
    wstring low = LowerW(pkg + L" " + inst);

    // 先从包名推断类型（ReleaseType 往往只是笼统的 "Update"）
    auto fromName = [&]() -> wstring {
        if (low.find(L"servicingstack") != wstring::npos) return L"服务堆栈更新";
        if (low.find(L"dotnet") != wstring::npos) return L".NET 更新";
        if (low.find(L"rollup") != wstring::npos) return L"累计更新";
        if (low.find(L"defender") != wstring::npos) return L"安全智能更新";
        if (low.find(L"security") != wstring::npos) return L"安全更新";
        return L"";
    };

    if (!rel.empty() && rel != L"Update") {
        if (rel == L"Security Update") return L"安全更新";
        if (rel == L"Feature Pack") return L"功能包";
        if (rel == L"Service Pack") return L"服务包";
        if (rel == L"Hotfix") return L"修补程序";
        return rel;
    }
    wstring byName = fromName();
    if (!byName.empty()) return byName;
    return L"更新";
}

class SysHotfixModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.hotfix"; }
    const wchar_t* Group() const override { return L"系统信息"; }
    const wchar_t* Title() const override { return L"已安装补丁"; }
    const wchar_t* Description() const override { return L"从 CBS 组件包解析近期安装的 Windows 更新（KB 号、安装时间、类型）"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        SysVer v = QuerySysVer();

        const wchar_t* kPkgs =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\Packages";
        HKEY hk = nullptr;
        LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPkgs, 0, KEY_READ, &hk);
        if (r != ERROR_SUCCESS) {
            out.summary = L"无法打开 CBS 包注册表：" + Win32Err(r);
            out.hint = T(L"系统补丁级别：") + v.BuildText() + L"（" + v.NameText() + L"）";
            return;
        }
        ctx.Report(-1, L"正在枚举 CBS 组件包…");

        DWORD subKeys = 0, maxSubKeyLen = 0;
        RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr, &subKeys, &maxSubKeyLen,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

        struct Hit {
            wstring kb, pkg, instName, release;
            unsigned long long time = 0;
            int score = 0;
        };
        vector<Hit> hits;
        wstring nameBuf((size_t)maxSubKeyLen + 4, L'\0');
        DWORD scanned = 0;
        bool cancelled = false;

        for (DWORD i = 0; i < subKeys; ++i) {
            if (ctx.Cancelled()) { cancelled = true; break; }

            DWORD nlen = (DWORD)nameBuf.size();
            LONG er = RegEnumKeyExW(hk, i, nameBuf.data(), &nlen, nullptr, nullptr, nullptr, nullptr);
            if (er != ERROR_SUCCESS) continue;
            wstring sub(nameBuf.data(), nlen);

            HKEY pk = nullptr;
            if (RegOpenKeyExW(hk, sub.c_str(), 0, KEY_READ, &pk) != ERROR_SUCCESS) continue;

            wstring loc, inst, rel;
            KeyQueryString(pk, L"InstallLocation", loc);
            KeyQueryString(pk, L"InstallName", inst);
            KeyQueryString(pk, L"ReleaseType", rel);

            wstring blob = sub + L" " + loc + L" " + inst;
            vector<wstring> kbs;
            CollectKBs(blob, kbs);

            if (!kbs.empty()) {
                DWORD hi = 0, lo = 0;
                bool hasTime = KeyQueryDword(pk, L"InstallTimeHigh", hi) &&
                               KeyQueryDword(pk, L"InstallTimeLow", lo);
                unsigned long long t = hasTime ? (((unsigned long long)hi << 32) | lo) : 0;
                int score = PkgScore(sub);
                for (auto& kb : kbs) {
                    bool found = false;
                    for (auto& h : hits) {
                        if (h.kb == kb) {
                            found = true;
                            if (score > h.score) {
                                // 用更能代表该 KB 的包（RollupFix 等）替换组件包
                                if (t > h.time) h.time = t;
                                h.pkg = sub;
                                h.instName = inst;
                                h.release = rel;
                                h.score = score;
                            } else if (score == h.score && t > h.time) {
                                h.time = t;
                                h.pkg = sub;
                                h.instName = inst;
                                h.release = rel;
                            }
                            break;
                        }
                    }
                    if (!found) hits.push_back({kb, sub, inst, rel, t, score});
                }
            }
            RegCloseKey(pk);
            ++scanned;
            if ((scanned % 250) == 0) {
                int pct = subKeys ? (int)((unsigned long long)scanned * 100 / subKeys) : 0;
                ctx.Report(pct, L"已扫描 " + NumU32(scanned) + L" / " + NumU32(subKeys) + L" 个 CBS 包…");
            }
        }
        RegCloseKey(hk);

        if (cancelled) {
            out.summary = L"已中止：已扫描 " + NumU32(scanned) + L" 个 CBS 包";
            out.hint = T(L"系统补丁级别：") + v.BuildText();
            return;
        }
        if (hits.empty()) {
            out.summary = L"未从 " + NumU32(subKeys) + L" 个 CBS 包中解析到已安装更新（可能权限不足或系统使用其他更新机制）";
            out.hint = T(L"系统补丁级别：") + v.BuildText() + L"（" + v.NameText() + L"）";
            return;
        }

        std::stable_sort(hits.begin(), hits.end(),
                         [](const Hit& a, const Hit& b) { return a.time > b.time; });
        size_t show = (std::min)(hits.size(), (size_t)10);
        for (size_t i = 0; i < show; ++i) {
            const Hit& h = hits[i];
            wstring timeText = h.time ? FmtFileTimeLocalTicks(h.time) : L"未知";
            wstring typeText = ReleaseText(h.release, h.pkg, h.instName);
            out.rows.push_back({h.kb, timeText, typeText, h.pkg, h.time, false});
        }

        out.summary = L"扫描 " + NumU32(subKeys) + L" 个 CBS 包，解析出 " +
                      NumU32((unsigned)hits.size()) + L" 个已安装更新（显示最近 " +
                      NumU32((unsigned)show) + L" 个）";
        out.hint = T(L"系统补丁级别：") + v.BuildText() + L"（" + v.NameText() +
                   T(L"） · 数据来源：Component Based Servicing\\Packages（只读）");
        ctx.Report(100, L"CBS 包扫描完成");
    }
};

// ===========================================================================
//  7) sys.env —— 环境变量
// ===========================================================================

wstring RegTypeText(DWORD type) {
    switch (type) {
    case REG_SZ:        return L"REG_SZ";
    case REG_EXPAND_SZ: return L"REG_EXPAND_SZ";
    case REG_MULTI_SZ:  return L"REG_MULTI_SZ";
    default:            return L"类型 " + NumU32(type);
    }
}

class SysEnvModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.env"; }
    const wchar_t* Group() const override { return L"系统信息"; }
    const wchar_t* Title() const override { return L"环境变量"; }
    const wchar_t* Description() const override { return L"系统（HKLM）与用户（HKCU）持久化环境变量，含类型与展开值"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"正在读取环境变量…");

        vector<RegValue> sysVals, userVals;
        bool okSys = RegEnumStringValues(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", sysVals);
        bool okUser = RegEnumStringValues(HKEY_CURRENT_USER, L"Environment", userVals);

        if (!okSys && !okUser) {
            out.summary = L"无法读取系统/用户环境变量（注册表访问被拒绝）";
            return;
        }

        auto byName = [](const RegValue& a, const RegValue& b) { return a.name < b.name; };
        std::stable_sort(sysVals.begin(), sysVals.end(), byName);
        std::stable_sort(userVals.begin(), userVals.end(), byName);

        auto addRows = [&](const vector<RegValue>& vals, const wchar_t* scope, unsigned long long rank) {
            for (auto& v : vals) {
                if (ctx.Cancelled()) return;
                wstring value = v.data;
                if (v.type == REG_EXPAND_SZ) {
                    wstring expanded = ExpandEnvW(v.data);
                    if (expanded != v.data) value = v.data + L"  →  " + expanded;
                }
                wstring c4 = RegTypeText(v.type);
                out.rows.push_back({v.name.empty() ? L"(默认)" : v.name, value, scope, c4, rank, false});
            }
        };

        addRows(sysVals, L"系统", 2);
        addRows(userVals, L"用户", 1);

        wstring summary = NumU32((unsigned)out.rows.size()) + L" 个环境变量（系统 " +
                          NumU32((unsigned)sysVals.size()) + L" · 用户 " + NumU32((unsigned)userVals.size()) + L"）";
        if (ctx.Cancelled()) summary += L"（已中止）";
        out.summary = summary;
        out.hint = T(L"说明：仅列出注册表中持久化的环境变量；REG_EXPAND_SZ 值同时显示展开后结果。");
        ctx.Report(100, L"环境变量读取完成");
    }
};

// ===========================================================================
//  8) sys.hosts —— hosts 文件（只读解析，绝不写文件）
// ===========================================================================

// 字节流 → wstring：识别 UTF-16LE/BE BOM，否则优先 UTF-8，失败回退系统 ANSI
wstring DecodeHostsBytes(const std::string& raw) {
    const unsigned char* p = (const unsigned char*)raw.data();
    size_t n = raw.size();
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        size_t cnt = (n - 2) / 2;
        wstring out(cnt, L'\0');
        if (cnt) memcpy(out.data(), raw.data() + 2, cnt * 2);
        return out;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        size_t cnt = (n - 2) / 2;
        wstring out(cnt, L'\0');
        for (size_t i = 0; i < cnt; ++i) {
            unsigned char lo = p[2 + i * 2], hi = p[3 + i * 2];
            out[i] = (wchar_t)((lo << 8) | hi);
        }
        return out;
    }
    size_t off = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) off = 3;
    const char* src = raw.data() + off;
    int srcLen = (int)(n - off);
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, srcLen, nullptr, 0);
    if (wn > 0) {
        wstring out(wn, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, srcLen, out.data(), wn);
        return out;
    }
    wn = MultiByteToWideChar(CP_ACP, 0, src, srcLen, nullptr, 0);
    if (wn <= 0) return L"";
    wstring out(wn, L'\0');
    MultiByteToWideChar(CP_ACP, 0, src, srcLen, out.data(), wn);
    return out;
}

bool LooksLikeIpW(const wstring& tok) {
    if (tok.size() < 3) return false;
    bool hasSep = false;
    for (wchar_t c : tok) {
        if (c == L'.' || c == L':') { hasSep = true; continue; }
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'%') continue;
        return false;
    }
    return hasSep;
}

struct HostsStats {
    int mappings = 0;
    int comments = 0;
    int invalid = 0;
    int blank = 0;
};

// 解析 hosts 文本：逐行分类为「本地映射 / 注释 / 无法解析 / 空行」
void ParseHostsText(const wstring& text, vector<ModuleRow>& rows, HostsStats& st,
                    const CancelFn& cancel = nullptr) {
    size_t pos = 0;
    while (pos <= text.size()) {
        if (cancel && cancel()) break;
        size_t nl = text.find(L'\n', pos);
        wstring line = (nl == wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == wstring::npos) ? text.size() + 1 : nl + 1;

        if (!line.empty() && line.back() == L'\r') line.pop_back();
        wstring trimmed = TrimW(line);
        if (trimmed.empty()) { ++st.blank; continue; }

        if (trimmed[0] == L'#') {
            rows.push_back({L"", L"", L"注释", CapW(trimmed, 400), 0, false});
            ++st.comments;
            continue;
        }

        // 行内 " # ..." 属于注释，不参与主机名解析（原始整行仍保留在 c4）
        wstring content = trimmed;
        size_t hash = content.find(L'#');
        if (hash != wstring::npos) content = TrimW(content.substr(0, hash));

        // 按空白切分：第一个 token 是 IP，其余是主机名
        vector<wstring> toks;
        size_t i = 0;
        while (i < content.size()) {
            while (i < content.size() && (content[i] == L' ' || content[i] == L'\t')) ++i;
            size_t start = i;
            while (i < content.size() && content[i] != L' ' && content[i] != L'\t') ++i;
            if (i > start) toks.push_back(content.substr(start, i - start));
        }
        if (toks.size() < 2) {
            ++st.invalid;
            if (!toks.empty()) rows.push_back({CapW(toks[0], 60), L"", L"无法解析", CapW(trimmed, 400), 0, false});
            continue;
        }
        if (LooksLikeIpW(toks[0])) {
            wstring hosts;
            for (size_t k = 1; k < toks.size(); ++k) {
                if (!hosts.empty()) hosts += L" ";
                hosts += toks[k];
            }
            rows.push_back({toks[0], hosts, L"本地映射", CapW(trimmed, 400), 0, false});
            ++st.mappings;
        } else {
            rows.push_back({CapW(toks[0], 60), L"", L"无法解析", CapW(trimmed, 400), 0, false});
            ++st.invalid;
        }
    }
}

// ---------------------------------------------------------------- hosts 写入
//  v2.15：只读解析升级为图形化编辑。写流程：
//    检测 FILE_ATTRIBUTE_READONLY → 清除 → 备份 hosts.bak → 写入 →
//    原来是只读则设回。编码保持原样（UTF-16 BOM 则写 UTF-16LE，
//    否则统一 UTF-8 无 BOM）；非提权写 System32\drivers\etc\hosts
//    失败时如实报错并提示需管理员。

wstring HostsPath2() {
    wchar_t wd[1024] = {};
    UINT n = GetWindowsDirectoryW(wd, 1024);
    return ((n > 0 && n < 1024) ? wstring(wd, n) : wstring(L"C:\\Windows"))
           + L"\\System32\\drivers\\etc\\hosts";
}

// 读 hosts 全文（原始字节 → DecodeHostsBytes 智能解码）
bool ReadHostsFull(const wstring& path, wstring* text, wstring* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法读取 hosts 文件：" + path + L"（" + LastErrW() + L"）";
        return false;
    }
    std::string raw;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
    CloseHandle(h);
    if (text) *text = DecodeHostsBytes(raw);
    return true;
}

// 原文件是否 UTF-16LE BOM（写回时保持原编码用）
bool HostsWasUtf16(const wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char bom[2] = { 0, 0 };
    DWORD got = 0;
    ReadFile(h, bom, 2, &got, nullptr);
    CloseHandle(h);
    return got == 2 && bom[0] == 0xFF && bom[1] == 0xFE;
}

// 把文本写入 hosts（保持原编码；完整只读/备份流程）。成功返回空串，失败返回原因。
wstring WriteHostsFull(const wstring& path, const wstring& text) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return L"无法访问 hosts：" + path + L"（" + ErrorTextZh(GetLastError()) + L"）";
    bool wasRo = (attr & FILE_ATTRIBUTE_READONLY) != 0;

    // ① 写前备份 hosts.bak（同目录）
    wstring bak = path + L".bak";
    if (!CopyFileW(path.c_str(), bak.c_str(), FALSE))
        return L"备份 hosts.bak 失败（" + ErrorTextZh(GetLastError()) +
               L"），已中止写入";

    // ② 清除只读
    if (wasRo) SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

    // ③ 写入（保持原编码）
    bool utf16 = HostsWasUtf16(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();   // 先存变量：ErrorTextZh 内部可能污染 LastError
        if (wasRo) SetFileAttributesW(path.c_str(), attr);
        wstring msg = L"写入 hosts 失败（" + ErrorTextZh(e) + L"）";
        if (e == ERROR_ACCESS_DENIED)
            msg += L"——需要管理员权限，请以管理员身份运行后重试";
        return msg;
    }
    std::string bytes;
    if (utf16) {
        bytes.push_back((char)0xFF);
        bytes.push_back((char)0xFE);
        const wstring& t = text;
        bytes.append((const char*)t.c_str(), t.size() * sizeof(wchar_t));
    } else {
        bytes = WToUtf8(text);   // UTF-8 无 BOM（hosts 主流形态）
    }
    DWORD w = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    CloseHandle(h);

    // ④ 还原只读
    if (wasRo) SetFileAttributesW(path.c_str(), attr);
    if (!ok) return L"写入 hosts 失败（" + ErrorTextZh(GetLastError()) + L"）";
    return wstring();
}

// ---------------------------------------------------------------- hosts 编辑对话框
//  多行编辑器（ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL）加载 hosts 全文。
//  模块运行在工作线程上：在此线程创建顶层窗口并自跑消息循环（Owner 设为
//  主窗口 → 模态遮挡与 Z 序成立；关闭后重新激活主窗口）。

struct HostsEditResult {
    bool ok = false;
    wstring text;
};

LRESULT CALLBACK HostsEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) {                       // 保存
            HWND edit = GetDlgItem(h, 100);
            int len = GetWindowTextLengthW(edit);
            wstring t(len + 1, L'\0');
            GetWindowTextW(edit, t.data(), len + 1);
            t.resize(len);
            auto* res = (HostsEditResult*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (res) { res->ok = true; res->text = t; }
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HostsEditResult ShowHostsEditDialog(HWND parent, const wstring& text) {
    HostsEditResult res;

    // 消息字体（跟随系统，非 UI 线程创建字体安全）
    HFONT font = nullptr;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        font = CreateFontIndirectW(&ncm.lfMessageFont);

    // 窗口类（重复注册失败无碍）
    static const wchar_t* kCls = L"NetAdminHostsEdit";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostsEditProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    int W = ui::dpi::S(760), H = ui::dpi::S(540);
    int m = ui::dpi::S(14), bh = ui::dpi::S(34), bw = ui::dpi::S(110);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls,
                               L"编辑 hosts（保存前自动备份为 hosts.bak）",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { if (font) DeleteObject(font); return res; }
    if (parent) {
        SetWindowLongPtrW(dlg, GWLP_HWNDPARENT, (LONG_PTR)parent);   // Owner → 模态遮挡
        EnableWindow(parent, FALSE);
    }

    // 多行编辑器
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL |
                                ES_WANTRETURN,
                                m, m, W - 2 * m, H - 3 * m - bh,
                                dlg, (HMENU)(INT_PTR)100, nullptr, nullptr);
    // 按钮（保存 / 取消）—— 子窗口必须显式 WS_VISIBLE
    HWND btnSave = CreateWindowExW(0, L"BUTTON", L"保存(&S)",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   W - 2 * bw - 3 * m, H - bh - m, bw, bh,
                                   dlg, (HMENU)(INT_PTR)IDOK, nullptr, nullptr);
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"取消(&C)",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     W - bw - 2 * m, H - bh - m, bw, bh,
                                     dlg, (HMENU)(INT_PTR)IDCANCEL, nullptr, nullptr);
    if (font) {
        SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(btnSave, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(btnCancel, WM_SETFONT, (WPARAM)font, TRUE);
    }
    SetWindowTextW(edit, text.c_str());
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&res);
    SetFocus(edit);

    ShowWindow(dlg, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {       // Tab/Esc 键盘导航
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
    if (font) DeleteObject(font);
    return res;
}

class SysHostsModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.hosts"; }
    const wchar_t* Group() const override { return L"系统维护"; }
    const wchar_t* Title() const override { return L"hosts 文件"; }
    const wchar_t* Description() const override {
        return L"解析展示 hosts：IP 映射、主机名与注释行；图形化编辑——多行编辑器"
               L"全文修改、添加映射、屏蔽域名模板一键插入；写前自动备份 hosts.bak"
               L"（只读属性自动处理），支持一键还原";
    }
    ModuleKind Kind() const override { return ModuleKind::Console; }
    const wchar_t* RunLabel() const override { return L"重新读取"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"添加映射 IP", L"如 127.0.0.1（配合「添加映射」动作）", false },
            { L"主机名",      L"如 ads.example.com，多个用空格分隔", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fIp_ = values[0];
        if (values.size() > 1) fHost_ = values[1];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"编辑 hosts",     nullptr, false, 0xE70F },   // 0 → btnClean 位
            { L"还原上次修改",   nullptr, false, 0xE72C },   // 1 → btnUndo 位
            { L"添加映射",       nullptr, false, 0 },        // 2 → 更多 ▾
            { L"屏蔽域名模板",   nullptr, false, 0 },        // 3 → 更多 ▾
        };
    }

    wstring ActionWarning(int actionIndex, const vector<int>&) const override {
        switch (actionIndex) {
        case 0:
            return L"将打开多行编辑器加载 hosts 全文；保存前自动备份到 hosts.bak，"
                   L"原文件只读属性保留。\n\n是否继续？";
        case 1:
            return L"将用 hosts.bak（上次保存前的内容）覆盖当前 hosts。\n\n是否继续？";
        case 2: {
            if (TrimW(fIp_).empty() || TrimW(fHost_).empty())
                return L"请先在上方填写「添加映射 IP」与「主机名」。";
            return L"将向 hosts 追加映射行：\n\n" + TrimW(fIp_) + L"  " + TrimW(fHost_) +
                   L"\n\n写前自动备份 hosts.bak。\n是否继续？";
        }
        case 3:
            return L"将向 hosts 追加一段常见广告/追踪域名屏蔽模板（127.0.0.1，"
                   L"带标记注释便于识别与删除，已存在则跳过）。\n\n写前自动备份"
                   L" hosts.bak。\n是否继续？";
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};

        wstring path = HostsPath2();
        wstring text, err;
        if (!ReadHostsFull(path, &text, &err)) {
            out.summary = err;
            out.hint = T(L"若被安全软件锁定或权限不足，请以管理员身份重试。");
            return;
        }
        ctx.Report(-1, L"正在读取 hosts 文件…");

        HostsStats st;
        ParseHostsText(text, out.rows, st, ctx.cancel);

        wstring summary = L"读取 " + NumU32((unsigned)(st.mappings + st.comments + st.invalid)) + L" 行：";
        summary += NumU32((unsigned)st.mappings) + L" 条映射 / " + NumU32((unsigned)st.comments) + L" 行注释";
        if (st.invalid) summary += L" / " + NumU32((unsigned)st.invalid) + L" 行无法解析";
        summary += L"（空行 " + NumU32((unsigned)st.blank) + L"）";
        out.summary = summary;

        DWORD attr = GetFileAttributesW(path.c_str());
        bool ro = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_READONLY);
        out.hint = T(L"文件：") + path + (ro ? T(L"（只读）") : wstring()) +
                   T(L" · 「编辑 hosts」可全文修改，「更多 ▾」添加映射 / 屏蔽模板 / 还原。");
        ctx.Report(100, L"hosts 读取完成");
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        wstring path = HostsPath2();
        switch (actionIndex) {
        case 0: {   // 编辑 hosts（多行编辑器）
            wstring text, err;
            if (!ReadHostsFull(path, &text, &err)) {
                out.summary = err;
                out.canClean = true;
                return;
            }
            HostsEditResult r = ShowHostsEditDialog(ctx.hwnd, text);
            if (!r.ok) {
                out.summary = L"已取消编辑（未做任何修改）";
                out.canClean = true;
                return;
            }
            wstring werr = WriteHostsFull(path, r.text);
            if (!werr.empty()) {
                out.summary = werr;
                out.canClean = true;
                return;
            }
            out.summary = L"hosts 已保存（原内容已备份为 hosts.bak，刷新 DNS 缓存后生效）";
            ctx.Log(L"hosts 已保存：原内容 " + NumU32((unsigned)text.size()) + L" 字符 → 新内容 " +
                    NumU32((unsigned)r.text.size()) + L" 字符");
            out.canClean = true;
            break;
        }
        case 1: {   // 还原上次修改（hosts.bak 覆盖）
            wstring bak = path + L".bak";
            if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) {
                out.summary = L"hosts.bak 不存在（还没有保存过修改）";
                out.canClean = true;
                return;
            }
            DWORD attr = GetFileAttributesW(path.c_str());
            bool wasRo = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_READONLY);
            if (wasRo) SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
            bool ok = CopyFileW(bak.c_str(), path.c_str(), FALSE) != FALSE;
            if (wasRo && attr != INVALID_FILE_ATTRIBUTES)
                SetFileAttributesW(path.c_str(), attr);
            out.summary = ok ? wstring(L"已还原：hosts.bak → hosts")
                             : L"还原失败（" + ErrorTextZh(GetLastError()) +
                                   L"）——需要管理员权限";
            out.canClean = true;
            break;
        }
        case 2: {   // 添加映射
            wstring ip = TrimW(fIp_), host = TrimW(fHost_);
            if (ip.empty() || host.empty()) {
                out.summary = L"请先在上方填写「添加映射 IP」与「主机名」";
                out.canClean = true;
                return;
            }
            if (!LooksLikeIpW(ip)) {
                out.summary = L"「" + ip + L"」不像 IP 地址（应为 IPv4/IPv6），已拒绝写入";
                out.canClean = true;
                return;
            }
            // 主机名再校验：禁止空白与非法字符
            for (wchar_t c : host) {
                if (c == L' ' || c == L'\t') continue;   // 多主机名空格分隔
                if (!( (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                       (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' ||
                       c == L'_' || c == L':' )) {
                    out.summary = L"主机名含非法字符（仅允许字母/数字/.-_: 与空格分隔），已拒绝写入";
                    out.canClean = true;
                    return;
                }
            }
            wstring text, err;
            if (!ReadHostsFull(path, &text, &err)) {
                out.summary = err;
                out.canClean = true;
                return;
            }
            if (!text.empty() && text.back() != L'\n') text += L"\r\n";
            text += ip + L" " + host + L"\r\n";
            wstring werr = WriteHostsFull(path, text);
            out.summary = werr.empty()
                ? wstring(L"已追加映射：") + ip + L" " + host + L"（原内容已备份 hosts.bak）"
                : werr;
            out.canClean = true;
            break;
        }
        case 3: {   // 屏蔽域名模板
            static const wchar_t* kTemplate[] = {
                L"ad.doubleclick.net",
                L"googleads.g.doubleclick.net",
                L"pagead2.googlesyndication.com",
                L"www.googleadservices.com",
                L"pos.baidu.com",
                L"cpro.baidu.com",
            };
            wstring mark1 = L"# >>> NetAdmin Toolbox 屏蔽模板 (v2.15) >>>";
            wstring mark2 = L"# <<< NetAdmin Toolbox 屏蔽模板 <<<";
            wstring text, err;
            if (!ReadHostsFull(path, &text, &err)) {
                out.summary = err;
                out.canClean = true;
                return;
            }
            if (text.find(mark1) != wstring::npos) {
                out.summary = L"屏蔽模板已存在（识别到标记注释），跳过";
                out.canClean = true;
                return;
            }
            if (!text.empty() && text.back() != L'\n') text += L"\r\n";
            text += mark1 + L"\r\n";
            for (const wchar_t* d : kTemplate) text += wstring(L"127.0.0.1 ") + d + L"\r\n";
            text += mark2 + L"\r\n";
            wstring werr = WriteHostsFull(path, text);
            out.summary = werr.empty()
                ? wstring(L"已追加屏蔽模板（") + NumU32((unsigned)(sizeof(kTemplate) / sizeof(kTemplate[0]))) +
                  L" 个域名，标记注释可识别可手动删除）"
                : werr;
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    wstring fIp_, fHost_;
};

}  // namespace

// ===========================================================================
//  模块注册（每个文件末尾一行；注册顺序即界面显示顺序）
// ===========================================================================

REGISTER_MODULE(SysInfoModule);
REGISTER_MODULE(SysDiskModule);
REGISTER_MODULE(SysProcessModule);
REGISTER_MODULE(SysStartupModule);
REGISTER_MODULE(SysServicesModule);
REGISTER_MODULE(SysHotfixModule);
REGISTER_MODULE(SysEnvModule);
REGISTER_MODULE(SysHostsModule);

}  // namespace mz
