// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 安全体检模块集（v2.8 T04/R6）
//  ---------------------------------------------------------------------------
//  sec.scan   一键体检   Action  只读聚合 8 项检查，逐行结论（正常/警告/无法检测）
//  sec.repair 系统修复   Action+扩展动作  sfc/DISM/chkdsk/netsh/Defender，
//             全部经 procrun::RunProcess 流式执行（命令原文入日志、逐行回调、
//             退出码汇总；Esc 取消 → TerminateProcess，日志记录取消时点）
//  数据来源：注册表 / SCM / INetFwPolicy2 COM / WMI(ROOT\SecurityCenter2) /
//  CBS.log 尾部 / 文件系统，全部只读（修复动作除外，均需确认框）。
//  链接：-lole32 -loleaut32（WMI/防火墙 COM；GUID 由 initguid.h 在本 TU 实例化）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../procrun.h"
#include "../cleanresult.h"   // ErrorTextZh（非提权拒绝提示等归因）

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include <initguid.h>   // 在 netfw.h / wbemcli.h 之前，实例化 CLSID/IID（仅本 TU）
#include <netfw.h>
#include <wbemcli.h>
#include <winsvc.h>

namespace mz {
namespace {

// ===========================================================================
//  通用小工具
// ===========================================================================

wstring U32(unsigned v) {
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"%u", v);
    return b;
}

wstring I64(long long v) {
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"%lld", v);
    return b;
}

wstring GetEnvS(const wchar_t* name) {
    wchar_t buf[32767] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, 32767);
    return (n > 0 && n < 32767) ? wstring(buf, n) : wstring();
}

// 读 HKLM DWORD 值（读不到返回 defV）
DWORD RegDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD defV = 0) {
    DWORD v = defV, sz = sizeof(v), type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, &type, &v, &sz) != ERROR_SUCCESS)
        return defV;
    return v;
}

// 读 HKLM REG_SZ 值
wstring RegString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[1024] = {};
    DWORD sz = sizeof(buf), type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, buf, &sz) != ERROR_SUCCESS)
        return wstring();
    return wstring(buf);
}

// 大小写不敏感子串
bool ContainsI(const wstring& hay, const wstring& needle) {
    wstring h = hay, n = needle;
    for (auto& c : h) { if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a'); }
    for (auto& c : n) { if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a'); }
    return h.find(n) != wstring::npos;
}

// 体检行写入（警告行红标；summary 计数由 Run 统一算）
void AddCheck(ModuleResult& out, const wstring& name, const wstring& result,
              const wchar_t* level, const wstring& advice) {
    ModuleRow r;
    r.c1 = name;
    r.c2 = result;
    r.c3 = level;
    r.c4 = advice;
    r.sortKey = 0;
    r.danger = (wcscmp(level, L"警告") == 0);
    out.rows.push_back(r);
}

// ===========================================================================
//  COM 会话（体检模块内 WMI / 防火墙 COM 共用）
// ===========================================================================

struct ComSession {
    bool uninit = false;
    void Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) uninit = true;   // RPC_E_CHANGED_MODE 视为可用，不释放
    }
    ~ComSession() { if (uninit) CoUninitialize(); }
};

// ===========================================================================
//  sec.scan 检查项实现（全部只读）
// ===========================================================================

// ① UAC 是否启用（EnableLUA）
void CheckUac(ModuleResult& out) {
    DWORD lua = RegDword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"EnableLUA", 1);
    if (lua) {
        AddCheck(out, L"用户账户控制(UAC)", L"已启用", L"正常",
                 L"保持默认即可；提权弹窗是最后一道防线");
    } else {
        AddCheck(out, L"用户账户控制(UAC)", L"已禁用", L"警告",
                 L"恶意程序可静默提权；建议「控制面板→用户账户→更改用户账户控制设置」恢复");
    }
}

// ② Defender 实时保护（SecurityCenter2 WMI 优先，降级注册表 + 服务口径）
// 查 SecurityCenter2 杀毒产品清单；返回 false = WMI 不可用
bool QueryAntivirusProducts(vector<std::pair<wstring, DWORD>>* out) {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (void**)&loc)) || !loc)
        return false;
    IWbemServices* svc = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\SecurityCenter2");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr) || !svc) return false;
    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    BSTR qLang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT displayName, productState FROM AntiVirusProduct");
    IEnumWbemClassObject* en = nullptr;
    hr = svc->ExecQuery(qLang, query,
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &en);
    SysFreeString(qLang);
    SysFreeString(query);
    bool ok = SUCCEEDED(hr) && en;
    if (ok) {
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0) break;
            VARIANT vName, vState;
            VariantInit(&vName);
            VariantInit(&vState);
            if (SUCCEEDED(obj->Get(L"displayName", 0, &vName, nullptr, nullptr)) &&
                SUCCEEDED(obj->Get(L"productState", 0, &vState, nullptr, nullptr)) &&
                vName.vt == VT_BSTR && vState.vt == VT_I4) {
                out->push_back({ vName.bstrVal, (DWORD)vState.lVal });
            }
            VariantClear(&vName);
            VariantClear(&vState);
            obj->Release();
        }
        en->Release();
    }
    svc->Release();
    return ok;
}

// 服务状态快查（不存在的服务返回 -1；startType 可空）
int ServiceState(const wchar_t* name, DWORD* startType) {
    if (startType) *startType = 0xFFFFFFFF;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return -1;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) { CloseServiceHandle(scm); return -1; }
    SERVICE_STATUS st = {};
    int state = -1;
    if (QueryServiceStatus(svc, &st)) state = (int)st.dwCurrentState;
    if (startType) {
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        vector<BYTE> cfg(needed ? needed : 8192);
        if (QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)cfg.data(),
                                (DWORD)cfg.size(), &needed))
            *startType = ((LPQUERY_SERVICE_CONFIGW)cfg.data())->dwStartType;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return state;
}

void CheckDefender(ModuleResult& out) {
    // 策略注册表口径
    bool policyDisabled = RegDword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
        L"DisableAntiSpyware", 0) == 1;
    bool rtDisabled = RegDword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows Defender\\Real-Time Protection",
        L"DisableRealtimeMonitoring", 0) == 1;
    // WinDefend 服务
    DWORD wdStart = 0xFFFFFFFF;
    int wdState = ServiceState(L"WinDefend", &wdStart);

    // WMI：第三方主防检测
    vector<std::pair<wstring, DWORD>> avs;
    bool wmiOk = QueryAntivirusProducts(&avs);
    wstring thirdParty;
    for (auto& av : avs) {
        bool realTimeOn = (av.second & 0x1000) != 0;   // productState bit 12
        if (realTimeOn && !ContainsI(av.first, L"Windows Defender"))
            thirdParty = av.first;
    }

    if (wmiOk && !thirdParty.empty()) {
        AddCheck(out, L"Defender 实时保护",
                 L"检测到非 Defender 主防：" + thirdParty, L"正常",
                 L"第三方杀软接管实时防护；本项按 Defender 口径仅供参考");
        return;
    }
    if (policyDisabled || rtDisabled) {
        AddCheck(out, L"Defender 实时保护", L"已被策略禁用", L"警告",
                 L"系统暴露于恶意软件；请到「Windows 安全中心→病毒和威胁防护」重新开启");
        return;
    }
    if (wdState == SERVICE_STOPPED || wdStart == SERVICE_DISABLED) {
        AddCheck(out, L"Defender 实时保护",
                 L"WinDefend 服务" + wstring(wdStart == SERVICE_DISABLED ? L"已禁用" : L"已停止"),
                 L"警告",
                 L"请到「服务」中启动 WinDefend，或在 Windows 安全中心手动触发");
        return;
    }
    if (wdState == SERVICE_RUNNING) {
        AddCheck(out, L"Defender 实时保护", L"运行中", L"正常",
                 L"实时防护开启；可用「系统修复 → Defender 扫描/更新」做快速体检");
        return;
    }
    AddCheck(out, L"Defender 实时保护", L"无法确认当前状态", L"无法检测",
             L"注册表与服务状态均不可读（可能被第三方策略接管）");
}

// ③ 防火墙三配置文件（INetFwPolicy2 COM，失败降级注册表）
void CheckFirewall(ModuleResult& out) {
    bool dom = false, priv = false, pub = false;
    bool comOk = false;
    {
        INetFwPolicy2* p = nullptr;
        static const CLSID kCLSID_NetFwPolicy2 = { 0xe2b3c97f, 0x6ae1, 0x41ac,
            { 0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd } };
        if (SUCCEEDED(CoCreateInstance(kCLSID_NetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_INetFwPolicy2, (void**)&p)) && p) {
            VARIANT_BOOL v = VARIANT_FALSE;
            if (SUCCEEDED(p->get_FirewallEnabled(NET_FW_PROFILE2_DOMAIN, &v))) {
                dom = (v != VARIANT_FALSE);
                if (SUCCEEDED(p->get_FirewallEnabled(NET_FW_PROFILE2_PRIVATE, &v))) priv = (v != VARIANT_FALSE);
                if (SUCCEEDED(p->get_FirewallEnabled(NET_FW_PROFILE2_PUBLIC, &v))) pub = (v != VARIANT_FALSE);
                comOk = true;
            }
            p->Release();
        }
    }
    if (!comOk) {
        // 降级注册表：SharedAccess\Parameters\FirewallPolicy\<Profile>\EnableFirewall
        auto profOn = [](const wchar_t* prof) {
            return RegDword(HKEY_LOCAL_MACHINE,
                (wstring(L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\") + prof).c_str(),
                L"EnableFirewall", 1) == 1;
        };
        dom = profOn(L"DomainProfile");
        priv = profOn(L"StandardProfile");
        pub = profOn(L"PublicProfile");
    }
    auto txt = [](bool b) { return b ? L"启用" : L"禁用"; };
    wstring result = wstring(L"域 ") + txt(dom) + L" · 专用 " + txt(priv) +
                     L" · 公用 " + txt(pub);
    if (dom && priv && pub) {
        AddCheck(out, L"防火墙", result, L"正常", L"三个配置文件均已开启");
    } else {
        AddCheck(out, L"防火墙", result, L"警告",
                 L"存在已禁用的配置文件；请到「Windows 安全中心→防火墙和网络保护」开启");
    }
}

// ④ 系统盘剩余空间
void CheckDiskSpace(ModuleResult& out) {
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wstring drive = (winDir[0] && winDir[1] == L':') ? wstring(winDir, 2) : L"C:";
    ULARGE_INTEGER freeBytes = {}, total = {};
    if (!GetDiskFreeSpaceExW((drive + L"\\").c_str(), &freeBytes, &total, nullptr)) {
        AddCheck(out, L"系统盘剩余空间", L"无法查询", L"无法检测", L"GetDiskFreeSpaceExW 失败");
        return;
    }
    long long gb = (long long)(freeBytes.QuadPart / (1024ULL * 1024 * 1024));
    if (gb < 10) {
        AddCheck(out, L"系统盘剩余空间(" + drive + L")", I64(gb) + L" GB", L"警告",
                 L"空间不足会影响更新与系统运行；可用「磁盘清理」模块释放空间");
    } else {
        AddCheck(out, L"系统盘剩余空间(" + drive + L")", I64(gb) + L" GB", L"正常",
                 L"充足（阈值 10 GB）");
    }
}

// ⑤ Windows 更新滞后（Auto Update Results\Install LastSuccessTime）
void CheckUpdateFreshness(ModuleResult& out) {
    wstring t = RegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\Results\\Install",
        L"LastSuccessTime");
    int y = 0, mo = 0, d = 0;
    if (!t.empty())
        swscanf_s(t.c_str(), L"%d-%d-%d", &y, &mo, &d);
    if (y < 2000) {
        AddCheck(out, L"Windows 更新", L"无成功安装记录", L"无法检测",
                 L"注册表无 LastSuccessTime；请到「设置→Windows 更新」手动检查");
        return;
    }
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    auto days = [](SYSTEMTIME a, int Y, int M, int D) -> long long {
        FILETIME fa = {}, fb = {};
        SYSTEMTIME b = a;
        b.wYear = (WORD)Y; b.wMonth = (WORD)M; b.wDay = (WORD)D;
        b.wHour = 12; b.wMinute = 0; b.wSecond = 0; b.wMilliseconds = 0;
        SystemTimeToFileTime(&a, &fa);
        SystemTimeToFileTime(&b, &fb);
        long long ta = ((long long)fa.dwHighDateTime << 32) | fa.dwLowDateTime;
        long long tb = ((long long)fb.dwHighDateTime << 32) | fb.dwLowDateTime;
        return (ta - tb) / 864000000000LL;   // 100ns → 天
    };
    long long dd = days(now, y, mo, d);
    wstring when = t;
    if (dd > 30) {
        AddCheck(out, L"Windows 更新", L"距上次成功安装 " + I64(dd) + L" 天（" + when + L"）",
                 L"警告", L"补丁滞后超过 30 天；请到「设置→Windows 更新→检查更新」");
    } else {
        AddCheck(out, L"Windows 更新", L"距上次成功安装 " + I64(dd) + L" 天（" + when + L"）",
                 L"正常", L"补丁新鲜度良好（阈值 30 天）");
    }
}

// ⑥ sfc 建议项：CBS.log 尾部 [SR] Cannot repair 计数（非管理员通常读不到）
void CheckSfcLog(ModuleResult& out) {
    wstring log = L"C:\\Windows\\Logs\\CBS\\CBS.log";
    HANDLE h = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AddCheck(out, L"sfc 完整性(历史)", L"CBS 日志不可读", L"无法检测",
                 L"需要管理员权限；提权后重扫，或直接「系统修复 → sfc 系统文件检查」");
        return;
    }
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    const long long kTail = 512 * 1024;
    LARGE_INTEGER pos;
    pos.QuadPart = (sz.QuadPart > kTail) ? (sz.QuadPart - kTail) : 0;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    vector<char> buf(256 * 1024);
    DWORD rd = 0;
    std::string text;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0)
        text.append(buf.data(), rd);
    CloseHandle(h);
    // ASCII 日志：统计 [SR] Cannot repair 出现次数
    int cnt = 0;
    for (size_t i = 0; i + 4 < text.size(); ++i) {
        if (text.compare(i, 4, "[SR]") == 0) {
            std::string seg = text.substr(i, 200);
            for (auto& c : seg) c = (char)towlower((unsigned char)c);
            if (seg.find("cannot repair") != std::string::npos) ++cnt;
        }
    }
    if (cnt > 0) {
        AddCheck(out, L"sfc 完整性(历史)", U32((unsigned)cnt) + L" 处 [SR] cannot repair",
                 L"警告", L"存在历史修复失败记录；建议「系统修复 → DISM 组件修复」后重跑 sfc");
    } else {
        AddCheck(out, L"sfc 完整性(历史)", L"日志尾部无未修复记录", L"正常",
                 L"近期完整性检查无遗留问题");
    }
}

// ⑦ 关键服务状态（wuauserv / BITS / Schedule / WlanSvc / MpsSvc）
void CheckKeyServices(ModuleResult& out) {
    struct SvcDef { const wchar_t* name; const wchar_t* label; };
    static const SvcDef svcs[] = {
        { L"wuauserv", L"Windows 更新" },
        { L"BITS",     L"后台传输" },
        { L"Schedule", L"任务计划" },
        { L"WlanSvc",  L"无线网络" },
        { L"MpsSvc",   L"防火墙服务" },
    };
    vector<wstring> problems, normals;
    for (auto& s : svcs) {
        DWORD start = 0xFFFFFFFF;
        int st = ServiceState(s.name, &start);
        if (st < 0) {
            normals.push_back(wstring(s.label) + L"(未安装)");
            continue;
        }
        if (start == SERVICE_DISABLED) {
            problems.push_back(wstring(s.label) + L" 已禁用");
        } else if (wcscmp(s.name, L"Schedule") == 0 && st != SERVICE_RUNNING) {
            problems.push_back(wstring(s.label) + L" 未运行");
        } else if (wcscmp(s.name, L"MpsSvc") == 0 && st != SERVICE_RUNNING) {
            problems.push_back(wstring(s.label) + L" 未运行");
        } else {
            normals.push_back(wstring(s.label) + L"(" +
                              (st == SERVICE_RUNNING ? L"运行" : L"停止") + L")");
        }
    }
    if (problems.empty()) {
        AddCheck(out, L"关键服务", L"全部就绪：" + normals[0] + L" 等 5 项", L"正常",
                 L"wuauserv / BITS / Schedule / WlanSvc / MpsSvc");
    } else {
        wstring all;
        for (auto& p : problems) all += p + L"；";
        AddCheck(out, L"关键服务", all, L"警告",
                 L"到「服务管理」模块或 services.msc 处理对应服务");
    }
}

// ⑧ 系统还原点服务（srservice 供「创建还原点」预警）
void CheckSystemRestore(ModuleResult& out) {
    DWORD start = 0xFFFFFFFF;
    int st = ServiceState(L"srservice", &start);
    if (start == SERVICE_DISABLED) {
        AddCheck(out, L"系统还原服务", L"srservice 已禁用", L"警告",
                 L"「创建还原点」将失败；请在 services.msc 将 System Restore Service 设为手动后启用");
    } else if (st == SERVICE_RUNNING) {
        AddCheck(out, L"系统还原服务", L"运行中", L"正常", L"可正常创建还原点");
    } else if (st < 0) {
        AddCheck(out, L"系统还原服务", L"服务不存在", L"无法检测",
                 L"本系统可能精简了系统还原组件");
    } else {
        AddCheck(out, L"系统还原服务", L"已安装（按需启动）", L"正常",
                 L"创建还原点时会按需拉起（默认配置）");
    }
}

// ===========================================================================
//  sec.repair —— 命令矩阵与流式执行
// ===========================================================================

// 定位 MpCmdRun.exe：Platform\<最新版本> → Program Files → 空串
wstring FindMpCmdRun() {
    wstring base = GetEnvS(L"ProgramData") + L"\\Microsoft\\Windows Defender\\Platform";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (best.empty() || wcscmp(fd.cFileName, best.c_str()) > 0)
                best = fd.cFileName;   // 版本目录名字典序即版本序
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best.empty()) return base + L"\\" + best + L"\\MpCmdRun.exe";
    wstring pf = L"C:\\Program Files\\Windows Defender\\MpCmdRun.exe";
    if (GetFileAttributesW(pf.c_str()) != INVALID_FILE_ATTRIBUTES) return pf;
    return wstring();
}

// sfc 三态结论解析（sfc 输出为 UTF-16，procrun 已按宽字符解码）
wstring SfcConclusion(const wstring& full, int exitCode) {
    if (ContainsI(full, L"未发现完整性冲突") ||
        ContainsI(full, L"did not find any integrity violations"))
        return L"未发现完整性冲突";
    if (ContainsI(full, L"无法修复") || ContainsI(full, L"unable to fix"))
        return L"存在无法修复项（建议先 DISM 组件修复，再重跑 sfc）";
    if (ContainsI(full, L"成功修复") || ContainsI(full, L"successfully repaired") ||
        ContainsI(full, L"修复操作") || ContainsI(full, L"repair"))
        return L"发现损坏文件并已修复（详见上方日志）";
    wchar_t b[160] = {};
    swprintf_s(b, 160, L"完成（退出码 %d），结论请查看上方日志", exitCode);
    return b;
}

class SecScanModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sec.scan"; }
    const wchar_t* Group() const override { return L"安全体检"; }
    const wchar_t* Title() const override { return L"一键体检"; }
    const wchar_t* Description() const override { return L"UAC/Defender/防火墙/磁盘/更新/sfc/关键服务/还原点 只读聚合检查"; }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    const wchar_t* RunLabel() const override { return L"开始体检"; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"开始体检（只读，约数秒）…");

        ComSession com;
        com.Init();   // WMI / 防火墙 COM 需要；失败时各检查自行降级

        CheckUac(out);
        if (ctx.Cancelled()) { out.summary = L"已中止"; return; }
        CheckDefender(out);
        if (ctx.Cancelled()) { out.summary = L"已中止"; return; }
        CheckFirewall(out);
        CheckDiskSpace(out);
        CheckUpdateFreshness(out);
        CheckSfcLog(out);
        CheckKeyServices(out);
        CheckSystemRestore(out);

        int ok = 0, warn = 0, unknown = 0;
        for (auto& r : out.rows) {
            if (r.c3 == L"警告") ++warn;
            else if (r.c3 == L"无法检测") ++unknown;
            else ++ok;
        }
        wchar_t b[160] = {};
        swprintf_s(b, 160, L"体检完成：%d 项正常 / %d 项警告 / %d 项无法检测", ok, warn, unknown);
        out.summary = b;
        if (warn > 0)
            out.hint = T(L"警告项见「建议」列；对应修复动作在「系统修复」模块（需管理员）。");
        out.canClean = false;
        ctx.Report(100, L"体检完成");
    }
};

class SecRepairModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sec.repair"; }
    const wchar_t* Group() const override { return L"安全体检"; }
    const wchar_t* Title() const override { return L"系统修复"; }
    const wchar_t* Description() const override { return L"sfc/DISM/chkdsk/网络栈重置/Defender 扫描（流式日志、可取消）"; }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsElevation() const override { return true; }

    // 动作 0/1 → 按钮位；2..8 → 「更多 ▾」菜单
    vector<ModuleAction> Actions() const override {
        return {
            { L"sfc 系统文件检查",
              L"sfc /scannow 通常需要 5~20 分钟，期间请勿关闭计算机。\n\n是否继续？",
              false, 0 },
            { L"DISM 组件修复",
              L"DISM RestoreHealth 通常需要 10~30 分钟，期间请勿关机或中断操作。\n\n是否继续？",
              true, 0 },
            { L"磁盘检查(只读)",
              L"chkdsk C: /scan 为只读诊断（不改磁盘），约 2~10 分钟。\n\n是否继续？",
              false, 0 },
            { L"Winsock 重置",
              L"重置 Winsock 目录后需重启才能完全生效，LSP 网络设置会被还原。\n\n是否继续？",
              true, 0 },
            { L"重置网络栈",
              L"将重置 TCP/IP 与 IPv6 配置并刷新 DNS：静态 IP 配置会丢失，需重启生效。\n\n是否继续？",
              true, 0 },
            { L"Defender 扫描/更新",
              L"先更新病毒定义，再执行快速扫描，约数分钟。\n\n是否继续？",
              false, 0 },
            { L"DISM ScanHealth(只读诊断)",
              L"只读检测组件存储健康状况（不做修复），约 5~15 分钟。\n\n是否继续？",
              false, 0 },
            { L"chkdsk /f 预约(重启后执行)",
              L"将设置 C: 卷脏位，下次重启时自动执行完整磁盘检查；重启检查过程不可中断，可能持续数十分钟。\n\n是否继续？",
              true, 0 },
            { L"Winsock 目录(只读诊断)",
              L"查看 Winsock 目录（只读，不修改任何网络配置）。\n\n是否继续？",
              false, 0 },
        };
    }

    // 只读诊断动作（非管理员也放行）：Winsock 目录查看
    bool ReadOnlyAction(int idx) const { return idx == 8; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        out.summary = L"请选择修复动作（共 " + U32((unsigned)acts.size()) +
                      L" 个）：主按钮 sfc / DISM，其余在「更多 ▾」菜单";
        out.hint = T(L"所有命令原文与逐行输出会写入日志通道；执行中可随时停止（Esc/停止按钮）。");
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        if (actionIndex < 0 || actionIndex >= (int)acts.size()) return;

        // 提权联动（R1 正常路径已是管理员）：非管理员仅放行只读诊断
        if (!IsElevated() && !ReadOnlyAction(actionIndex)) {
            out.summary = L"需要管理员权限：「" + wstring(acts[actionIndex].label) +
                          L"」未执行";
            out.hint = T(L"请以管理员身份运行本程序（启动时会自动请求提权）。");
            ctx.Log(L"  拒绝: 当前非管理员，动作「" + wstring(acts[actionIndex].label) +
                    L"」需要管理员权限");
            out.canClean = true;
            return;
        }

        wstring conclusion;
        switch (actionIndex) {
        case 0: conclusion = RunSfc(ctx, out); break;
        case 1: conclusion = RunDism(ctx, L"DISM /Online /Cleanup-Image /RestoreHealth",
                                     L"还原操作已成功完成",
                                     L"The restore operation completed successfully",
                                     L"组件修复"); break;
        case 2: conclusion = RunSimple(ctx, L"chkdsk C: /scan",
                                       L"只读磁盘检查", L"Windows 已检查该卷未发现问题"); break;
        case 3: conclusion = RunSimple(ctx, L"netsh winsock reset",
                                       L"Winsock 重置", L"成功重置 Winsock 目录"); break;
        case 4: conclusion = RunNetReset(ctx); break;
        case 5: conclusion = RunDefender(ctx); break;
        case 6: conclusion = RunDism(ctx, L"DISM /Online /Cleanup-Image /ScanHealth",
                                     L"操作成功完成", L"The operation completed successfully",
                                     L"组件诊断"); break;
        case 7: conclusion = RunChkdskSchedule(ctx); break;
        case 8: conclusion = RunSimple(ctx, L"netsh winsock show catalog",
                                       L"Winsock 目录诊断", wstring()); break;
        }
        out.summary = L"「" + wstring(acts[actionIndex].label) + L"」" + conclusion;
        out.canClean = true;
    }

private:
    // ---- procrun 流式执行通道：命令原文入日志、逐行回调、退出码汇总 ----
    int ExecRepair(ModuleContext& ctx, const wstring& cmd, int timeoutMs,
                   wstring* fullOut) {
        ctx.Log(L"  $ " + cmd);
        ProcRunOptions opt;
        opt.timeoutMs = timeoutMs;
        opt.cancel = [&ctx]() { return ctx.Cancelled(); };
        opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  " + line); };
        int code = RunProcess(cmd, opt, fullOut);
        if (code == -3)
            ctx.Log(L"  ⚠ 用户取消——修复可能未完成，建议稍后重新执行");
        else if (code == -2)
            ctx.Log(L"  ⚠ 超时被终止（时限 " + I64(timeoutMs / 60000) + L" 分钟）");
        else if (code == -1)
            ctx.Log(L"  ⚠ 进程启动失败");
        else
            ctx.Log(L"  [退出码 " + I64(code) + L"]");
        return code;
    }

    wstring RunSfc(ModuleContext& ctx, ModuleResult& out) {
        ctx.Report(-1, L"阶段：sfc 系统文件检查（5~20 分钟，可随时停止）");
        wstring full;
        int code = ExecRepair(ctx, L"sfc /scannow", 1800000, &full);
        if (code == -3) return L"已取消";
        wstring c = SfcConclusion(full, code);
        ctx.Log(L"  结论: " + c);
        return c;
    }

    // DISM 族：成功关键词中英文双匹配
    wstring RunDism(ModuleContext& ctx, const wstring& cmd, const wchar_t* okZh,
                    const wchar_t* okEn, const wchar_t* label) {
        ctx.Report(-1, (wstring(L"阶段：") + label + L"（10~30 分钟，可随时停止）").c_str());
        wstring full;
        int code = ExecRepair(ctx, cmd, 1800000, &full);
        if (code == -3) return L"已取消";
        if (code == 0 || ContainsI(full, okZh) || ContainsI(full, okEn))
            return L"成功完成" + wstring(code != 0 ? L"（含警告，退出码 " + I64(code) + L"）" : L"");
        return L"退出码 " + I64(code) + L"（非 0，详见上方日志）";
    }

    wstring RunSimple(ModuleContext& ctx, const wstring& cmd,
                      const wchar_t* label, const wstring& okMark) {
        ctx.Report(-1, (wstring(L"阶段：") + label).c_str());
        wstring full;
        int code = ExecRepair(ctx, cmd, 120000, &full);
        if (code == -3) return L"已取消";
        if (!okMark.empty() && ContainsI(full, okMark))
            return L"成功（" + okMark + L"）";
        if (code == 0) return L"成功完成";
        return L"退出码 " + I64(code) + L"（非 0，详见上方日志）";
    }

    wstring RunNetReset(ModuleContext& ctx) {
        ctx.Report(-1, L"阶段：重置网络栈（秒级 ×3 条）");
        wstring full;
        int code1 = ExecRepair(ctx, L"netsh int ip reset", 60000, &full);
        wstring full2;
        int code2 = ExecRepair(ctx, L"netsh int ipv6 reset", 60000, &full2);
        wstring full3;
        int code3 = ExecRepair(ctx, L"ipconfig /flushdns", 60000, &full3);
        if (code1 == -3 || code2 == -3 || code3 == -3) return L"已取消";
        int bad = (code1 != 0) + (code2 != 0) + (code3 != 0);
        ctx.Log(L"  提示: 网络栈已重置，需要重启计算机才能完全生效");
        if (bad == 0) return L"成功（3 条命令全部退出码 0），需重启生效";
        return I64(3 - bad) + L"/3 成功，需重启生效（详见上方日志）";
    }

    wstring RunDefender(ModuleContext& ctx) {
        ctx.Report(-1, L"阶段：Defender 签名更新 + 快速扫描");
        wstring exe = FindMpCmdRun();
        wstring full;
        int codeUp = 0, codeScan = 0;
        if (!exe.empty()) {
            ctx.Log(L"  MpCmdRun: " + exe);
            codeUp = ExecRepair(ctx, L"\"" + exe + L"\" -SignatureUpdate", 300000, &full);
            if (codeUp == -3) return L"已取消";
            codeScan = ExecRepair(ctx, L"\"" + exe + L"\" -Scan -ScanType 1", 900000, &full);
            if (codeScan == -3) return L"已取消";
            if (codeUp == 0 && codeScan == 0)
                return L"成功（定义更新 + 快速扫描均退出码 0）";
        }
        // 降级：PowerShell Defender cmdlet
        ctx.Log(L"  降级: MpCmdRun 不可用或失败，改用 PowerShell Defender cmdlet");
        codeUp = ExecRepair(ctx,
            L"powershell -NoProfile -Command \"Update-MpSignature\"", 300000, &full);
        if (codeUp == -3) return L"已取消";
        codeScan = ExecRepair(ctx,
            L"powershell -NoProfile -Command \"Start-MpScan -ScanType QuickScan\"", 900000, &full);
        if (codeScan == -3) return L"已取消";
        if (codeUp == 0 && codeScan == 0)
            return L"成功（PowerShell 通道：定义更新 + 快速扫描）";
        return L"定义更新退出码 " + I64(codeUp) + L"，扫描退出码 " + I64(codeScan) +
               L"（详见上方日志）";
    }

    wstring RunChkdskSchedule(ModuleContext& ctx) {
        ctx.Report(-1, L"阶段：预约磁盘检查（脏位标记，秒级）");
        wstring full;
        int code = ExecRepair(ctx, L"fsutil dirty set C:", 60000, &full);
        if (code == -3) return L"已取消";
        if (code == 0) {
            ctx.Log(L"  提示: 已预约——下次重启时自动执行完整磁盘检查（autochk）");
            ctx.Log(L"  提示: 可用「chkntfs C:」查看预约状态；重启检查过程请勿断电");
            return L"成功：C: 已标记为需检查，重启后自动执行";
        }
        return L"退出码 " + I64(code) + L"（详见上方日志）";
    }
};

} // namespace

REGISTER_MODULE(SecScanModule);
REGISTER_MODULE(SecRepairModule);

} // namespace mz
