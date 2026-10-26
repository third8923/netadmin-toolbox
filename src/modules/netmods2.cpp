// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 网络管理模块集二期（v2.15 需求 2）
//  ---------------------------------------------------------------------------
//  net.dnsopt  DNS 优化（Tweaks + Actions 组合，参照 purg.scan 范式）：
//    内置 9 家公共 DNS（阿里/DNSPod/114/百度/360/Google/Cloudflare/Quad9/
//    OpenDNS）；自实现 UDP DNS 探测（12 字节头 + A 查询 www.baidu.com，
//    sendto + select 1s 超时，测 3 次取最小延迟——不依赖 dnsapi 以便超时可控）；
//    列表展示 名称/IP/延迟/状态 按延迟排序；「自动优选」取最快两个设主/备；
//    「应用选中」把勾选的两个 IP 经 PowerShell Set-DnsClientServerAddress
//    应用到指定网卡（不走 netsh——部分环境被策略限制）；同时展示当前各网卡
//    DNS 配置；支持还原为自动获取（DHCP 下发）。
//  链接：-lws2_32 -liphlpapi（build.bat 已统一追加）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../procrun.h"
#include "../cleanresult.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace mz {
namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32s2(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring FoldLow2(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

// WSAStartup 一次性守卫（进程级引用计数由系统管理，配对清理）
struct WsaGuard2 {
    bool ok = false;
    WsaGuard2() {
        WSADATA d{};
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WsaGuard2() { if (ok) WSACleanup(); }
};

// ---------------------------------------------------------------- 内置 DNS 清单

struct DnsServer {
    const wchar_t* name;   // 提供商
    const wchar_t* ip;
};
const DnsServer kDnsList[] = {
    { L"阿里 DNS",   L"223.5.5.5" },
    { L"阿里 DNS",   L"223.6.6.6" },
    { L"DNSPod",     L"119.29.29.29" },
    { L"DNSPod",     L"182.254.116.116" },
    { L"114 DNS",    L"114.114.114.114" },
    { L"114 DNS",    L"114.114.115.115" },
    { L"百度 DNS",   L"180.76.76.76" },
    { L"360 DNS",    L"101.226.4.6" },
    { L"Google",     L"8.8.8.8" },
    { L"Google",     L"8.8.4.4" },
    { L"Cloudflare", L"1.1.1.1" },
    { L"Cloudflare", L"1.0.0.1" },
    { L"Quad9",      L"9.9.9.9" },
    { L"OpenDNS",    L"208.67.222.222" },
};
const int kDnsCount = (int)(sizeof(kDnsList) / sizeof(kDnsList[0]));

// ---------------------------------------------------------------- UDP DNS 探测

// 构造标准查询报文：12 字节头（RD=1，QDCOUNT=1）+ A 查询 www.baidu.com
int BuildDnsQueryA(unsigned char* out, unsigned short txid) {
    int p = 0;
    out[p++] = (unsigned char)(txid >> 8);
    out[p++] = (unsigned char)(txid & 0xFF);
    out[p++] = 0x01; out[p++] = 0x00;          // flags: RD
    out[p++] = 0x00; out[p++] = 0x01;          // QDCOUNT=1
    out[p++] = 0; out[p++] = 0;                // ANCOUNT
    out[p++] = 0; out[p++] = 0;                // NSCOUNT
    out[p++] = 0; out[p++] = 0;                // ARCOUNT
    const char* q = "www.baidu.com";           // 3www5baidu3com0
    const char* labels[] = { "www", "baidu", "com" };
    for (const char* lb : labels) {
        size_t n = strlen(lb);
        out[p++] = (unsigned char)n;
        memcpy(out + p, lb, n);
        p += (int)n;
    }
    out[p++] = 0;                              // 根标签
    out[p++] = 0; out[p++] = 1;                // QTYPE=A
    out[p++] = 0; out[p++] = 1;                // QCLASS=IN
    return p;
}

// 对单个 DNS 测 3 次取最小延迟（ms，QPC 精确计时；一次尝试失败立即重试，
// 全部超时返回 -1）。select 等待上限 1000ms，超时可控且不依赖 dnsapi。
int ProbeDnsTimed(const wstring& ip) {
    WsaGuard2 wsa;
    if (!wsa.ok) return -1;
    char ipA[64] = {};
    WideCharToMultiByte(CP_ACP, 0, ip.c_str(), -1, ipA, 64, nullptr, nullptr);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    if (InetPtonA(AF_INET, ipA, &addr.sin_addr) != 1) { closesocket(s); return -1; }

    int best = -1;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    unsigned short seed = (unsigned short)(GetTickCount() & 0xFFFF);
    for (int attempt = 0; attempt < 3; ++attempt) {
        unsigned char pkt[64];
        unsigned short txid = (unsigned short)(seed + (unsigned short)(attempt * 101));
        int qlen = BuildDnsQueryA(pkt, txid);
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        if (sendto(s, (const char*)pkt, qlen, 0, (sockaddr*)&addr, sizeof(addr)) != qlen)
            continue;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ 1, 0 };
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        unsigned char resp[512];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(s, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &fromLen);
        QueryPerformanceCounter(&t1);
        if (n < 12 || resp[0] != pkt[0] || resp[1] != pkt[1]) continue;
        int ms = (int)((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
        if (best < 0 || ms < best) best = ms;
        if (best == 0) break;   // 0ms 已是下限
    }
    closesocket(s);
    return best;
}

// ---------------------------------------------------------------- 网卡枚举

struct AdapterInfo2 {
    wstring name;
    ULONG ifIndex = 0;
    bool up = false;
    wstring dns;     // 当前 DNS（逗号分隔）
};

vector<AdapterInfo2> EnumAdapters2() {
    vector<AdapterInfo2> out;
    ULONG sz = 16 * 1024;
    for (int tries = 0; tries < 3; ++tries) {
        vector<BYTE> buf(sz);
        ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                       GAA_FLAG_SKIP_MULTICAST |
                                       GAA_FLAG_SKIP_FRIENDLY_NAME,
                                       nullptr,
                                       (PIP_ADAPTER_ADDRESSES)buf.data(), &sz);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (rc != NO_ERROR) return out;
        for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
            if ((ULONG)p->IfType == 24) continue;   // 环回
            if (!p->FriendlyName || !*p->FriendlyName) continue;
            AdapterInfo2 a;
            a.name = p->FriendlyName;
            a.ifIndex = p->IfIndex;
            a.up = (p->OperStatus == IfOperStatusUp);
            bool first = true;
            for (auto* d = p->FirstDnsServerAddress; d; d = d->Next) {
                if (!d->Address.lpSockaddr ||
                    d->Address.lpSockaddr->sa_family != AF_INET) continue;
                char ip[64] = {};
                sockaddr_in* sa = (sockaddr_in*)d->Address.lpSockaddr;
                InetNtopA(AF_INET, &sa->sin_addr, ip, 64);
                if (!first) a.dns += L", ";
                a.dns += wstring(ip, ip + strlen(ip));   // IP 纯 ASCII，逐字符转宽
                first = false;
            }
            out.push_back(std::move(a));
        }
        break;
    }
    return out;
}

// ---------------------------------------------------------------- PowerShell 应用

// 经 procrun 流式执行器跑 PowerShell（不走 netsh——部分环境被策略限制）
bool ApplyDnsViaPowerShell(const wstring& ifIndex, const wstring& dnsCsv,
                           ModuleContext& ctx, wstring* err) {
    wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
                  L"Set-DnsClientServerAddress -InterfaceIndex " + ifIndex +
                  L" -ServerAddresses " + dnsCsv;
    ProcRunOptions opt;
    opt.timeoutMs = 60000;
    opt.cancel = ctx.cancel;
    opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  [powershell] " + line); };
    wstring full;
    int code = RunProcess(cmd, opt, &full);
    if (code == 0) return true;
    if (err)
        *err = (code == -1) ? L"PowerShell 启动失败" :
               (code == -2 ? L"执行超时被终止" :
                (code == -3 ? L"已取消" :
                 L"退出码 " + U32s2((unsigned)code) + L" " +
                 (full.empty() ? wstring() : L"· " + full)));
    return false;
}

// ================================================================ net.dnsopt

class NetDnsOpt final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.dnsopt"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"DNS 优化"; }
    const wchar_t* Description() const override {
        return L"内置 9 家公共 DNS 实测延迟（UDP 标准查询 3 次取最小值，1s 超时）；"
               L"「自动优选」一键设最快两个为主/备；「应用选中」把勾选 DNS 写到"
               L"指定网卡（PowerShell）；支持还原为自动获取；同时展示当前各网卡 DNS";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* RunLabel() const override { return L"开始测速"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"网卡序号", L"目标网卡 InterfaceIndex（见列表第 4 列），留空 = 第一个活动网卡", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fIf_ = values[0];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"自动优选",       nullptr, false, 0xE9D9 },  // 0 → btnClean 位
            { L"应用选中",       nullptr, false, 0xE73E },  // 1 → btnUndo 位
            { L"还原为自动获取", nullptr, false, 0 },       // 2 → 更多 ▾
            { L"刷新 DNS 缓存",  nullptr, false, 0 },       // 3 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        switch (actionIndex) {
        case 0: {
            wstring target = PickAdapterNote();
            return L"将对网卡 " + target + L" 应用实测最快的两个公共 DNS 作为主/备。\n\n是否继续？";
        }
        case 1: {
            if (tags.empty()) return L"未选中任何 DNS。请先在列表中勾选两个 IP。";
            wstring ips;
            int n = 0;
            for (int t : tags) {
                if (t < 0 || t >= kDnsCount) continue;
                ips += wstring(kDnsList[t].ip) + L"（" + kDnsList[t].name + L"）\n";
                ++n;
            }
            if (n != 2) return L"请恰好勾选 2 个 DNS（主 + 备）；当前选中 " + U32s2((unsigned)n) + L" 个。";
            return L"将把以下 DNS 应用到网卡 " + PickAdapterNote() + L"：\n" + ips +
                   L"\n是否继续？";
        }
        case 2:
            return L"将把网卡 " + PickAdapterNote() +
                   L" 的 DNS 还原为自动获取（DHCP 下发 / 路由器默认）。\n\n是否继续？";
        case 3:
            return L"将执行 ipconfig /flushdns 刷新本机 DNS 缓存。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"正在测速公共 DNS（每个 3 次取最小延迟）…");

        // ① 逐个探测
        struct ProbeRow { int idx; int ms; };
        vector<ProbeRow> probes;
        for (int i = 0; i < kDnsCount; ++i) {
            if (ctx.Cancelled()) break;
            int ms = ProbeDnsTimed(kDnsList[i].ip);
            probes.push_back({ i, ms });
            ctx.Report((i + 1) * 80 / kDnsCount,
                       wstring(L"测速 ") + kDnsList[i].ip + (ms >= 0 ?
                                  (L" … " + U32s2((unsigned)ms) + L" ms") : L" … 超时"));
        }

        // ② 延迟升序落表（tag = kDnsList 下标，排序安全回溯）
        std::stable_sort(probes.begin(), probes.end(),
                         [](const ProbeRow& a, const ProbeRow& b) {
                             if (a.ms < 0) return false;
                             if (b.ms < 0) return true;
                             return a.ms < b.ms;
                         });
        int okCnt = 0;
        for (const ProbeRow& pr : probes) {
            ModuleRow r;
            r.c1 = kDnsList[pr.idx].name;
            r.c2 = kDnsList[pr.idx].ip;
            if (pr.ms >= 0) {
                r.c3 = U32s2((unsigned)pr.ms) + L" ms";
                r.c4 = L"可用";
                ++okCnt;
                r.sortKey = (unsigned long long)(10000 - pr.ms) << 8;
            } else {
                r.c3 = L"—";
                r.c4 = L"超时/不可达";
                r.danger = true;
                r.sortKey = 0;
            }
            r.tag = pr.idx;
            out.rows.push_back(std::move(r));
        }

        // ③ 当前网卡 DNS 配置
        vector<AdapterInfo2> adapters = EnumAdapters2();
        for (const AdapterInfo2& a : adapters) {
            ModuleRow r;
            r.c1 = a.name;
            r.c2 = a.dns.empty() ? L"(自动获取)" : a.dns;
            r.c3 = L"当前配置";
            r.c4 = L"InterfaceIndex " + U32s2(a.ifIndex) + (a.up ? L" · 已连接" : L" · 未连接");
            r.tag = -1;
            out.rows.push_back(std::move(r));
        }

        wchar_t b[128];
        swprintf_s(b, 128, L"测速完成：%d/%d 个 DNS 可达；当前 %zu 个网卡（勾选 2 个 IP 可「应用选中」）",
                   okCnt, kDnsCount, adapters.size());
        out.summary = b;
        out.hint = L"「自动优选」直接取最快两个；「应用选中」需勾选恰好 2 个 IP；"
                   L"「更多 ▾」可还原自动获取 / 刷新 DNS 缓存。应用 DNS 需管理员权限。";
        out.canClean = true;
        ctx.Report(100, L"DNS 测速完成");
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 自动优选：实测取最快两个
            ctx.Log(L"自动优选：重新测速并取最快两个 DNS…");
            vector<std::pair<int, int>> ms;   // idx, latency
            for (int i = 0; i < kDnsCount; ++i) {
                if (ctx.Cancelled()) break;
                int v = ProbeDnsTimed(kDnsList[i].ip);
                if (v >= 0) ms.push_back({ i, v });
            }
            std::sort(ms.begin(), ms.end(),
                      [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                          return a.second < b.second;
                      });
            if (ms.size() < 2) {
                out.summary = L"可达 DNS 不足 2 个（仅 " + U32s2((unsigned)ms.size()) +
                              L" 个），无法自动优选";
                out.canClean = true;
                return;
            }
            wstring primary = kDnsList[ms[0].first].ip;
            wstring backup  = kDnsList[ms[1].first].ip;
            ctx.Log(L"  最快：" + primary + L"（" + U32s2((unsigned)ms[0].second) +
                    L" ms）/ " + backup + L"（" + U32s2((unsigned)ms[1].second) + L" ms）");
            ApplyToTarget(ctx, primary + L"," + backup, out);
            break;
        }
        case 1: {   // 应用选中（恰好 2 个）
            vector<wstring> ips;
            for (int t : selection_) {
                if (t < 0 || t >= kDnsCount) continue;
                ips.push_back(kDnsList[t].ip);
            }
            if (ips.size() != 2) {
                out.summary = L"请恰好勾选 2 个 DNS（主 + 备）；当前选中 " +
                              U32s2((unsigned)ips.size()) + L" 个";
                out.canClean = true;
                return;
            }
            ApplyToTarget(ctx, ips[0] + L"," + ips[1], out);
            break;
        }
        case 2: {   // 还原为自动获取
            wstring err;
            wstring target = ResolveIfIndex(&err);
            if (target.empty()) { out.summary = err; out.canClean = true; return; }
            wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
                          L"Set-DnsClientServerAddress -InterfaceIndex " + target +
                          L" -ResetServerAddresses";
            ProcRunOptions opt;
            opt.timeoutMs = 60000;
            opt.cancel = ctx.cancel;
            opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  [powershell] " + line); };
            wstring full;
            int code = RunProcess(cmd, opt, &full);
            out.summary = (code == 0)
                ? wstring(L"已还原为自动获取（DHCP 下发）：网卡 ") + target
                : wstring(L"还原失败：") + (code == -1 ? L"PowerShell 启动失败" :
                     L"退出码 " + U32s2((unsigned)code) + L"（非提权会被拒绝，请以管理员重启后重试）");
            out.canClean = true;
            break;
        }
        case 3: {   // 刷新 DNS 缓存
            ProcRunOptions opt;
            opt.timeoutMs = 30000;
            opt.cancel = ctx.cancel;
            opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  " + line); };
            wstring full;
            int code = RunProcess(L"ipconfig.exe /flushdns", opt, &full);
            out.summary = (code == 0) ? wstring(L"已刷新 DNS 缓存")
                                      : wstring(L"刷新失败：退出码 ") + U32s2((unsigned)code);
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<int> selection_;
    wstring fIf_;

    // 目标网卡说明（确认框文案用）
    wstring PickAdapterNote() const {
        vector<AdapterInfo2> adapters = EnumAdapters2();
        wstring idx = ResolveIfIndexStatic(fIf_, adapters);
        for (const AdapterInfo2& a : adapters)
            if (U32s2(a.ifIndex) == idx)
                return L"「" + a.name + L"」(InterfaceIndex " + idx + L")";
        return L"(InterfaceIndex " + idx + L")";
    }

    // 解析目标网卡：字段优先，留空取第一个活动网卡
    wstring ResolveIfIndex(wstring* err) const {
        vector<AdapterInfo2> adapters = EnumAdapters2();
        wstring idx = ResolveIfIndexStatic(fIf_, adapters);
        if (idx.empty() && err)
            *err = L"没有活动网卡：请先联网，或在字段中填写网卡 InterfaceIndex";
        return idx;
    }

    static wstring ResolveIfIndexStatic(const wstring& field,
                                        const vector<AdapterInfo2>& adapters) {
        wstring f = field;
        while (!f.empty() && (f.back() == L' ' || f.back() == L'\t')) f.pop_back();
        if (!f.empty()) {
            bool allDigits = true;
            for (wchar_t c : f) if (c < L'0' || c > L'9') { allDigits = false; break; }
            if (allDigits) return f;
        }
        for (const AdapterInfo2& a : adapters)
            if (a.up) return U32s2(a.ifIndex);
        return wstring();
    }

    void ApplyToTarget(ModuleContext& ctx, const wstring& dnsCsv, ModuleResult& out) {
        wstring err;
        wstring target = ResolveIfIndex(&err);
        if (target.empty()) {
            out.summary = err;
            out.canClean = true;
            return;
        }
        wstring psErr;
        if (ApplyDnsViaPowerShell(target, dnsCsv, ctx, &psErr)) {
            out.summary = L"已应用 DNS " + dnsCsv + L" → 网卡 " + target +
                          L"（ipconfig /flushdns 后立即生效）";
        } else {
            out.summary = L"应用失败：" + psErr +
                          L"（Set-DnsClientServerAddress 需管理员权限，请提权后重试）";
        }
        out.canClean = true;
    }
};

} // namespace

REGISTER_MODULE(NetDnsOpt);

} // namespace mz
