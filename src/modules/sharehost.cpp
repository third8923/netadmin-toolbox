// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 共享部署一键通（v2.9 T03）
//  ---------------------------------------------------------------------------
//  单模块 net.sharehost「共享部署」，Group=网络管理，Kind=Action，提权要求=是。
//  实现严格对照 docs/dev-plan-v29.md §1.3 权威清单 14 项矩阵：
//    🟢 只读确认 / 🟡 开通动作可改 / 🔴 永远只读展示（代码层面无任何写路径）
//  数据来源：SCM(QueryServiceStatus/Config)、netsh(procrun 通道，命令原文入日志，
//  中英双组名探测 + 注册表 Grouping 字段兜底)、INetworkListManager COM
//  （MinGW 缺 netlistmgr.h → 本地最小接口声明，GUID 为 Windows SDK 标准值）、
//  RegGetValue（LSA / LanmanWorkstation 参数）、MRxSmb10 注册表判 SMB1。
//  ⚠ 安全敏感项（RestrictAnonymous / RestrictNullSessAccess / SMB 签名 /
//    HardenedPaths）：v2.9 为纯只读；v2.10 起按用户要求开放**显式逐项编辑**——
//    全部收进「更多 ▾」菜单、danger=true 红色项 + 独立风险确认框，仅点选
//    对应动作才会写；「一键开通 / 恢复保守」两个批量动作依然永不触碰它们。
//    SMB1 仍为只读（启用/禁用属 DISM 功能操作，见不做清单）。
//  链接：-ladvapi32 -lole32 -loleaut32 -lshlwapi -luuid（build.bat 已有）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"
#include "../procrun.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <objbase.h>
#include <oaidl.h>        // IEnumVARIANT（INetFwRules 枚举）
#include <shlwapi.h>      // SHLoadIndirectString（防火墙组名资源串本地化解析）
#include <netlistmgr.h>   // INetworkListManager / INetwork（MinGW 自带，GUID 在 uuid.lib）
#include <netfw.h>        // INetFwPolicy2 / INetFwRule（防火墙规则权威枚举）

namespace mz {
namespace {

// ---------------------------------------------------------------- 小工具

wstring U32(unsigned v) {
    wchar_t b[24] = {};
    swprintf_s(b, L"%u", v);
    return b;
}
wstring I64(long long v) {
    wchar_t b[24] = {};
    swprintf_s(b, L"%lld", v);
    return b;
}
wstring LastErrZh() { return ErrorTextZh(GetLastError()); }

// 大小写不敏感包含
bool ContainsI(const wstring& hay, const wstring& needle) {
    if (needle.empty()) return true;
    wstring h = hay, n = needle;
    for (auto& c : h) c = (wchar_t)towlower(c);
    for (auto& c : n) c = (wchar_t)towlower(c);
    return h.find(n) != wstring::npos;
}

// ---------------------------------------------------------------- 注册表读写

bool RegReadDwordV(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD* v) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(DWORD), type = 0;
    LONG r = RegGetValueW(hk, nullptr, name, RRF_RT_REG_DWORD, &type, v, &sz);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

LONG RegWriteDwordV(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD v) {
    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE,
                             nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
    return r;
}

LONG RegDeleteValueV(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY hk = nullptr;
    LONG r = RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &hk);
    if (r != ERROR_SUCCESS) return r;
    r = RegDeleteValueW(hk, name);
    RegCloseKey(hk);
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    return r;
}

const wchar_t* kLsaKey = L"SYSTEM\\CurrentControlSet\\Control\\Lsa";
const wchar_t* kLanmanWsParams = L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Parameters";
const wchar_t* kFwRulesKey = L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules";
const wchar_t* kMrxSmb10 = L"SYSTEM\\CurrentControlSet\\Services\\MRxSmb10";

// ---------------------------------------------------------------- 提权拒绝

bool RequireElevation(ModuleContext& ctx, const wstring& label, ModuleResult& out) {
    if (IsElevated()) return true;
    out.summary = L"需要管理员权限：「" + label + L"」未执行";
    out.hint = T(L"请以管理员身份运行本程序（启动时会自动请求提权）。");
    ctx.Log(L"  拒绝: 当前非管理员，动作「" + label + L"」需要管理员权限");
    out.canClean = true;
    return false;
}

// ---------------------------------------------------------------- SCM 查询

bool SvcQuery(SC_HANDLE scm, const wchar_t* name,
              DWORD* state, DWORD* startType, bool* delayed, bool* disabled) {
    *state = 0; *startType = 0xFFFFFFFF; *delayed = false; *disabled = false;
    SC_HANDLE svc = OpenServiceW(scm, name,
                                 SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) return false;
    bool ok = false;
    do {
        SERVICE_STATUS st = {};
        if (!QueryServiceStatus(svc, &st)) break;
        *state = st.dwCurrentState;
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        vector<BYTE> cfg(needed ? needed : 8192);
        if (!QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)cfg.data(),
                                 (DWORD)cfg.size(), &needed)) break;
        auto* q = (LPQUERY_SERVICE_CONFIGW)cfg.data();
        *startType = q->dwStartType;
        *disabled = (*startType == SERVICE_DISABLED);
        if (*startType == SERVICE_AUTO_START) {
            SERVICE_DELAYED_AUTO_START_INFO dsi = {};
            DWORD db = 0;
            if (QueryServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                     (LPBYTE)&dsi, sizeof(dsi), &db) && dsi.fDelayedAutostart)
                *delayed = true;
        }
        ok = true;
    } while (false);
    CloseServiceHandle(svc);
    return ok;
}

wstring StartTypeName(DWORD t) {
    switch (t) {
    case SERVICE_AUTO_START:  return L"自动";
    case SERVICE_DEMAND_START: return L"手动";
    case SERVICE_DISABLED:    return L"禁用";
    default:                  return L"未知";
    }
}
wstring SvcStateName(DWORD s) {
    switch (s) {
    case SERVICE_RUNNING:     return L"运行中";
    case SERVICE_STOPPED:     return L"已停止";
    case SERVICE_START_PENDING: return L"启动中";
    case SERVICE_STOP_PENDING:  return L"停止中";
    default:                  return L"其他";
    }
}

// 轮询服务进入目标状态（≤10s，sysmods 同款范式）
bool SvcPoll(SC_HANDLE svc, DWORD target) {
    for (int i = 0; i < 100; ++i) {
        SERVICE_STATUS st = {};
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == target) return true;
        Sleep(100);
    }
    return false;
}

// 配置启动类型（含延迟自动标志）
LONG SvcSetStart(SC_HANDLE svc, DWORD startType, bool delayed) {
    LONG r = (LONG)ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, startType,
                                        SERVICE_NO_CHANGE, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (r != 0 && startType == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO dsi = {};
        dsi.fDelayedAutostart = delayed ? TRUE : FALSE;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                              (LPBYTE)&dsi);
    }
    return r;
}

// ---------------------------------------------------------------- NLM COM
// 直接使用 MinGW 自带的 netlistmgr.h（真实 vtable/GUID，杜绝本地声明错位）。
// 之前本地声明版的三处接口继承关系猜错（INetworkListManager/INetwork 实为
// IDispatch 派生、IEnumNetworks 为 IUnknown 派生）导致 vtable 槽位错位 AV。

wstring NlmCatName(int c) {
    switch (c) {
    case NLM_NETWORK_CATEGORY_PRIVATE: return L"专用 (Private)";
    case NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED: return L"域 (Domain)";
    default:                            return L"公用 (Public)";
    }
}

// 遍历已连接网络：onNet(名字, 类别)；返回触达的已连接网络数（-1 = COM 不可用）
template <typename Fn>
int ForEachConnectedNetwork(Fn onNet) {
    int hit = 0;
    INetworkListManager* nlm = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetworkListManager), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(INetworkListManager),
                                  (void**)&nlm);
    if (FAILED(hr) || !nlm) return -1;
    do {
        IEnumNetworks* en = nullptr;
        if (FAILED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &en)) || !en) break;
        INetwork* net = nullptr;
        ULONG fetched = 0;
        while (en->Next(1, &net, &fetched) == S_OK && fetched == 1 && net) {
            BSTR name = nullptr;
            wstring nm;
            if (SUCCEEDED(net->GetName(&name)) && name) {
                nm = name; SysFreeString(name);
            }
            NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PUBLIC;
            net->GetCategory(&cat);
            onNet(nm, (int)cat);
            ++hit;
            net->Release();
        }
        en->Release();
    } while (false);
    nlm->Release();
    return hit;
}

// 已连接网络的当前类别（无网络/COM 失败返回 -1）
int FirstConnectedCategory(wstring* outName) {
    int cat = -1;
    ForEachConnectedNetwork([&](const wstring& nm, int c) {
        if (cat < 0) {
            cat = c;
            if (outName) *outName = nm;
        }
    });
    return cat;
}

// 把第一张已连接网络的类别改为 target（成功返回 true，log 收中文结果行）
bool SetFirstConnectedCategory(int target, wstring* log) {
    bool ok = false;
    ForEachConnectedNetwork([&](const wstring& nm, int cat) {
        if (ok) return;
        INetworkListManager* nlm = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr,
                                    CLSCTX_INPROC_SERVER, __uuidof(INetworkListManager),
                                    (void**)&nlm)) || !nlm) return;
        IEnumNetworks* en = nullptr;
        if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &en)) && en) {
            INetwork* net = nullptr;
            ULONG f = 0;
            if (en->Next(1, &net, &f) == S_OK && f == 1 && net) {
                if (SUCCEEDED(net->SetCategory((NLM_NETWORK_CATEGORY)target))) {
                    ok = true;
                    if (log)
                        *log = L"  ✓ 网络「" + nm + L"」→ " + NlmCatName(target);
                }
                net->Release();
            }
            en->Release();
        }
        nlm->Release();
    });
    return ok;
}

// ========================================================= net.sharehost ===

class ShareHostModule : public IModule {
public:
    const wchar_t* Id() const override { return L"net.sharehost"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"共享部署"; }
    const wchar_t* Description() const override {
        return L"共享环境一键体检/一键开通/恢复保守（服务+防火墙+网络配置+安全基线，🔴 项只读）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsElevation() const override { return true; }

    // ---------------- Run = 一键体检（§1.3 14 项）----------------
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        fwShareGroup_.clear();
        fwNdiscGroup_.clear();

        // COM 本线程初始化（防火墙 COM 探测与 NLM 网络配置文件都依赖；
        // 动作路径各自也有成对初始化，嵌套合法）
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInit = SUCCEEDED(hrCom);

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        int ready = 0, todo = 0, info = 0;
        auto row = [&](const wstring& c1, const wstring& c2, const wstring& c3,
                       const wstring& c4, bool ok, bool infoOnly, bool danger = false) {
            ModuleRow r;
            r.c1 = c1; r.c2 = c2; r.c3 = c3; r.c4 = c4;
            r.danger = danger;
            out.rows.push_back(r);
            if (infoOnly) ++info; else if (ok) ++ready; else ++todo;
        };

        // ---- 1) LanmanServer ----
        {
            DWORD state = 0, stype = 0; bool delayed = false, dis = false;
            bool q = scm && SvcQuery(scm, L"LanmanServer", &state, &stype, &delayed, &dis);
            wstring c2 = q ? (SvcStateName(state) + L" · " + StartTypeName(stype) +
                              (delayed ? L"(延迟)" : L""))
                           : L"读取失败";
            bool ok = q && state == SERVICE_RUNNING;
            row(L"Server 服务 (LanmanServer)", c2, L"服务",
                q ? (ok ? L"供他人访问本机共享，就绪" : L"未运行则他人无法访问本机共享，用「一键开通」修复")
                  : L"无法打开服务（可能需要管理员权限）",
                ok, false, q && dis);
        }
        // ---- 2) LanmanWorkstation ----
        {
            DWORD state = 0, stype = 0; bool delayed = false, dis = false;
            bool q = scm && SvcQuery(scm, L"LanmanWorkstation", &state, &stype, &delayed, &dis);
            wstring c2 = q ? (SvcStateName(state) + L" · " + StartTypeName(stype))
                           : L"读取失败";
            bool ok = q && state == SERVICE_RUNNING;
            row(L"Workstation 服务 (LanmanWorkstation)", c2, L"服务",
                ok ? L"访问他人共享的客户端侧，默认开启" : L"⚠ 未运行：本机将无法访问网络共享（异常，请检查）",
                ok, false, !ok);
        }
        // ---- 3) 网络发现四件套 ----
        {
            const wchar_t* four[] = { L"FDResPub", L"fdPHost", L"SSDPSRV", L"upnphost" };
            int running = 0;
            wstring detail;
            for (const wchar_t* s : four) {
                DWORD state = 0, stype = 0; bool d = false, dis = false;
                if (scm && SvcQuery(scm, s, &state, &stype, &d, &dis) &&
                    state == SERVICE_RUNNING) ++running;
                if (!detail.empty()) detail += L" / ";
                detail += wstring(s) + L"=" +
                          ((state == SERVICE_RUNNING) ? L"运行" : SvcStateName(state));
            }
            bool ok = running == 4;
            row(L"网络发现服务四件套", I64(running) + L"/4 运行中", L"服务",
                detail + (ok ? L" · 就绪" : L" · 缺席的服务让本机不出现在「网络」里，用「一键开通」修复"),
                ok, false);
            fourRunning_ = running;
        }
        // ---- 4) 防火墙「文件和打印机共享」规则组 ----
        {
            FwStatus fs = ProbeFwGroup(ctx, L"文件和打印机共享", L"File and Printer Sharing");
            if (fs.found) fwShareGroup_ = fs.matchedName;
            wstring c2 = fs.Text();
            bool ok = fs.found && fs.total > 0 && fs.enabled == fs.total;
            row(L"防火墙组：文件和打印机共享", c2, L"防火墙",
                fs.note + (fs.found ? L"（探测方式：" + fs.via + L"）" : L""),
                ok, false);
        }
        // ---- 5) 防火墙「网络发现」规则组（规则+服务合并判定）----
        {
            FwStatus fs = ProbeFwGroup(ctx, L"网络发现", L"Network Discovery");
            if (fs.found) fwNdiscGroup_ = fs.matchedName;
            bool rulesOk = fs.found && fs.total > 0 && fs.enabled == fs.total;
            bool ok = rulesOk && fourRunning_ == 4;
            wstring c2 = fs.Text() + L" · 服务 " + I64(fourRunning_) + L"/4";
            row(L"防火墙组：网络发现（含服务联动）", c2, L"防火墙",
                fs.note + L"｜只开规则不启动服务=假开启，本行为规则+服务合并判定",
                ok, false);
        }
        // ---- 6) 当前网络配置文件 ----
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            int cat = -1;
            wstring netName;
            if (com) {
                int hit = ForEachConnectedNetwork([&](const wstring& nm, int c) {
                    if (cat < 0) { cat = c; netName = nm; }
                });
                if (hit < 0) cat = -1;
                if (SUCCEEDED(hr)) CoUninitialize();
            }
            bool ok = (cat == NLM_NETWORK_CATEGORY_PRIVATE ||
                       cat == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED);
            wstring c2 = (cat < 0) ? L"读取失败" : NlmCatName(cat);
            row(L"网络配置文件",
                c2 + (netName.empty() ? L"" : L" · " + netName), L"网络",
                (cat < 0) ? L"Network List Manager 不可用（netprofm 服务？）"
                : (ok ? L"专用/域网络才建议开通共享" : L"公用网络下共享/发现规则组整组不生效，用「一键开通」切到专用"),
                ok, false);
        }
        // ---- 7) 来宾登录 AllowInsecureGuestAuth ----
        {
            DWORD v = 0; bool has = RegReadDwordV(HKEY_LOCAL_MACHINE, kLanmanWsParams,
                                                  L"AllowInsecureGuestAuth", &v);
            bool on = has && v == 1;
            row(L"不安全来宾登录 (AllowInsecureGuestAuth)",
                has ? (on ? L"已开启(=1)" : L"已关闭(=0)") : L"缺省(=0)",
                L"安全",
                has ? (on ? L"可匿名访问 NAS/老设备共享（SMB2 客户端侧）" : L"Win10 1709+ 默认关闭：匿名访问 NAS/老设备会被拒，共享场景用「一键开通」")
                    : L"键不存在=默认拒绝来宾登录，共享场景用「一键开通」",
                on, false);
        }
        // ---- 8) 匿名枚举 EveryoneIncludesAnonymous ----
        {
            DWORD v = 0; bool has = RegReadDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                                  L"EveryoneIncludesAnonymous", &v);
            bool on = has && v == 1;
            row(L"密码保护共享 (EveryoneIncludesAnonymous)",
                has ? (on ? L"已开启(=1)" : L"已关闭(=0)") : L"缺省(=0)",
                L"安全",
                on ? L"「关闭密码保护共享」效果：无账户设备也能访问共享（配合 Server 服务）"
                   : L"默认要求凭据访问共享，共享场景用「一键开通」",
                on, false);
        }
        // ---- 9-13) 组策略安全敏感项：体检展示 + 「更多 ▾」显式逐项编辑（v2.10）----
        {
            DWORD v = 0;
            bool has = RegReadDwordV(HKEY_LOCAL_MACHINE, kLsaKey, L"RestrictAnonymous", &v);
            row(L"RestrictAnonymous（匿名枚举限制·Server 侧）",
                has ? (v == 0 ? L"=0（已放宽）" : L"=" + I64(v)) : L"缺省(=1 严格)", L"组策略",
                L"对应 GPO「不允许 SAM 账户和共享的匿名枚举」；缺省/非 0 = 严格。"
                L"「更多 ▾」可放宽/恢复（危险操作，逐项确认）", v == 0, true, v == 0);
        }
        {
            DWORD v = 0;
            bool has = RegReadDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                     L"RestrictNullSessAccess", &v);
            row(L"RestrictNullSessAccess（空会话限制）",
                has ? (v == 0 ? L"=0（已放宽）" : L"=" + I64(v)) : L"缺省(=1 严格)", L"组策略",
                L"限制空会话可枚举的命名管道/共享；仅特殊遗留设备需要放宽。"
                L"「更多 ▾」可放宽/恢复（危险操作，逐项确认）", v == 0, true, v == 0);
        }
        {
            DWORD vc = 0, vs = 0;
            bool hc = RegReadDwordV(HKEY_LOCAL_MACHINE,
                                    L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Parameters",
                                    L"RequireSecuritySignature", &vc);
            bool hs = RegReadDwordV(HKEY_LOCAL_MACHINE,
                                    L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                                    L"RequireSecuritySignature", &vs);
            bool off = (hc && vc == 0) || (hs && vs == 0);
            row(L"SMB 签名 (RequireSecuritySignature)",
                L"客户端" + (hc ? L"=" + I64(vc) : wstring(L"缺省(1)")) +
                L" / 服务端" + (hs ? L"=" + I64(vs) : wstring(L"缺省(1)")),
                L"组策略",
                L"Win11 24H2 起默认强制签名；连老 NAS 报错时可临时降级（有中间人风险）。"
                L"「更多 ▾」可降级/恢复（危险操作，逐项确认）", !off, true, off);
        }
        {
            // HardenedPaths 键是否存在 + 值数量（组策略 UNC 加固）
            int n = -1;
            HKEY hk = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Policies\\Microsoft\\Windows\\NetworkProvider\\HardenedPaths",
                              0, KEY_READ, &hk) == ERROR_SUCCESS) {
                DWORD sub = 0, vals = 0;
                if (RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr, &sub, nullptr,
                                     nullptr, &vals, nullptr, nullptr, nullptr, nullptr)
                    == ERROR_SUCCESS) n = (int)vals;
                RegCloseKey(hk);
            }
            row(L"组策略 HardenedPaths（UNC 加固）",
                n < 0 ? L"未配置" : (n == 0 ? L"已配置(空)" : I64(n) + L" 条"),
                L"组策略",
                L"限制特定 UNC 路径强制走 MSAUTH/Kerberos（域加固策略）。"
                L"「更多 ▾」可清除本机策略副本（域 GPO 会刷回，危险操作）", n <= 0, true, n <= 0);
        }
        {
            // LanmanWorkstation\Policies 系位（GPO 覆盖位，只读展示键存在性）
            HKEY hk = nullptr;
            bool has = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                     L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Policies",
                                     0, KEY_READ, &hk) == ERROR_SUCCESS;
            int vals = 0;
            if (has) {
                DWORD v = 0;
                RegQueryInfoKeyW(hk, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, (LPDWORD)&vals, nullptr, nullptr, nullptr, nullptr);
                RegCloseKey(hk);
            }
            row(L"组策略 LanmanWorkstation\\Policies（域覆盖位）",
                has ? I64((long long)vals) + L" 个值" : L"未配置", L"组策略",
                L"域环境 GPO 会刷回手工修改，本工具只读展示不做编辑", true, true);
        }
        // ---- 14) SMB1（只读：MRxSmb10 服务启动类型判；只读展示）----
        {
            // 先探测键是否存在，区分「组件未安装」与「读取失败」
            HKEY hk = nullptr;
            LSTATUS kst = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMrxSmb10, 0,
                                        KEY_READ, &hk);
            bool keyMissing = (kst == ERROR_FILE_NOT_FOUND);
            if (kst == ERROR_SUCCESS) RegCloseKey(hk);
            DWORD v = 0;
            bool has = !keyMissing &&
                       RegReadDwordV(HKEY_LOCAL_MACHINE, kMrxSmb10, L"Start", &v);
            wstring c2 = has ? (v == 4 ? L"已禁用" : L"已启用")
                             : (keyMissing ? L"组件未安装（视为已禁用）" : L"无法检测");
            row(L"SMB1 (minisrv/client 可选组件)", c2, L"仅展示",
                has ? (v == 4 ? L"🔴 已禁用（推荐保持，安全基线）" : L"🔴 已启用：WannaCry 级历史漏洞面，建议在「启用或关闭 Windows 功能」中关闭（本工具只读不写）")
                : keyMissing
                    ? L"SMB1 组件未安装（MRxSmb10 驱动键不存在），视为安全状态；如需精确核对可用 DISM /Online /Get-FeatureInfo /FeatureName:SMB1Protocol"
                    : L"🔴 无法检测（注册表读取失败），可用 DISM /Online /Get-FeatureInfo /FeatureName:SMB1Protocol 人工核对",
                (has && v == 4) || keyMissing, true, has && v != 4);
        }

        if (scm) CloseServiceHandle(scm);
        if (comInit) CoUninitialize();

        if (todo == 0)
            out.summary = L"本机共享环境已就绪（" + I64(ready) + L" 项就绪 · " +
                          I64(info) + L" 项仅展示）";
        else
            out.summary = I64(ready) + L" 就绪 / " + I64(todo) + L" 待处理 / " +
                          I64(info) + L" 仅展示";
        out.hint = wstring(T(L"动作：①一键开通共享环境 ②恢复保守设置；「更多 ▾」= 分项开关 + "))
                   + T(L"组策略安全项逐项编辑（红色危险项，逐项确认）。");
        out.canClean = true;
        ctx.Report(100, L"共享环境体检完成");
    }

    // ---------------- 动作表 ----------------
    vector<ModuleAction> Actions() const override {
        return {
            { L"一键开通共享环境", nullptr, true, 0xE73E },   // Install（danger）
            { L"恢复保守设置", nullptr, true, 0xE72C },       // Undo（danger）
            { L"停用 Server 服务",
              L"将停止 LanmanServer（仅停止、不改启动类型）：他人将暂时无法访问本机共享。\n\n是否继续？",
              false, 0xE769 },   // Pause
            { L"启动 Server 服务",
              L"将启动 LanmanServer（不改启动类型）。\n\n是否继续？",
              false, 0xE768 },
            { L"开启来宾登录",
              L"将设置 AllowInsecureGuestAuth=1：允许匿名访问 NAS/老设备的共享。\n"
              L"⚠ 这会放宽 SMB2 客户端安全基线，仅建议可信内网使用。\n\n是否继续？",
              true, 0xE7EF },
            { L"关闭来宾登录",
              L"将删除 AllowInsecureGuestAuth 值（恢复默认拒绝来宾登录）。\n\n是否继续？",
              false, 0xE72E },
            { L"开启匿名枚举",
              L"将设置 EveryoneIncludesAnonymous=1（「关闭密码保护共享」效果）。\n"
              L"⚠ 无账户设备也能枚举/访问本机共享，暴露面增大。\n\n是否继续？",
              true, 0xE7EF },
            { L"关闭匿名枚举",
              L"将设置 EveryoneIncludesAnonymous=0（恢复要求凭据访问）。\n\n是否继续？",
              false, 0xE72E },
            { L"切换网络配置文件",
              L"将在 专用(Private) ↔ 公用(Public) 之间切换当前网络：\n"
              L"  · 切到公用：本机在网络中不可发现，共享/发现规则组整组不生效\n"
              L"  · 切到专用：仅在可信的家庭/办公网使用\n\n是否继续？",
              true, 0xE968 },
            { L"仅刷新状态",
              L"重新执行 14 项体检（只读，不修改任何设置）。",
              false, 0xE895 },
            // ---- v2.10：组策略安全敏感项逐项编辑（danger + 独立确认框）----
            { L"组策略：放宽匿名枚举限制",
              L"将设置 RestrictAnonymous=0（相当于取消 GPO「不允许 SAM 账户和共享的匿名枚举」）。\n"
              L"⚠ 匿名用户将能列出本机共享清单，暴露面增大；仅建议隔离内网使用。\n\n是否继续？",
              true, 0xE7EF },
            { L"组策略：恢复匿名枚举限制",
              L"将设置 RestrictAnonymous=1（恢复默认：禁止匿名枚举共享清单）。\n\n是否继续？",
              false, 0xE72E },
            { L"组策略：降级 SMB 签名",
              L"将把客户端与服务端的 RequireSecuritySignature 均设为 0（关闭 SMB 签名强制）。\n"
              L"⚠ 报文可被中间人篡改；仅用于连接不支持签名的老设备，用完请恢复。\n\n是否继续？",
              true, 0xE7EF },
            { L"组策略：恢复 SMB 签名",
              L"将把客户端与服务端的 RequireSecuritySignature 均设为 1（恢复签名强制，安全默认）。\n\n是否继续？",
              false, 0xE72E },
            { L"组策略：放宽空会话限制",
              L"将设置 RestrictNullSessAccess=0（允许空会话枚举命名管道/共享）。\n"
              L"⚠ 仅特殊遗留设备需要；绝大多数环境不应放宽。\n\n是否继续？",
              true, 0xE7EF },
            { L"组策略：恢复空会话限制",
              L"将设置 RestrictNullSessAccess=1（恢复默认：限制空会话）。\n\n是否继续？",
              false, 0xE72E },
            { L"组策略：清除 UNC 加固策略",
              L"将删除注册表策略键 HardenedPaths（GPO「加固 UNC 路径」的本机策略副本）。\n"
              L"⚠ 域环境下下次策略刷新会被 GPO 刷回；删除后特定 UNC 不再强制 Kerberos/MSAUTH。\n\n是否继续？",
              true, 0xE7EF },
            { L"组策略：恢复默认安全基线",
              L"将批量恢复安全默认值（幂等，已符合的项跳过）：\n"
              L"  · RestrictAnonymous=1（禁止匿名枚举）\n"
              L"  · RestrictNullSessAccess=1（限制空会话）\n"
              L"  · SMB 签名：客户端/服务端 =1（签名强制）\n"
              L"  （HardenedPaths 不动；域 GPO 下发值以域策略为准）\n\n是否继续？",
              true, 0xE72C },
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)tags;
        if (actionIndex == 0) {
            return L"将批量修改以下 🟡 项（🔴 安全敏感项永不触碰）：\n"
                   L"  1. Server 服务 → 自动启动并启动\n"
                   L"  2. 网络发现四件套 → 自动(延迟) 并启动\n"
                   L"  3. 防火墙「文件和打印机共享」「网络发现」规则组 → 启用\n"
                   L"  4. 网络配置文件 → 公用时切为专用\n"
                   L"  5. AllowInsecureGuestAuth → 1（允许来宾访问 NAS/老设备）\n"
                   L"  6. EveryoneIncludesAnonymous → 1（无账户设备可访问共享）\n\n"
                   L"⚠ 仅建议在可信的家庭/办公网执行；执行中可随时 Esc/停止中止。\n\n是否继续？";
        }
        if (actionIndex == 1) {
            return L"将恢复保守设置（反向关闭 🟡 项）：\n"
                   L"  1. Server 服务 → 手动并停止\n"
                   L"  2. 网络发现四件套 → 手动并停止\n"
                   L"  3. 两个防火墙规则组 → 禁用\n"
                   L"  4. AllowInsecureGuestAuth → 删除值（恢复默认）\n"
                   L"  5. EveryoneIncludesAnonymous → 0\n"
                   L"  （网络配置文件不改动，避免网络断连感知）\n\n"
                   L"恢复后本机共享能力回到 Windows 默认状态。\n\n是否继续？";
        }
        return L"";   // 其余动作走 Actions() 静态 warning
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        if (actionIndex < 0 || actionIndex >= (int)acts.size()) return;
        const wstring label = acts[actionIndex].label;
        // 动作 9「仅刷新状态」只读放行；其余全部要求提权
        if (actionIndex != 9 && !RequireElevation(ctx, label, out)) return;

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                       SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
        int okN = 0, failN = 0, skipN = 0;
        auto tally = [&](bool ok, bool skipped, const wstring& msg) {
            if (skipped) { ++skipN; ctx.Log(L"  ○ 跳过: " + msg); }
            else if (ok) { ++okN; ctx.Log(L"  ✓ " + msg); }
            else { ++failN; ctx.Log(L"  ✗ " + msg); }
        };

        switch (actionIndex) {
        case 0: DoOpenUp(ctx, scm, tally); break;
        case 1: DoRestore(ctx, scm, tally); break;
        case 2: {   // 停用 Server（仅停用，不改启动类型）
            tally(SvcStartStop(scm, L"LanmanServer", false),
                  false, L"停止 Server 服务 (LanmanServer)");
            break;
        }
        case 3: {   // 启动 Server
            tally(SvcStartStop(scm, L"LanmanServer", true),
                  false, L"启动 Server 服务 (LanmanServer)");
            break;
        }
        case 4: {   // 开启来宾登录
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLanmanWsParams,
                                    L"AllowInsecureGuestAuth", 1);
            tally(r == ERROR_SUCCESS, false,
                  L"AllowInsecureGuestAuth=1（允许来宾登录）" +
                  (r == ERROR_SUCCESS ? wstring() : L" — " + ErrorTextZh((DWORD)r)));
            break;
        }
        case 5: {   // 关闭来宾登录（删值恢复默认）
            LONG r = RegDeleteValueV(HKEY_LOCAL_MACHINE, kLanmanWsParams,
                                     L"AllowInsecureGuestAuth");
            tally(r == ERROR_SUCCESS, false,
                  L"删除 AllowInsecureGuestAuth（恢复默认拒绝来宾）" +
                  (r == ERROR_SUCCESS ? wstring() : L" — " + ErrorTextZh((DWORD)r)));
            break;
        }
        case 6: {   // 开启匿名枚举
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                    L"EveryoneIncludesAnonymous", 1);
            tally(r == ERROR_SUCCESS, false,
                  L"EveryoneIncludesAnonymous=1（关闭密码保护共享）" +
                  (r == ERROR_SUCCESS ? wstring() : L" — " + ErrorTextZh((DWORD)r)));
            break;
        }
        case 7: {   // 关闭匿名枚举
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                    L"EveryoneIncludesAnonymous", 0);
            tally(r == ERROR_SUCCESS, false,
                  L"EveryoneIncludesAnonymous=0（恢复要求凭据）" +
                  (r == ERROR_SUCCESS ? wstring() : L" — " + ErrorTextZh((DWORD)r)));
            break;
        }
        case 8: {   // 切换网络配置文件 Private↔Public
            DoSwitchProfile(ctx, tally);
            break;
        }
        case 9:     // 仅刷新状态（Run 由 GUI/CLI 在动作后自动重扫触发；此处直接重扫一次）
            Run(ctx, out);
            return;
        // ---- v2.10：组策略安全敏感项逐项编辑（幂等：已是目标值则跳过）----
        case 10:   // 放宽匿名枚举 RestrictAnonymous=0
            RegSetChecked(ctx, kLsaKey, L"RestrictAnonymous", 0,
                          L"RestrictAnonymous=0（允许匿名枚举共享清单）", tally);
            break;
        case 11:   // 恢复匿名枚举限制 =1
            RegSetChecked(ctx, kLsaKey, L"RestrictAnonymous", 1,
                          L"RestrictAnonymous=1（禁止匿名枚举）", tally);
            break;
        case 12:   // 降级 SMB 签名（客户端 + 服务端 =0）
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Parameters",
                          L"RequireSecuritySignature", 0,
                          L"SMB 签名·客户端 =0", tally);
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                          L"RequireSecuritySignature", 0,
                          L"SMB 签名·服务端 =0", tally);
            break;
        case 13:   // 恢复 SMB 签名（两端 =1）
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Parameters",
                          L"RequireSecuritySignature", 1,
                          L"SMB 签名·客户端 =1", tally);
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                          L"RequireSecuritySignature", 1,
                          L"SMB 签名·服务端 =1", tally);
            break;
        case 14:   // 放宽空会话限制 =0
            RegSetChecked(ctx, kLsaKey, L"RestrictNullSessAccess", 0,
                          L"RestrictNullSessAccess=0（放宽空会话）", tally);
            break;
        case 15:   // 恢复空会话限制 =1
            RegSetChecked(ctx, kLsaKey, L"RestrictNullSessAccess", 1,
                          L"RestrictNullSessAccess=1（限制空会话）", tally);
            break;
        case 16: {  // 清除 UNC 加固策略：删除 HardenedPaths 策略键
            HKEY hk = nullptr;
            LONG st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Policies\\Microsoft\\Windows\\NetworkProvider\\HardenedPaths",
                                    0, KEY_READ, &hk);
            if (st == ERROR_FILE_NOT_FOUND) {
                tally(false, true, L"HardenedPaths 未配置（无需清除）");
            } else {
                if (st == ERROR_SUCCESS) RegCloseKey(hk);
                LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Policies\\Microsoft\\Windows\\NetworkProvider\\HardenedPaths");
                tally(r == ERROR_SUCCESS, false,
                      wstring(L"删除策略键 HardenedPaths") +
                      (r == ERROR_SUCCESS ? L"（域 GPO 刷新后可能刷回）"
                                          : L" — " + ErrorTextZh((DWORD)r)));
            }
            break;
        }
        case 17:   // 恢复默认安全基线（幂等批量）
            RegSetChecked(ctx, kLsaKey, L"RestrictAnonymous", 1,
                          L"基线：RestrictAnonymous=1", tally);
            RegSetChecked(ctx, kLsaKey, L"RestrictNullSessAccess", 1,
                          L"基线：RestrictNullSessAccess=1", tally);
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\Parameters",
                          L"RequireSecuritySignature", 1,
                          L"基线：SMB 签名·客户端 =1", tally);
            RegSetChecked(ctx,
                          L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                          L"RequireSecuritySignature", 1,
                          L"基线：SMB 签名·服务端 =1", tally);
            ctx.Log(L"  ℹ HardenedPaths 未触碰；域环境以 GPO 下发值为准");
            break;
        }

        out.summary = L"「" + label + L"」完成：成功 " + I64(okN) +
                      L"，跳过 " + I64(skipN) + L"，失败 " + I64(failN);
        out.canClean = true;
    }

private:
    wstring fwShareGroup_, fwNdiscGroup_;   // 最近一次探测到的本地化组名
    int fourRunning_ = 0;                   // 四件套运行数（Run 时更新）

    // ---------------- 防火墙规则组探测 ----------------
    struct FwStatus {
        bool found = false;
        int total = 0, enabled = 0;
        wstring matchedName;   // 探测到规则的组名（中/英）
        wstring via;           // netsh / 注册表兜底
        wstring note;

        wstring Text() const {
            if (!found) return L"未找到规则组";
            if (total == 0) return L"无规则";
            if (enabled == total) return L"已开启 (" + I64(enabled) + L"/" + I64(total) + L")";
            if (enabled == 0) return L"已关闭 (0/" + I64(total) + L")";
            return L"部分开启 (" + I64(enabled) + L"/" + I64(total) + L")";
        }
    };

    // netsh 解析一行「已启用/Enabled」值行
    static bool ParseEnabledLine(const wstring& line, bool* on) {
        bool isEn = ContainsI(line, L"已启用") || ContainsI(line, L"Enabled");
        if (!isEn || line.find(L':') == wstring::npos) return false;
        if (ContainsI(line, L"是") || ContainsI(line, L"yes")) { *on = true; return true; }
        if (ContainsI(line, L"否") || ContainsI(line, L"no")) { *on = false; return true; }
        return false;
    }

    // procrun 跑 netsh（命令原文入日志；logLines=false 时只记命令与退出码，
    // 全量输出进 full——show rule 全量 dump 数千行，不刷日志面板）
    int NetshRun(ModuleContext& ctx, const wstring& args, wstring* full,
                 bool logLines = true) {
        wstring cmd = L"netsh advfirewall firewall " + args;
        ctx.Log(L"  $ " + cmd);
        ProcRunOptions opt;
        opt.timeoutMs = 60000;
        opt.cancel = [&ctx]() { return ctx.Cancelled(); };
        if (logLines)
            opt.onLine = [&ctx](const wstring& line) {
                if (!line.empty()) ctx.Log(L"    " + line);
            };
        return RunProcess(cmd, opt, full);
    }

    // 主探测：注册表 FirewallRules 的 Group= 字段（语言无关，且不受 show rule
    // 不支持 group= 参数的新版 netsh 影响）。资源串引用经 SHLoadIndirectString
    // 解析为本地化组名后与目标匹配，不依赖具体资源下标。
    FwStatus ProbeFwGroupRegistry(const wstring& cnName, const wstring& enName) {
        FwStatus fs;
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kFwRulesKey, 0, KEY_READ, &hk)
            != ERROR_SUCCESS) return fs;
        std::map<wstring, std::pair<int, int>> groups;   // group token → (total, enabled)
        wchar_t valName[256];
        wchar_t data[8192];
        for (DWORD i = 0;; ++i) {
            DWORD vn = 256, dn = 8192, type = 0;
            LONG r = RegEnumValueW(hk, i, valName, &vn, nullptr, &type,
                                   (LPBYTE)data, &dn);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS) continue;   // 单条失败跳过
            if (type != REG_SZ) continue;
            wstring rule(data, dn / sizeof(wchar_t));
            size_t gp = rule.find(L"|Group=");
            if (gp == wstring::npos) continue;
            size_t gs = gp + 7, ge = rule.find(L'|', gs);
            wstring token = rule.substr(gs, ge == wstring::npos ? wstring::npos
                                                                : ge - gs);
            if (token.empty()) continue;
            bool on = ContainsI(rule, L"Enabled=TRUE");
            std::pair<int, int>& bucket = groups[token];
            ++bucket.first;
            if (on) ++bucket.second;
        }
        RegCloseKey(hk);

        for (auto& kv : groups) {
            wchar_t loc[512] = {};
            if (FAILED(SHLoadIndirectString(kv.first.c_str(), loc, 512, nullptr)))
                continue;
            wstring s = loc;
            if (!ContainsI(s, cnName) && !ContainsI(s, enName)) continue;
            fs.found = true;
            fs.via = L"注册表 Grouping 字段";
            fs.total = kv.second.first;
            fs.enabled = kv.second.second;
            fs.matchedName = s;   // 本地化组名，可直接回填 netsh set rule group=
            break;
        }
        return fs;
    }

    // 兜底探测：netsh show rule name=all 全量 dump，按「组名:/Group Name:」行
    // 分段统计已启用数（本机构建的 show rule 不支持 group= 参数，实测）
    FwStatus ProbeFwGroupNetshDump(ModuleContext& ctx, const wstring& cnName,
                                   const wstring& enName) {
        FwStatus fs;
        wstring full;
        int code = NetshRun(ctx, L"show rule name=all", &full, false);
        if (code != 0 || full.empty()) return fs;
        wstring curGroup;
        bool curHit = false;
        bool on = false;
        size_t pos = 0;
        while (pos <= full.size()) {
            size_t e = full.find(L'\n', pos);
            wstring line = full.substr(pos, e == wstring::npos ? wstring::npos
                                                               : e - pos);
            size_t colon = line.find(L':');
            if (colon != wstring::npos) {
                wstring field = line.substr(0, colon);
                wstring value = line.substr(colon + 1);
                // 去首尾空白
                while (!value.empty() && (value.front() == L' ' || value.front() == L'\r'))
                    value.erase(value.begin());
                while (!value.empty() && value.back() == L' ') value.pop_back();
                if (ContainsI(field, L"组名") || ContainsI(field, L"Group Name")) {
                    curGroup = value;
                    curHit = ContainsI(value, cnName) || ContainsI(value, enName);
                } else if (ParseEnabledLine(line, &on)) {
                    if (curHit) { ++fs.total; if (on) ++fs.enabled; }
                }
            }
            if (e == wstring::npos) break;
            pos = e + 1;
        }
        if (fs.total > 0) {
            fs.found = true;
            fs.via = L"netsh 全量解析";
            fs.matchedName = curGroup.empty() ? cnName : curGroup;
        }
        return fs;
    }

    // 权威探测：INetFwPolicy2 COM 规则枚举（语言无关：Grouping 资源串经
    // SHLoadIndirectString 解析为本地化组名后与目标匹配，不依赖 netsh 输出格式）
    FwStatus ProbeFwGroupCom(const wstring& cnName, const wstring& enName) {
        FwStatus fs;
        INetFwPolicy2* pol = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                      CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                      (void**)&pol);
        if (FAILED(hr) || !pol) return fs;
        do {
            INetFwRules* rules = nullptr;
            if (FAILED(pol->get_Rules(&rules)) || !rules) break;
            IUnknown* unk = nullptr;
            if (FAILED(rules->get__NewEnum(&unk)) || !unk) { rules->Release(); break; }
            IEnumVARIANT* en = nullptr;
            if (FAILED(unk->QueryInterface(IID_IEnumVARIANT, (void**)&en)) || !en) {
                unk->Release(); rules->Release(); break;
            }
            VARIANT v;
            ULONG f = 0;
            while (en->Next(1, &v, &f) == S_OK && f == 1) {
                if (v.vt == VT_DISPATCH && v.pdispVal) {
                    INetFwRule* rule = nullptr;
                    if (SUCCEEDED(v.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                             (void**)&rule)) && rule) {
                        BSTR grp = nullptr;
                        if (SUCCEEDED(rule->get_Grouping(&grp)) && grp) {
                            wchar_t loc[512] = {};
                            if (SUCCEEDED(SHLoadIndirectString(grp, loc, 512, nullptr))) {
                                wstring s = loc;
                                if (ContainsI(s, cnName) || ContainsI(s, enName)) {
                                    VARIANT_BOOL enb = VARIANT_FALSE;
                                    rule->get_Enabled(&enb);
                                    ++fs.total;
                                    if (enb != VARIANT_FALSE) ++fs.enabled;
                                    if (fs.matchedName.empty()) fs.matchedName = s;
                                }
                            }
                            SysFreeString(grp);
                        }
                        rule->Release();
                    }
                }
                VariantClear(&v);
            }
            en->Release();
            unk->Release();
            rules->Release();
            if (fs.total > 0) { fs.found = true; fs.via = L"INetFwPolicy2 COM"; }
        } while (false);
        pol->Release();
        return fs;
    }

    // 组装探测：INetFwPolicy2 COM 权威 → 注册表 Grouping 兜底 → netsh 全量解析
    FwStatus ProbeFwGroup(ModuleContext& ctx, const wstring& cnName,
                          const wstring& enName) {
        FwStatus fs = ProbeFwGroupCom(cnName, enName);
        if (fs.found) return fs;
        ctx.Log(L"  INetFwPolicy2 未命中，改用注册表 Grouping 兜底（只读）");
        fs = ProbeFwGroupRegistry(cnName, enName);
        if (fs.found) return fs;
        ctx.Log(L"  注册表 Grouping 未命中，改用 netsh 全量解析兜底（只读）");
        return ProbeFwGroupNetshDump(ctx, cnName, enName);
    }

    // ---------------- 服务启停（错误码 → 中文）----------------
    bool SvcStartStop(SC_HANDLE scm, const wchar_t* name, bool start) {
        SC_HANDLE svc = OpenServiceW(scm, name,
                                     SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!svc) return false;
        bool ok = false;
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(svc, &st)) {
            if (start && st.dwCurrentState == SERVICE_RUNNING) {
                ok = true;   // 已在运行 = 成功
            } else if (!start && st.dwCurrentState == SERVICE_STOPPED) {
                ok = true;
            } else if (start) {
                ok = StartServiceW(svc, 0, nullptr) && SvcPoll(svc, SERVICE_RUNNING);
            } else {
                ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
                     SvcPoll(svc, SERVICE_STOPPED);
            }
        }
        CloseServiceHandle(svc);
        return ok;
    }

    // ---------------- 一键开通 / 恢复保守 ----------------
    using TallyFn = std::function<void(bool, bool, const wstring&)>;

    // 组策略 DWORD 编辑（v2.10）：读当前值，已是目标值则跳过（幂等），
    // 否则写入并按结果 tally。带跳过/失败中文日志。
    void RegSetChecked(ModuleContext& ctx, const wchar_t* sub, const wchar_t* name,
                       DWORD target, const wstring& label, const TallyFn& tally) {
        DWORD cur = 0;
        bool has = RegReadDwordV(HKEY_LOCAL_MACHINE, sub, name, &cur);
        if (has && cur == target) {
            tally(false, true, label + L"（已是目标值）");
            return;
        }
        LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, sub, name, target);
        tally(r == ERROR_SUCCESS, false,
              label + (r == ERROR_SUCCESS ? wstring()
                                          : L" — " + ErrorTextZh((DWORD)r)));
    }

    void DoOpenUp(ModuleContext& ctx, SC_HANDLE scm, const TallyFn& tally) {
        ctx.Report(-1, L"阶段：批量开通共享环境（逐项日志）");

        // 1) LanmanServer：自动 + 启动
        {
            SC_HANDLE svc = OpenServiceW(scm, L"LanmanServer",
                                         SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
            bool ok = false, skip = false;
            if (svc) {
                ok = SvcSetStart(svc, SERVICE_AUTO_START, false) != 0;
                SERVICE_STATUS st = {};
                if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
                    skip = true;
                } else {
                    ok = StartServiceW(svc, 0, nullptr) && SvcPoll(svc, SERVICE_RUNNING);
                }
                CloseServiceHandle(svc);
            }
            tally(ok, skip, L"Server 服务 → 自动启动并运行");
        }
        // 2) 四件套：自动(延迟) + 启动（依赖顺序：FDResPub/fdPHost 先，SSDPSRV/upnphost 后）
        {
            const wchar_t* order[] = { L"FDResPub", L"fdPHost", L"SSDPSRV", L"upnphost" };
            for (const wchar_t* s : order) {
                SC_HANDLE svc = OpenServiceW(scm, s,
                                             SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
                bool ok = false, skip = false;
                if (svc) {
                    ok = SvcSetStart(svc, SERVICE_AUTO_START, true) != 0;
                    SERVICE_STATUS st = {};
                    if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
                        skip = true;
                    } else {
                        ok = StartServiceW(svc, 0, nullptr) && SvcPoll(svc, SERVICE_RUNNING);
                    }
                    CloseServiceHandle(svc);
                }
                tally(ok, skip, wstring(L"网络发现服务 ") + s + L" → 自动(延迟) 并运行");
                if (ctx.Cancelled()) { ctx.Log(L"  ⚠ 用户中止，剩余项未执行"); return; }
            }
        }
        // 3) 两个防火墙规则组 enable=Yes（用探测到的本地化组名；未探测到用中文名）
        {
            const wchar_t* defs[] = { L"文件和打印机共享", L"网络发现" };
            wstring* cached[] = { &fwShareGroup_, &fwNdiscGroup_ };
            for (int i = 0; i < 2; ++i) {
                wstring g = cached[i]->empty() ? wstring(defs[i]) : *cached[i];
                wstring full;
                int code = NetshRun(ctx, L"set rule group=\"" + g + L"\" new enable=Yes", &full);
                tally(code == 0, false,
                      L"防火墙规则组「" + g + L"」→ 启用" +
                      (code == 0 ? wstring() : L"（netsh 退出码 " + I64(code) + L"）"));
                if (ctx.Cancelled()) { ctx.Log(L"  ⚠ 用户中止，剩余项未执行"); return; }
            }
        }
        // 4) 网络配置文件：公时 → 专用
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
                wstring netName;
                int cat = FirstConnectedCategory(&netName);
                if (cat < 0) {
                    tally(false, false, L"网络配置文件切换（Network List Manager 不可用）");
                } else if (cat != NLM_NETWORK_CATEGORY_PUBLIC) {
                    tally(false, true, L"网络配置文件已是专用/域（无需切换）");
                } else {
                    wstring log;
                    bool ok = SetFirstConnectedCategory(NLM_NETWORK_CATEGORY_PRIVATE, &log);
                    if (ok && !log.empty()) ctx.Log(log);
                    tally(ok, false, L"网络配置文件 公用→专用");
                }
                CoUninitialize();
            } else {
                tally(false, false, L"网络配置文件切换（COM 初始化失败）");
            }
        }
        // 5) AllowInsecureGuestAuth=1
        {
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLanmanWsParams,
                                    L"AllowInsecureGuestAuth", 1);
            tally(r == ERROR_SUCCESS, false, L"AllowInsecureGuestAuth=1（允许来宾登录）");
        }
        // 6) EveryoneIncludesAnonymous=1
        {
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                    L"EveryoneIncludesAnonymous", 1);
            tally(r == ERROR_SUCCESS, false, L"EveryoneIncludesAnonymous=1（关闭密码保护共享）");
        }
        ctx.Log(L"  ℹ 🔴 安全敏感项（RestrictAnonymous/SMB 签名/HardenedPaths/SMB1）未被触碰");
    }

    void DoRestore(ModuleContext& ctx, SC_HANDLE scm, const TallyFn& tally) {
        ctx.Report(-1, L"阶段：恢复保守设置（逐项日志）");

        // 1) LanmanServer：手动 + 停止
        {
            SC_HANDLE svc = OpenServiceW(scm, L"LanmanServer",
                                         SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS);
            bool ok = false, skip = false;
            if (svc) {
                ok = SvcSetStart(svc, SERVICE_DEMAND_START, false) != 0;
                SERVICE_STATUS st = {};
                if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED) {
                    skip = true;
                } else {
                    ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
                         SvcPoll(svc, SERVICE_STOPPED);
                }
                CloseServiceHandle(svc);
            }
            tally(ok, skip, L"Server 服务 → 手动并停止");
        }
        // 2) 四件套：手动 + 停止（依赖逆序：upnphost/SSDPSRV 先，FDResPub/fdPHost 后）
        {
            const wchar_t* order[] = { L"upnphost", L"SSDPSRV", L"FDResPub", L"fdPHost" };
            for (const wchar_t* s : order) {
                SC_HANDLE svc = OpenServiceW(scm, s,
                                             SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS);
                bool ok = false, skip = false;
                if (svc) {
                    ok = SvcSetStart(svc, SERVICE_DEMAND_START, false) != 0;
                    SERVICE_STATUS st = {};
                    if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED) {
                        skip = true;
                    } else {
                        ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
                             SvcPoll(svc, SERVICE_STOPPED);
                    }
                    CloseServiceHandle(svc);
                }
                tally(ok, skip, wstring(L"网络发现服务 ") + s + L" → 手动并停止");
                if (ctx.Cancelled()) { ctx.Log(L"  ⚠ 用户中止，剩余项未执行"); return; }
            }
        }
        // 3) 规则组 enable=No
        {
            const wchar_t* defs[] = { L"文件和打印机共享", L"网络发现" };
            wstring* cached[] = { &fwShareGroup_, &fwNdiscGroup_ };
            for (int i = 0; i < 2; ++i) {
                wstring g = cached[i]->empty() ? wstring(defs[i]) : *cached[i];
                wstring full;
                int code = NetshRun(ctx, L"set rule group=\"" + g + L"\" new enable=No", &full);
                tally(code == 0, false,
                      L"防火墙规则组「" + g + L"」→ 禁用" +
                      (code == 0 ? wstring() : L"（netsh 退出码 " + I64(code) + L"）"));
            }
        }
        // 4) AllowInsecureGuestAuth 删值
        {
            LONG r = RegDeleteValueV(HKEY_LOCAL_MACHINE, kLanmanWsParams,
                                     L"AllowInsecureGuestAuth");
            tally(r == ERROR_SUCCESS, false, L"删除 AllowInsecureGuestAuth（恢复默认）");
        }
        // 5) EveryoneIncludesAnonymous=0
        {
            LONG r = RegWriteDwordV(HKEY_LOCAL_MACHINE, kLsaKey,
                                    L"EveryoneIncludesAnonymous", 0);
            tally(r == ERROR_SUCCESS, false, L"EveryoneIncludesAnonymous=0（要求凭据访问）");
        }
        ctx.Log(L"  ℹ 网络配置文件未改动（避免网络断连感知）");
    }

    void DoSwitchProfile(ModuleContext& ctx, const TallyFn& tally) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            tally(false, false, L"网络配置文件切换（COM 初始化失败）");
            return;
        }
        wstring netName;
        int cat = FirstConnectedCategory(&netName);
        if (cat < 0) {
            tally(false, true, L"切换网络配置文件（无已连接网络或 NLM 不可用，跳过）");
        } else {
            int target = (cat == NLM_NETWORK_CATEGORY_PUBLIC)
                             ? NLM_NETWORK_CATEGORY_PRIVATE
                             : NLM_NETWORK_CATEGORY_PUBLIC;
            wstring log;
            bool ok = SetFirstConnectedCategory(target, &log);
            if (ok && !log.empty()) ctx.Log(log);
            tally(ok, false, L"切换网络配置文件（Private↔Public）");
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

REGISTER_MODULE(ShareHostModule);

} // namespace
} // namespace mz
