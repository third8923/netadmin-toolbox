// ============================================================================
//  网管工具箱 NetAdmin Toolbox — 网络管理模块集（9 个模块）
//  ---------------------------------------------------------------------------
//    net.adapters  网络适配器（Inventory）
//    net.conn      网络连接  （Inventory）
//    net.portscan  端口扫描  （Inventory，需要目标）
//    net.lanscan   局域网扫描（Inventory，可选目标，缺省自动探测本机 /24）
//    net.diag      网络诊断  （Console，需要目标）
//    net.route     路由表    （Inventory）
//    net.arp       ARP 表    （Inventory）
//    net.flushdns  刷新 DNS 缓存（Action）
//    net.wifi      无线网络  （Inventory）
//
//  所有数据均来自 Windows API 实时查询，失败时优雅降级为 summary 说明。
//  依赖库：ws2_32 iphlpapi icmp wlanapi dnsapi（链接清单已随回报提交）。
// ============================================================================

// iptypes.h 的完整结构要求 Vista+；本文件单独定义，不影响其他编译单元。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "../module.h"
#include "../common.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <wlanapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

// MinGW 的 windns.h 没声明它，但 dnsapi.dll 确实导出，libdnsapi.a 也有导入符号。
extern "C" __declspec(dllimport) BOOL WINAPI DnsFlushResolverCache(void);

namespace mz {

// ---------------------------------------------------------------------------
//  目标输入通道
//  集成方式：GUI/CLI 在 Run 前调用 IModule::SetInput(path, target)（module.h 已提供），
//  本文件三个需要目标的模块会保存 target 并在 Run 中使用。
//  另外两个兜底通道（便于 CLI/自动化测试，优先级低于 SetInput）：
//    * 环境变量 MZ_NET_TARGET
//    * 运行前调用 mz::netmods::SetTarget(L"1.2.3.4")
// ---------------------------------------------------------------------------
namespace netmods {
static wstring g_target;
void SetTarget(const wstring& t) { g_target = t; }
wstring GetTargetOverride() { return g_target; }
} // namespace netmods

namespace {

// ------------------------------------------------------------------ 基础工具
wstring ToW(long long v) {
    wchar_t b[32];
    swprintf_s(b, L"%lld", v);
    return b;
}
wstring ToWU(unsigned long long v) {
    wchar_t b[32];
    swprintf_s(b, L"%llu", v);
    return b;
}

wstring Trim(const wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a &&
           (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n'))
        --b;
    return s.substr(a, b - a);
}

wstring JoinList(const vector<wstring>& v, const wstring& sep) {
    wstring out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

wstring BaseName(const wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == wstring::npos) ? path : path.substr(p + 1);
}

unsigned long long NowMs() { return GetTickCount64(); }

wstring FormatWinError(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
    wstring s;
    if (n && msg) {
        s.assign(msg, n);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
            s.pop_back();
    }
    if (msg) LocalFree(msg);
    if (s.empty()) return L"错误码 " + ToWU(code);
    return s + L"（错误码 " + ToWU(code) + L"）";
}

wstring WsaErrorText(int err) {
    switch (err) {
        case 10061: return L"连接被拒绝";
        case 10060: return L"连接超时";
        case 10065: return L"主机不可达";
        case 10051: return L"网络不可达";
        case 10054: return L"连接被重置";
        case 10048: return L"地址已被占用";
        default: return FormatWinError((DWORD)err);
    }
}

wstring GaiErrorText(int rc) {
    switch (rc) {
        case 11001: return L"找不到该主机（EAI_NONAME）";
        case 11002: return L"DNS 服务器暂时不可用（EAI_AGAIN）";
        case 11003: return L"域名解析失败（EAI_FAIL）";
        case 11004: return L"主机名存在但没有地址记录（EAI_NODATA）";
        case 10047: return L"不支持的地址族（EAI_FAMILY）";
        case 10093: return L"网络尚未初始化";
        default: return L"解析错误码 " + ToW(rc);
    }
}

// ---------------------------------------------------------------- 目标字符串
template <class T, class = void>
struct HasTargetMember : std::false_type {};
template <class T>
struct HasTargetMember<T, std::void_t<decltype(std::declval<const T&>().target)>>
    : std::true_type {};

template <class Ctx>
wstring ContextTarget(const Ctx& ctx) {
    if constexpr (HasTargetMember<Ctx>::value) {
        using TT = std::decay_t<decltype(ctx.target)>;
        if constexpr (std::is_pointer<TT>::value) {
            if (ctx.target && *ctx.target) return Trim(ctx.target);
        } else {
            wstring t = ctx.target;
            if (!t.empty()) return Trim(t);
        }
    }
    wstring over = netmods::GetTargetOverride();
    if (!over.empty()) return Trim(over);
    wchar_t buf[1024] = {0};
    DWORD n = GetEnvironmentVariableW(L"MZ_NET_TARGET", buf, 1024);
    if (n > 0 && n < 1024) return Trim(wstring(buf, n));
    return L"";
}

// SetInput() 注入的 target 优先，其次才是环境变量/本地设置
template <class Ctx>
wstring EffectiveTarget(const wstring& injected, const Ctx& ctx) {
    wstring t = Trim(injected);
    if (!t.empty()) return t;
    return ContextTarget(ctx);
}

// ---------------------------------------------------------------- Winsock
struct WsaGuard {
    bool ok = false;
    WsaGuard() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WsaGuard() {
        if (ok) WSACleanup();
    }
    WsaGuard(const WsaGuard&) = delete;
    WsaGuard& operator=(const WsaGuard&) = delete;
};

// ---------------------------------------------------------------- 地址格式化
wstring Ip4Str(ULONG addrNetOrder) {
    IN_ADDR a{};
    a.S_un.S_addr = addrNetOrder;
    wchar_t b[INET_ADDRSTRLEN] = {0};
    if (!InetNtopW(AF_INET, &a, b, INET_ADDRSTRLEN)) return L"?";
    return b;
}

wstring Ip6Str(const UCHAR* addr16, DWORD scopeId) {
    IN6_ADDR a{};
    memcpy(&a, addr16, 16);
    wchar_t b[INET6_ADDRSTRLEN] = {0};
    if (!InetNtopW(AF_INET6, &a, b, INET6_ADDRSTRLEN)) return L"?";
    wstring s = b;
    if (scopeId) s += L"%" + ToWU(scopeId);
    return s;
}

wstring SockAddrStr(const sockaddr* sa) {
    if (!sa) return L"";
    if (sa->sa_family == AF_INET) {
        const auto* s4 = (const sockaddr_in*)sa;
        wchar_t b[INET_ADDRSTRLEN] = {0};
        if (!InetNtopW(AF_INET, &s4->sin_addr, b, INET_ADDRSTRLEN)) return L"?";
        return b;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* s6 = (const sockaddr_in6*)sa;
        wchar_t b[INET6_ADDRSTRLEN] = {0};
        if (!InetNtopW(AF_INET6, &s6->sin6_addr, b, INET6_ADDRSTRLEN)) return L"?";
        wstring s = b;
        if (s6->sin6_scope_id) s += L"%" + ToWU(s6->sin6_scope_id);
        return s;
    }
    return L"";
}

wstring MacStr(const BYTE* b, DWORD n) {
    if (!b || n == 0) return L"—";
    wstring s;
    for (DWORD i = 0; i < n && i < 8; ++i) {
        wchar_t t[8];
        swprintf_s(t, L"%02X", (unsigned int)b[i]);
        if (i) s += L'-';
        s += t;
    }
    return s.empty() ? wstring(L"—") : s;
}

wstring SpeedStr(ULONG64 bps) {
    if (bps == 0) return L"未知";
    wchar_t b[64];
    if (bps >= 1000000000ULL) swprintf_s(b, L"%.1f Gbps", (double)bps / 1e9);
    else if (bps >= 1000000ULL) swprintf_s(b, L"%.0f Mbps", (double)bps / 1e6);
    else if (bps >= 1000ULL) swprintf_s(b, L"%.0f Kbps", (double)bps / 1e3);
    else swprintf_s(b, L"%llu bps", (unsigned long long)bps);
    return b;
}

wstring FmtRateKbps(ULONG kbps) {
    if (kbps == 0) return L"未知";
    wchar_t b[64];
    if (kbps >= 1000) swprintf_s(b, L"%.1f Mbps", (double)kbps / 1000.0);
    else swprintf_s(b, L"%lu Kbps", (unsigned long)kbps);
    return b;
}

wstring IwStr(ULONG hostOrder) {
    wchar_t b[32];
    swprintf_s(b, L"%lu.%lu.%lu.%lu", (unsigned long)((hostOrder >> 24) & 0xFF),
               (unsigned long)((hostOrder >> 16) & 0xFF),
               (unsigned long)((hostOrder >> 8) & 0xFF), (unsigned long)(hostOrder & 0xFF));
    return b;
}

// 把主机序 IPv4 的最后一个 octet 清零并去掉，得到 "a.b.c"
wstring PrefixOf24(ULONG hostOrder) {
    wstring s = IwStr(hostOrder & 0xFFFFFF00u);
    size_t p = s.find_last_of(L'.');
    return (p == wstring::npos) ? s : s.substr(0, p);
}

wstring OperStatusStr(IF_OPER_STATUS st) {
    switch ((int)st) {
        case 1: return L"已连接";       // IfOperStatusUp
        case 2: return L"已断开";       // IfOperStatusDown
        case 3: return L"正在测试";     // IfOperStatusTesting
        case 5: return L"休眠";         // IfOperStatusDormant
        case 6: return L"不存在";       // IfOperStatusNotPresent
        case 7: return L"下层断开";     // IfOperStatusLowerLayerDown
        default: return L"未知";
    }
}

wstring IfTypeStr(ULONG t) {
    switch (t) {
        case 6: return L"以太网";
        case 23: return L"PPP";
        case 24: return L"环回";
        case 71: return L"无线";
        case 131: return L"隧道";
        case 243: return L"移动宽带";
        case 244: return L"虚拟";
        default: return L"类型 " + ToWU(t);
    }
}

wstring TcpStateStr(DWORD st) {
    switch (st) {
        case 1: return L"已关闭";
        case 2: return L"侦听";
        case 3: return L"SYN已发送";
        case 4: return L"SYN已接收";
        case 5: return L"已建立";
        case 6: return L"FIN等待1";
        case 7: return L"FIN等待2";
        case 8: return L"关闭等待";
        case 9: return L"正在关闭";
        case 10: return L"最后确认";
        case 11: return L"时间等待";
        case 12: return L"删除中";
        default: return L"状态 " + ToWU(st);
    }
}

int PrefixLenFromMask(ULONG maskNetOrder) {
    ULONG m = ntohl(maskNetOrder);
    int n = 0;
    for (int i = 31; i >= 0; --i) {
        if (m & (1u << i)) ++n;
        else break;
    }
    return n;
}

wstring RouteTypeStr(DWORD t) {
    switch (t) {
        case 3: return L"直连";
        case 4: return L"间接";
        case 2: return L"无效";
        default: return L"类型 " + ToWU(t);
    }
}

wstring WlanStateStr(WLAN_INTERFACE_STATE st) {
    switch ((int)st) {
        case 0: return L"未就绪";
        case 1: return L"已连接";
        case 2: return L"临时网络";
        case 3: return L"正在断开";
        case 4: return L"已断开";
        case 5: return L"正在关联";
        case 6: return L"正在发现";
        case 7: return L"正在认证";
        default: return L"状态 " + ToW((int)st);
    }
}

wstring GuidStr(const GUID& g) {
    wchar_t b[80];
    swprintf_s(b, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               (unsigned long)g.Data1, (unsigned int)g.Data2, (unsigned int)g.Data3,
               (unsigned int)g.Data4[0], (unsigned int)g.Data4[1], (unsigned int)g.Data4[2],
               (unsigned int)g.Data4[3], (unsigned int)g.Data4[4], (unsigned int)g.Data4[5],
               (unsigned int)g.Data4[6], (unsigned int)g.Data4[7]);
    return b;
}

wstring SsidStr(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0) return L"(隐藏网络)";
    if (ssid.uSSIDLength > (ULONG)sizeof(ssid.ucSSID)) return L"(SSID 长度异常)";
    const char* raw = (const char*)ssid.ucSSID;
    int n = MultiByteToWideChar(CP_UTF8, 0, raw, (int)ssid.uSSIDLength, nullptr, 0);
    if (n > 0) {
        wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)ssid.uSSIDLength, w.data(), n);
        return w;
    }
    n = MultiByteToWideChar(CP_ACP, 0, raw, (int)ssid.uSSIDLength, nullptr, 0);
    if (n > 0) {
        wstring w(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, raw, (int)ssid.uSSIDLength, w.data(), n);
        return w;
    }
    return L"(无法解码 SSID)";
}

wstring AuthAlgoStr(int v) {
    switch (v) {
        case 1: return L"开放(Open)";
        case 2: return L"共享密钥(WEP)";
        case 3: return L"WPA-Enterprise";
        case 4: return L"WPA-PSK";
        case 5: return L"WPA-None";
        case 6: return L"WPA2-Enterprise";
        case 7: return L"WPA2-PSK";
        case 8: return L"WPA3-Enterprise";
        case 9: return L"WPA3-SAE";
        case 10: return L"OWE";
        default: return L"认证算法 " + ToW(v);
    }
}

wstring CipherAlgoStr(int v) {
    switch (v) {
        case 0: return L"无";
        case 1: return L"WEP40";
        case 2: return L"TKIP";
        case 4: return L"CCMP(AES)";
        case 5: return L"WEP104";
        case 6: return L"BIP";
        case 8: return L"GCMP";
        case 9: return L"GCMP-256";
        case 10: return L"CCMP-256";
        case 0x100: return L"组密钥";
        case 0x101: return L"WEP";
        default: return L"加密算法 " + ToW(v);
    }
}

// ---------------------------------------------------------------- 适配器数据
bool LoadAdapterAddresses(vector<BYTE>& buf, DWORD* err) {
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
#ifdef GAA_FLAG_INCLUDE_GATEWAYS
    flags |= GAA_FLAG_INCLUDE_GATEWAYS;
#endif
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 5; ++attempt) {
        buf.assign(size + 1024, 0);
        DWORD rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                        (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
        if (rc == ERROR_SUCCESS) return true;
        if (rc != ERROR_BUFFER_OVERFLOW) {
            if (err) *err = rc;
            return false;
        }
    }
    if (err) *err = ERROR_BUFFER_OVERFLOW;
    return false;
}

std::map<DWORD, wstring> BuildIfIndexNames() {
    std::map<DWORD, wstring> m;
    vector<BYTE> buf;
    DWORD err = 0;
    if (!LoadAdapterAddresses(buf, &err)) return m;
    for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
        if (p->FriendlyName && *p->FriendlyName) m[(DWORD)p->IfIndex] = p->FriendlyName;
    }
    return m;
}

wstring IfName(const std::map<DWORD, wstring>& m, DWORD idx) {
    auto it = m.find(idx);
    if (it != m.end() && !it->second.empty()) return it->second;
    return L"接口 #" + ToWU(idx);
}

// 读取一次 IPv4 ARP 表：Ping 扫描后同网段主机的 MAC 基本都在里面，
// 比逐个调用 SendARP（无响应时最长阻塞 4 秒）快几百倍。
std::map<ULONG, wstring> BuildArpMap() {
    std::map<ULONG, wstring> m;
    ULONG size = 0;
    DWORD rc = GetIpNetTable(nullptr, &size, FALSE);
    if (rc != ERROR_INSUFFICIENT_BUFFER) return m;
    vector<BYTE> buf(size + 2048);
    ULONG sz = (ULONG)buf.size();
    if (GetIpNetTable((PMIB_IPNETTABLE)buf.data(), &sz, FALSE) != NO_ERROR) return m;
    auto* tab = (PMIB_IPNETTABLE)buf.data();
    for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
        const MIB_IPNETROW& r = tab->table[i];
        if (r.dwType != 3 && r.dwType != 4) continue;  // 动态 / 静态
        if (r.dwPhysAddrLen < 6) continue;
        m[r.dwAddr] = MacStr(r.bPhysAddr, r.dwPhysAddrLen);
    }
    return m;
}

// ------------------------------------------------------------------- ICMP
struct IcmpHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    IcmpHandle() { h = IcmpCreateFile(); }
    ~IcmpHandle() {
        if (h != INVALID_HANDLE_VALUE) IcmpCloseHandle(h);
    }
    IcmpHandle(const IcmpHandle&) = delete;
    IcmpHandle& operator=(const IcmpHandle&) = delete;
    bool Valid() const { return h != INVALID_HANDLE_VALUE; }

    // ipNetOrder：网络字节序 IPv4（inet_addr 风格）
    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs = nullptr, BYTE* ttl = nullptr,
              DWORD* ipStatus = nullptr) const {
        if (!Valid()) return false;
        BYTE req[32];
        memset(req, 'a', sizeof(req));
        BYTE reply[sizeof(ICMP_ECHO_REPLY) + 8 + 32 + 16];
        memset(reply, 0, sizeof(reply));
        DWORD n = IcmpSendEcho(h, (IPAddr)ipNetOrder, req, sizeof(req), nullptr, reply,
                               sizeof(reply), timeoutMs);
        if (n == 0) {
            if (ipStatus) *ipStatus = GetLastError();
            return false;
        }
        auto* r = (PICMP_ECHO_REPLY)reply;
        if (r->Status != IP_SUCCESS) {
            if (ipStatus) *ipStatus = r->Status;
            return false;
        }
        if (rttMs) *rttMs = r->RoundTripTime;
        if (ttl) *ttl = r->Options.Ttl;
        return true;
    }
};

// ------------------------------------------------ 原始套接字 ICMP（回退通道）
// 本机（非管理员）实测 IcmpSendEcho 返回 0 且 GetLastError=5（拒绝访问），
// 但 SOCK_RAW + IPPROTO_ICMP 可用，因此提供等价的手工 ICMP 回显实现。
unsigned short IcmpChecksum(const void* data, int len) {
    const unsigned short* p = (const unsigned short*)data;
    unsigned long sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        unsigned short last = 0;
        *(unsigned char*)&last = *(const unsigned char*)p;
        sum += last;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

#pragma pack(push, 1)
struct IcmpEchoHeader {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short id;
    unsigned short seq;
};
#pragma pack(pop)

struct RawPing {
    SOCKET s = INVALID_SOCKET;
    unsigned short id = 0;
    unsigned short seq = 0;

    RawPing() = default;
    RawPing(const RawPing&) = delete;
    RawPing& operator=(const RawPing&) = delete;
    ~RawPing() {
        if (s != INVALID_SOCKET) closesocket(s);
    }

    bool Open() {
        if (s != INVALID_SOCKET) return true;
        s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (s == INVALID_SOCKET) return false;
        id = (unsigned short)(0x4D00u | (GetCurrentProcessId() & 0xFFu));
        return true;
    }

    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs, BYTE* ttl) {
        if (!Open()) return false;
        unsigned short mySeq = ++seq;
        if (mySeq == 0) mySeq = ++seq;

        unsigned char pkt[8 + 32];
        memset(pkt, 0, sizeof(pkt));
        auto* h = (IcmpEchoHeader*)pkt;
        h->type = 8;  // echo request
        h->code = 0;
        h->id = htons(id);
        h->seq = htons(mySeq);
        memset(pkt + 8, 'a', 32);
        h->checksum = IcmpChecksum(pkt, sizeof(pkt));

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.S_un.S_addr = ipNetOrder;
        const ULONG64 t0 = GetTickCount64();
        if (sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&dst, sizeof(dst)) ==
            SOCKET_ERROR)
            return false;

        for (;;) {
            const long long remain =
                (long long)timeoutMs - (long long)(GetTickCount64() - t0);
            if (remain <= 0) return false;
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(s, &rf);
            timeval tv{};
            tv.tv_sec = (long)(remain / 1000);
            tv.tv_usec = (long)((remain % 1000) * 1000);
            int sr = select(0, &rf, nullptr, nullptr, &tv);
            if (sr <= 0) return false;
            unsigned char buf[2048];
            int n = recvfrom(s, (char*)buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) continue;
            const int ipHdrLen = (int)(buf[0] & 0x0F) * 4;
            if (ipHdrLen < 20 || n < ipHdrLen + 8) continue;
            const auto* rh = (const IcmpEchoHeader*)(buf + ipHdrLen);
            if (rh->type != 0) continue;  // echo reply
            if (ntohs(rh->id) != id || ntohs(rh->seq) != mySeq) continue;
            if (rttMs) *rttMs = (DWORD)(GetTickCount64() - t0);
            if (ttl) *ttl = buf[8];
            return true;
        }
    }
};

// 统一的 Ping 实现：优先 IcmpSendEcho，被拒绝时自动切换原始套接字。
struct PingEngine {
    enum Mode { ModeUnknown, ModeIcmp, ModeRaw };
    IcmpHandle icmp;
    RawPing raw;
    Mode mode = ModeUnknown;

    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs, BYTE* ttl = nullptr) {
        if (mode != ModeRaw && icmp.Valid()) {
            DWORD status = 0;
            if (icmp.Ping(ipNetOrder, timeoutMs, rttMs, ttl, &status)) {
                mode = ModeIcmp;
                return true;
            }
            if (status == ERROR_ACCESS_DENIED) {
                mode = ModeRaw;  // ICMP 辅助 API 被权限/安全策略拒绝
            } else {
                mode = ModeIcmp;  // 正常的超时 / 不可达
                return false;
            }
        }
        if (mode != ModeRaw) mode = ModeRaw;
        return raw.Ping(ipNetOrder, timeoutMs, rttMs, ttl);
    }

    bool UsingRaw() const { return mode == ModeRaw; }
};

wstring RttStr(DWORD rtt) {
    return (rtt == 0) ? wstring(L"<1 ms") : (ToW(rtt) + L" ms");
}

//------------------------------------------------------------------ 解析/连接
bool ResolveIpv4(const wstring& host, ULONG* addrNetOrder, wstring* err) {
    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* res = nullptr;
    int rc = GetAddrInfoW(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        if (err) *err = GaiErrorText(rc);
        return false;
    }
    bool ok = false;
    for (auto* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET && p->ai_addr) {
            const auto* s4 = (const sockaddr_in*)p->ai_addr;
            *addrNetOrder = s4->sin_addr.S_un.S_addr;
            ok = true;
            break;
        }
    }
    FreeAddrInfoW(res);
    if (!ok && err) *err = L"没有解析到 IPv4 地址";
    return ok;
}

bool TcpProbe(ULONG ipNetOrder, int port, DWORD timeoutMs, DWORD* elapsedMs, DWORD* wsaErr) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (wsaErr) *wsaErr = (DWORD)WSAGetLastError();
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.S_un.S_addr = ipNetOrder;

    ULONG64 t0 = NowMs();
    bool open = false;
    DWORD err = 0;
    int rc = connect(s, (sockaddr*)&sa, sizeof(sa));
    if (rc == 0) {
        open = true;
    } else {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{};
            tv.tv_sec = (long)(timeoutMs / 1000);
            tv.tv_usec = (long)((timeoutMs % 1000) * 1000);
            int sr = select(0, nullptr, &wf, nullptr, &tv);
            if (sr > 0) {
                int soErr = 0;
                int len = sizeof(soErr);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len) == 0 && soErr == 0)
                    open = true;
                else
                    err = (DWORD)(soErr ? soErr : WSAGetLastError());
            } else if (sr == 0) {
                err = WSAETIMEDOUT;
            } else {
                err = (DWORD)WSAGetLastError();
            }
        } else {
            err = (DWORD)e;
        }
    }
    if (elapsedMs) *elapsedMs = (DWORD)(NowMs() - t0);
    if (wsaErr) *wsaErr = err;
    closesocket(s);
    return open;
}

// -------------------------------------------------------------- 局域网扫描
struct SweepState {
    vector<wstring> ips;
    vector<char> alive;   // 0/1，每个线程只写自己领到的下标
    vector<DWORD> rtt;
    std::atomic<int> next{0};
    std::atomic<int> aliveCount{0};
    std::atomic<bool> stop{false};
    std::atomic<int> icmpFail{0};
    std::atomic<int> rawMode{0};
};

DWORD WINAPI SweepWorker(LPVOID param) {
    auto* st = (SweepState*)param;
    PingEngine ping;
    const int total = (int)st->ips.size();
    for (;;) {
        if (st->stop.load(std::memory_order_relaxed)) break;
        int i = st->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= total) break;
        IN_ADDR ia{};
        if (InetPtonW(AF_INET, st->ips[i].c_str(), &ia) != 1) continue;
        DWORD rtt = 0;
        if (ping.Ping(ia.S_un.S_addr, 500, &rtt)) {
            st->rtt[i] = rtt;
            st->alive[i] = 1;
            st->aliveCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (ping.UsingRaw()) st->rawMode.store(1);
    return 0;
}

bool AutoSweepPrefix(wstring* prefix, wstring* localIp, wstring* err) {
    vector<BYTE> buf;
    DWORD e = 0;
    if (!LoadAdapterAddresses(buf, &e)) {
        if (err) *err = L"枚举网卡失败：" + FormatWinError(e);
        return false;
    }
    for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
        if ((ULONG)p->IfType == 24) continue;           // 环回
        if (p->OperStatus != IfOperStatusUp) continue;  // 只看已连接
        for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const auto* s4 = (const sockaddr_in*)sa;
            ULONG host = ntohl(s4->sin_addr.S_un.S_addr);
            if ((host >> 24) == 127) continue;
            if ((host & 0xFFFF0000u) == 0xA9FE0000u) continue;  // 169.254/16
            if (host == 0) continue;
            *prefix = PrefixOf24(host);
            if (localIp) *localIp = IwStr(host);
            return true;
        }
    }
    if (err) *err = L"未找到已连接的 IPv4 网卡";
    return false;
}

bool ParseSweepPrefix(const wstring& target, wstring* prefix, wstring* err) {
    wstring t = Trim(target);
    size_t scheme = t.find(L"://");
    if (scheme != wstring::npos) t = t.substr(scheme + 3);
    size_t slash = t.find(L'/');
    if (slash != wstring::npos) t = t.substr(0, slash);
    if (t.empty()) {
        if (err) *err = L"目标为空";
        return false;
    }
    vector<wstring> parts;
    wstring cur;
    for (wchar_t c : t) {
        if (c == L'.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    if (parts.size() != 3 && parts.size() != 4) {
        if (err) *err = L"目标格式不支持，请填网段前缀（如 192.168.1）";
        return false;
    }
    unsigned nums[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < parts.size(); ++i) {
        wstring q = Trim(parts[i]);
        if (q.empty() || q.size() > 3) {
            if (err) *err = L"目标格式不支持";
            return false;
        }
        for (wchar_t c : q)
            if (c < L'0' || c > L'9') {
                if (err) *err = L"目标不是合法的 IPv4 网段";
                return false;
            }
        unsigned v = (unsigned)_wtoi(q.c_str());
        if (v > 255) {
            if (err) *err = L"目标 IPv4 段超出 0-255";
            return false;
        }
        nums[i] = v;
    }
    wchar_t b[64];
    swprintf_s(b, L"%u.%u.%u", nums[0], nums[1], nums[2]);
    *prefix = b;
    return true;
}

wstring FindLocalIpv4InPrefix(const wstring& prefix) {
    vector<BYTE> buf;
    DWORD e = 0;
    if (!LoadAdapterAddresses(buf, &e)) return L"";
    for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
        for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const auto* s4 = (const sockaddr_in*)sa;
            ULONG host = ntohl(s4->sin_addr.S_un.S_addr);
            if (PrefixOf24(host) == prefix) return IwStr(host);
        }
    }
    return L"";
}

wstring ReverseName(const wstring& ip) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    if (InetPtonW(AF_INET, ip.c_str(), &sa.sin_addr) != 1) return L"—";
    wchar_t host[NI_MAXHOST] = {0};
    int rc = GetNameInfoW((const SOCKADDR*)&sa, sizeof(sa), host, NI_MAXHOST, nullptr, 0,
                          NI_NAMEREQD);
    if (rc == 0 && host[0]) return host;
    return L"—";
}

// ---------------------------------------------------------------- 端口扫描
struct PortSvc {
    int port;
    const wchar_t* name;
};

const PortSvc kCommonPorts[] = {
    {21, L"FTP"},          {22, L"SSH"},           {23, L"Telnet"},
    {25, L"SMTP"},         {53, L"DNS"},           {80, L"HTTP"},
    {110, L"POP3"},        {135, L"RPC"},          {139, L"NetBIOS"},
    {143, L"IMAP"},        {443, L"HTTPS"},        {445, L"SMB"},
    {587, L"SMTP提交"},    {993, L"IMAPS"},        {995, L"POP3S"},
    {1433, L"SQL Server"}, {1521, L"Oracle"},      {3306, L"MySQL"},
    {3389, L"远程桌面"},   {5432, L"PostgreSQL"},  {5900, L"VNC"},
    {6379, L"Redis"},      {8000, L"HTTP(8000)"},  {8080, L"HTTP代理"},
    {8443, L"HTTPS(8443)"},{27017, L"MongoDB"},
};
const int kCommonPortCount = (int)(sizeof(kCommonPorts) / sizeof(kCommonPorts[0]));

wstring PortServiceName(int port) {
    for (const auto& e : kCommonPorts)
        if (e.port == port) return e.name;
    return L"未知服务";
}

void ParseScanTarget(const wstring& raw, wstring* host, vector<int>* ports, bool* autoLocal) {
    wstring t = Trim(raw);
    size_t scheme = t.find(L"://");
    if (scheme != wstring::npos) t = t.substr(scheme + 3);
    size_t slash = t.find_first_of(L"/?#");
    if (slash != wstring::npos) t = t.substr(0, slash);

    int singlePort = 0;
    size_t colon = t.rfind(L':');
    if (colon != wstring::npos) {
        wstring portPart = Trim(t.substr(colon + 1));
        bool digits = !portPart.empty();
        for (wchar_t c : portPart)
            if (c < L'0' || c > L'9') {
                digits = false;
                break;
            }
        if (digits) {
            int v = _wtoi(portPart.c_str());
            if (v > 0 && v <= 65535) {
                singlePort = v;
                t = Trim(t.substr(0, colon));
            }
        }
    }
    *autoLocal = false;
    if (t.empty()) {
        t = L"127.0.0.1";
        *autoLocal = true;
    } else {
        bool onlyDigits = true;
        for (wchar_t c : t)
            if (c < L'0' || c > L'9') {
                onlyDigits = false;
                break;
            }
        if (onlyDigits && singlePort == 0) {
            int v = _wtoi(t.c_str());
            if (v > 0 && v <= 65535) {
                singlePort = v;
                t = L"127.0.0.1";
            }
        }
    }
    *host = t;
    ports->clear();
    if (singlePort) {
        ports->push_back(singlePort);
    } else {
        for (const auto& e : kCommonPorts) ports->push_back(e.port);
    }
}

// ---------------------------------------------------------------- 进程名
wstring ProcNameByPid(DWORD pid, std::map<DWORD, wstring>& cache) {
    auto it = cache.find(pid);
    if (it != cache.end()) return it->second;
    wstring name;
    if (pid == 0) {
        name = L"System Idle";
    } else if (pid == 4) {
        name = L"System";
    } else {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t full[1024] = {0};
            DWORD n = 1024;
            if (QueryFullProcessImageNameW(h, 0, full, &n) && n > 0)
                name = BaseName(wstring(full, n));
            CloseHandle(h);
        }
    }
    cache[pid] = name;
    return name;
}

// ---------------------------------------------------------------- 行构造
ModuleRow MakeRow(wstring c1, wstring c2, wstring c3, wstring c4,
                  unsigned long long sortKey = 0, bool danger = false) {
    ModuleRow r;
    r.c1 = std::move(c1);
    r.c2 = std::move(c2);
    r.c3 = std::move(c3);
    r.c4 = std::move(c4);
    r.sortKey = sortKey;
    r.danger = danger;
    return r;
}

} // namespace

// ===========================================================================
//  1. net.adapters 网络适配器
// ===========================================================================
class NetAdapters final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.adapters"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"网络适配器"; }
    const wchar_t* Description() const override {
        return L"列出所有网卡的 IPv4/IPv6、网关、DNS、MAC、连接状态与链路速率";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        WsaGuard wsa;
        ctx.Report(-1, L"正在枚举网络适配器…");

        vector<BYTE> buf;
        DWORD err = 0;
        if (!LoadAdapterAddresses(buf, &err)) {
            out.summary = L"获取网络适配器失败：" + FormatWinError(err);
            out.hint = T(L"可尝试以管理员身份运行后重试");
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        int total = 0, up = 0, done = 0;
        for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
            if ((ULONG)p->IfType == 24) continue;   // 跳过环回
            if (!p->FriendlyName || !*p->FriendlyName) continue;
            ++total;
        }
        if (total == 0) {
            out.summary = L"未发现可用的网络适配器";
            out.hint = T(L"可能是驱动异常，或运行在无网络的环境中");
            ctx.Report(100, out.summary);
            return;
        }

        ctx.Report(5, L"正在读取适配器信息…");
        for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
            if ((ULONG)p->IfType == 24) continue;
            if (!p->FriendlyName || !*p->FriendlyName) continue;
            const bool isUp = (p->OperStatus == IfOperStatusUp);
            if (isUp) ++up;

            vector<wstring> v4s, v6s, gws, dns;
            for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
                const sockaddr* sa = u->Address.lpSockaddr;
                if (!sa) continue;
                if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6) {
                    wstring s = SockAddrStr(sa);
                    if (!s.empty()) (sa->sa_family == AF_INET ? v4s : v6s).push_back(s);
                }
            }
            for (auto* g = p->FirstGatewayAddress; g; g = g->Next) {
                if (!g->Address.lpSockaddr) continue;
                wstring s = SockAddrStr(g->Address.lpSockaddr);
                if (!s.empty()) gws.push_back(s);
            }
            for (auto* d = p->FirstDnsServerAddress; d; d = d->Next) {
                if (!d->Address.lpSockaddr) continue;
                wstring s = SockAddrStr(d->Address.lpSockaddr);
                if (!s.empty()) dns.push_back(s);
            }

            wstring c2 = v4s.empty() ? wstring(L"(无 IPv4)") : JoinList(v4s, L", ");
            wstring c3 = OperStatusStr(p->OperStatus);

            ULONG64 speed = p->TransmitLinkSpeed ? p->TransmitLinkSpeed : p->ReceiveLinkSpeed;
            wstring det = L"网关 " + (gws.empty() ? wstring(L"无") : JoinList(gws, L","));
            det += L" · MAC " + MacStr(p->PhysicalAddress, p->PhysicalAddressLength);
            det += L" · DNS " + (dns.empty() ? wstring(L"无") : JoinList(dns, L","));
            if (speed) det += L" · 链路 " + SpeedStr(speed);
            det += L" · " + IfTypeStr((ULONG)p->IfType);
            if (!v6s.empty()) det += L" · IPv6 " + ToWU(v6s.size()) + L" 个";

            unsigned long long sortKey =
                ((isUp ? 1ULL : 0ULL) << 32) | ((speed / 1000000ULL) & 0xFFFFFFFFULL);
            out.rows.push_back(MakeRow(p->FriendlyName, c2, c3, det, sortKey));

            ++done;
            ctx.Report(5 + 90 * done / total, L"正在读取 " + wstring(p->FriendlyName));
        }

        out.summary = L"共 " + ToW(total) + L" 个适配器，已连接 " + ToW(up) + L" 个";
        out.hint = T(L"已跳过环回接口；虚拟网卡/隧道接口也会列出");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  2. net.conn 网络连接
// ===========================================================================
class NetConnections final : public IModule {
    static DWORD FetchTcp(int family, TCP_TABLE_CLASS cls, vector<BYTE>& buf) {
        DWORD size = 0;
        DWORD rc = GetExtendedTcpTable(nullptr, &size, FALSE, family, cls, 0);
        if (rc == ERROR_NO_DATA) return ERROR_NO_DATA;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
        for (int i = 0; i < 4; ++i) {
            buf.assign(size + 2048, 0);
            DWORD sz = (DWORD)buf.size();
            rc = GetExtendedTcpTable(buf.data(), &sz, FALSE, family, cls, 0);
            if (rc == ERROR_SUCCESS) return ERROR_SUCCESS;
            if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
            size = sz;
        }
        return ERROR_INSUFFICIENT_BUFFER;
    }
    static DWORD FetchUdp(int family, vector<BYTE>& buf) {
        DWORD size = 0;
        DWORD rc = GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
        if (rc == ERROR_NO_DATA) return ERROR_NO_DATA;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
        for (int i = 0; i < 4; ++i) {
            buf.assign(size + 2048, 0);
            DWORD sz = (DWORD)buf.size();
            rc = GetExtendedUdpTable(buf.data(), &sz, FALSE, family, UDP_TABLE_OWNER_PID, 0);
            if (rc == ERROR_SUCCESS) return ERROR_SUCCESS;
            if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
            size = sz;
        }
        return ERROR_INSUFFICIENT_BUFFER;
    }

public:
    const wchar_t* Id() const override { return L"net.conn"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"网络连接"; }
    const wchar_t* Description() const override {
        return L"TCP/UDP 连接与监听端口，含所属进程、本地/远端地址和状态";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        WsaGuard wsa;
        ctx.Report(-1, L"正在读取 TCP/UDP 连接表…");

        std::map<DWORD, wstring> procCache;
        std::set<DWORD> pidSet;
        int tcpCount = 0, udpCount = 0, listenCount = 0, noPerm = 0;
        bool cancelled = false;

        auto addRow = [&](DWORD pid, const wstring& local, const wstring& state,
                          const wstring& remote, unsigned long long prio) {
            wstring pn = ProcNameByPid(pid, procCache);
            if (pn.empty()) {
                ++noPerm;
                pn = L"PID " + ToWU(pid);
            }
            pidSet.insert(pid);
            unsigned long long sk = prio * 1000000ULL + (pid & 0xFFFFFULL);
            out.rows.push_back(MakeRow(pn, local, state, remote, sk));
        };

        // ---- TCP IPv4：侦听表 + 连接表 ----
        // 注意：本机实测 TCP_TABLE_OWNER_PID_ALL 的侦听行 dwState 不可靠（返回 0 或垃圾值），
        // 因此分别读取 LISTENER / CONNECTIONS 两张表，语义明确、状态可信。
        {
            vector<BYTE> buf;
            DWORD rc = FetchTcp(AF_INET, TCP_TABLE_OWNER_PID_LISTENER, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_TCPTABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_TCPROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    addRow(r.dwOwningPid, Ip4Str(r.dwLocalAddr) + L":" + ToW(lp), L"侦听", L"—", 3);
                    ++tcpCount;
                    ++listenCount;
                }
            } else if (rc != ERROR_NO_DATA) {
                ctx.Log(L"读取 IPv4 TCP 侦听表失败：" + FormatWinError(rc));
            }
        }
        if (!cancelled) {
            vector<BYTE> buf;
            DWORD rc = FetchTcp(AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_TCPTABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_TCPROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    int rp = ntohs((u_short)r.dwRemotePort);
                    wstring remote = (r.dwRemoteAddr == 0 && rp == 0)
                                         ? wstring(L"—")
                                         : Ip4Str(r.dwRemoteAddr) + L":" + ToW(rp);
                    const bool listen = (r.dwState == 2);
                    if (listen) ++listenCount;
                    addRow(r.dwOwningPid, Ip4Str(r.dwLocalAddr) + L":" + ToW(lp),
                           listen ? wstring(L"侦听") : TcpStateStr(r.dwState), remote,
                           listen ? 3 : (r.dwState == 5 ? 2 : 1));
                    ++tcpCount;
                }
            } else if (rc != ERROR_NO_DATA) {
                ctx.Log(L"读取 IPv4 TCP 连接表失败：" + FormatWinError(rc));
            }
        }
        // ---- TCP IPv6：侦听表 + 连接表 ----
        if (!cancelled) {
            vector<BYTE> buf;
            DWORD rc = FetchTcp(AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_TCP6TABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_TCP6ROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    addRow(r.dwOwningPid, Ip6Str(r.ucLocalAddr, r.dwLocalScopeId) + L":" + ToW(lp),
                           L"侦听", L"—", 3);
                    ++tcpCount;
                    ++listenCount;
                }
            }
        }
        if (!cancelled) {
            vector<BYTE> buf;
            DWORD rc = FetchTcp(AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_TCP6TABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_TCP6ROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    int rp = ntohs((u_short)r.dwRemotePort);
                    wstring remote = (rp == 0) ? wstring(L"—")
                                               : Ip6Str(r.ucRemoteAddr, r.dwRemoteScopeId) + L":" +
                                                     ToW(rp);
                    const bool listen = (r.dwState == 2);
                    if (listen) ++listenCount;
                    addRow(r.dwOwningPid, Ip6Str(r.ucLocalAddr, r.dwLocalScopeId) + L":" + ToW(lp),
                           listen ? wstring(L"侦听") : TcpStateStr(r.dwState), remote,
                           listen ? 3 : (r.dwState == 5 ? 2 : 1));
                    ++tcpCount;
                }
            }
        }
        // ---- UDP IPv4 ----
        if (!cancelled) {
            vector<BYTE> buf;
            DWORD rc = FetchUdp(AF_INET, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_UDPTABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_UDPROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    wstring local = Ip4Str(r.dwLocalAddr) + L":" + ToW(lp);
                    addRow(r.dwOwningPid, local, L"UDP", L"—", 0);
                    ++udpCount;
                }
            }
        }
        // ---- UDP IPv6 ----
        if (!cancelled) {
            vector<BYTE> buf;
            DWORD rc = FetchUdp(AF_INET6, buf);
            if (rc == ERROR_SUCCESS) {
                auto* tab = (PMIB_UDP6TABLE_OWNER_PID)buf.data();
                for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
                    if ((i & 63) == 0 && ctx.Cancelled()) {
                        cancelled = true;
                        break;
                    }
                    const MIB_UDP6ROW_OWNER_PID& r = tab->table[i];
                    int lp = ntohs((u_short)r.dwLocalPort);
                    wstring local = Ip6Str(r.ucLocalAddr, r.dwLocalScopeId) + L":" + ToW(lp);
                    addRow(r.dwOwningPid, local, L"UDP", L"—", 0);
                    ++udpCount;
                }
            }
        }

        out.summary = L"TCP " + ToW(tcpCount) + L" 条（侦听 " + ToW(listenCount) + L"）/ UDP " +
                      ToW(udpCount) + L" 条，涉及 " + ToWU(pidSet.size()) + L" 个进程";
        if (cancelled) out.summary += L"（已取消，结果不完整）";
        if (noPerm > 0)
            out.hint = T(L"有 ") + ToW(noPerm) + T(L" 个进程名因权限不足无法读取（可尝试管理员运行）");
        else
            out.hint = T(L"侦听在 0.0.0.0 上的端口对所有网卡开放，请留意暴露到公网的服务");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  3. net.portscan 端口扫描
// ===========================================================================
class NetPortScan final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.portscan"; }
    const wchar_t* Group() const override { return L"网络诊断"; }
    const wchar_t* Title() const override { return L"端口扫描"; }
    const wchar_t* Description() const override {
        return L"对目标 IP/域名扫描 26 个常见 TCP 端口（300ms 超时，多路并发）";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    bool NeedsTarget() const override { return true; }
    const wchar_t* TargetHint() const override { return L"目标 IP 或域名，如 192.168.1.1"; }
    void SetInput(const wstring& /*path*/, const wstring& target) override {
        target_ = Trim(target);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        WsaGuard wsa;
        if (!wsa.ok) {
            out.summary = L"Winsock 初始化失败，无法进行端口扫描";
            out.hint = T(L"请检查系统网络组件（ws2_32.dll）是否正常");
            ctx.Report(100, out.summary);
            return;
        }

        wstring raw = EffectiveTarget(target_, ctx);
        wstring host;
        vector<int> ports;
        bool autoLocal = false;
        ParseScanTarget(raw, &host, &ports, &autoLocal);

        ctx.Report(2, L"正在解析目标 " + host + L"…");
        ULONG ip = 0;
        wstring err;
        if (!ResolveIpv4(host, &ip, &err)) {
            out.summary = L"无法解析目标 " + host + L"：" + err;
            out.hint = T(L"请填写 IP 或域名，例如 127.0.0.1、192.168.1.1、example.com");
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        const wstring ipStr = Ip4Str(ip);
        ctx.Log(L"端口扫描目标 " + host + L" → " + ipStr + L"，共 " + ToW(ports.size()) + L" 个端口");

        std::set<int> openPorts;
        bool cancelled = false;
        bool scanFail = false;
        const int kBatch = 24;
        size_t donePorts = 0;
        ULONG64 t0 = NowMs();

        for (size_t base = 0; base < ports.size(); base += kBatch) {
            if (ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            const size_t n = std::min<size_t>(kBatch, ports.size() - base);
            SOCKET socks[24];
            int pend[24];
            fd_set wf;
            FD_ZERO(&wf);
            bool waiting = false;
            for (size_t k = 0; k < n; ++k) {
                socks[k] = INVALID_SOCKET;
                pend[k] = 0;
            }
            for (size_t k = 0; k < n; ++k) {
                const int port = ports[base + k];
                SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (s == INVALID_SOCKET) {
                    scanFail = true;
                    continue;
                }
                socks[k] = s;
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
                sockaddr_in sa{};
                sa.sin_family = AF_INET;
                sa.sin_port = htons((u_short)port);
                sa.sin_addr.S_un.S_addr = ip;
                int rc = connect(s, (sockaddr*)&sa, sizeof(sa));
                if (rc == 0) {
                    pend[k] = 1;  // 立即连接成功
                } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    pend[k] = 2;
                    FD_SET(s, &wf);
                    waiting = true;
                } else {
                    closesocket(s);
                    socks[k] = INVALID_SOCKET;
                }
            }
            if (waiting) {
                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 300 * 1000;
                int sr = select(0, nullptr, &wf, nullptr, &tv);
                if (sr > 0) {
                    for (size_t k = 0; k < n; ++k) {
                        if (pend[k] != 2 || socks[k] == INVALID_SOCKET) continue;
                        if (!FD_ISSET(socks[k], &wf)) continue;
                        int soErr = 0;
                        int len = sizeof(soErr);
                        if (getsockopt(socks[k], SOL_SOCKET, SO_ERROR, (char*)&soErr, &len) == 0 &&
                            soErr == 0)
                            pend[k] = 1;
                    }
                }
            }
            for (size_t k = 0; k < n; ++k) {
                if (pend[k] == 1) openPorts.insert(ports[base + k]);
                if (socks[k] != INVALID_SOCKET) closesocket(socks[k]);
            }
            donePorts += n;
            ctx.Report(5 + (int)(90 * donePorts / std::max<size_t>(1, ports.size())),
                       L"已扫描 " + ToWU(donePorts) + L"/" + ToWU(ports.size()) + L" 个端口");
        }

        const DWORD elapsed = (DWORD)(NowMs() - t0);
        for (int port : openPorts) {
            out.rows.push_back(
                MakeRow(ipStr, ToW((long long)port), L"开放", PortServiceName(port), 100000 - port));
        }

        wstring autoNote = autoLocal ? L"（未填写目标，默认扫描本机）" : L"";
        out.summary = L"目标 " + ipStr + autoNote + L"：扫描 " + ToWU(ports.size()) +
                      L" 个端口，开放 " + ToWU(openPorts.size()) + L" 个，用时 " +
                      ToWU(elapsed) + L" ms";
        if (cancelled) out.summary += L"（已取消，结果不完整）";
        if (scanFail)
            out.hint = T(L"部分套接字创建失败，结果可能不完整（检查安全软件/防火墙）");
        else if (openPorts.empty())
            out.hint = T(L"未发现开放端口：目标可能离线、禁用了这些端口或被防火墙拦截");
        else
            out.hint = T(L"仅扫描常见端口，单项超时 300ms；判断开放依据 TCP 三次握手");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    wstring target_;
};

// ===========================================================================
//  4. net.lanscan 局域网扫描
// ===========================================================================
class NetLanScan final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.lanscan"; }
    const wchar_t* Group() const override { return L"网络诊断"; }
    const wchar_t* Title() const override { return L"局域网扫描"; }
    const wchar_t* Description() const override {
        return L"Ping 扫描 /24 网段，列出存活主机、响应时间、主机名与 MAC";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    bool NeedsTarget() const override { return true; }
    void SetInput(const wstring& /*path*/, const wstring& target) override {
        target_ = Trim(target);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        WsaGuard wsa;
        wstring target = EffectiveTarget(target_, ctx);
        wstring prefix, localIp, err;
        if (target.empty()) {
            if (!AutoSweepPrefix(&prefix, &localIp, &err)) {
                out.summary = L"局域网扫描：无法确定网段";
                out.hint = err + T(L"；也可以在目标框填写 192.168.1 这样的网段");
                ctx.Report(100, out.summary);
                return;
            }
        } else {
            if (!ParseSweepPrefix(target, &prefix, &err)) {
                out.summary = L"目标格式不正确：" + err;
                out.hint = T(L"目标可填 192.168.1 / 192.168.1.0 / 192.168.1.0/24 / 192.168.1.55");
                ctx.Report(100, out.summary);
                return;
            }
            localIp = FindLocalIpv4InPrefix(prefix);
        }
        ctx.Log(L"局域网扫描网段 " + prefix + L".0/24（本机 " +
                (localIp.empty() ? wstring(L"未知") : localIp) + L"）");

        SweepState st;
        st.ips.reserve(254);
        st.alive.assign(254, 0);
        st.rtt.assign(254, 0);
        for (int i = 1; i <= 254; ++i) st.ips.push_back(prefix + L"." + ToW(i));
        const int total = (int)st.ips.size();
        const ULONG64 t0 = NowMs();
        ctx.Report(1, L"正在 Ping 扫描 " + prefix + L".0/24 的 254 个地址…");

        int threadCount = std::min<int>(48, total);
        vector<HANDLE> threads;
        threads.reserve(threadCount);
        for (int i = 0; i < threadCount; ++i) {
            HANDLE h = CreateThread(nullptr, 0, SweepWorker, &st, 0, nullptr);
            if (!h) break;
            threads.push_back(h);
        }
        bool cancelled = false;
        if (threads.empty()) {
            SweepWorker(&st);  // 极端情况：串行兜底
        } else {
            for (;;) {
                DWORD wr = WaitForMultipleObjects((DWORD)threads.size(), threads.data(), TRUE, 120);
                if (wr == WAIT_OBJECT_0) break;
                if (wr != WAIT_TIMEOUT) break;
                int done = st.next.load(std::memory_order_relaxed);
                if (done > total) done = total;
                int alive = st.aliveCount.load(std::memory_order_relaxed);
                ctx.Report(2 + 73 * done / std::max(1, total),
                           L"Ping 扫描 " + ToW(done) + L"/" + ToW(total) + L"，存活 " +
                               ToW(alive));
                if (ctx.Cancelled()) {
                    cancelled = true;
                    st.stop.store(true);
                }
            }
            WaitForMultipleObjects((DWORD)threads.size(), threads.data(), TRUE, 3000);
            for (HANDLE h : threads) CloseHandle(h);
        }

        int scanned = std::min(st.next.load(std::memory_order_relaxed), total);
        const ULONG64 tPing = NowMs();
        ctx.Log(L"Ping 扫描完成：" + ToWU(tPing - t0) + L" ms，存活 " +
                ToWU(st.aliveCount.load()) + L" 台");
        vector<int> aliveIdx;
        for (int i = 0; i < total; ++i)
            if (st.alive[i]) aliveIdx.push_back(i);

        // 反向 DNS 可能被网络环境拖慢（单次可达 1~2 秒），因此给 2.5 秒预算；
        // 超时后剩余主机名显示为 —，并提示已跳过。
        const int kNameLimit = 32;
        const ULONG64 dnsDeadline = NowMs() + 2500;
        auto arpMap = BuildArpMap();
        int named = 0, skippedNames = 0, missingMac = 0;
        for (size_t k = 0; k < aliveIdx.size(); ++k) {
            if (ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            const int i = aliveIdx[k];
            const wstring& ip = st.ips[i];
            IN_ADDR ia{};
            InetPtonW(AF_INET, ip.c_str(), &ia);
            // MAC 直接取 ARP 缓存（刚被 Ping 扫描填充）。不再逐台 SendARP：
            // 无响应时单次会阻塞约 4 秒，会让整个扫描不可接受地变慢。
            wstring macStr = L"—";
            auto ait = arpMap.find((ULONG)ia.S_un.S_addr);
            if (ait != arpMap.end()) macStr = ait->second;
            if (macStr == L"—") ++missingMac;
            wstring name = L"—";
            if (named < kNameLimit && NowMs() < dnsDeadline) {
                name = ReverseName(ip);
                ++named;
            } else {
                ++skippedNames;
            }
            if (name == ip) name = L"—";  // 有些路由器把 PTR 记录设成 IP 本身
            wstring rtt = RttStr(st.rtt[i]);
            wstring c3 = name;
            if (!localIp.empty() && ip == localIp) c3 += L"（本机）";
            out.rows.push_back(MakeRow(ip, rtt, c3, macStr, 1000000 - st.rtt[i]));
            ctx.Report(77 + (int)(22 * (k + 1) / aliveIdx.size()),
                       L"正在解析 " + ip + L" 的主机名/MAC");
        }
        ctx.Log(L"主机名/MAC 解析完成：" + ToWU(NowMs() - tPing) + L" ms");

        const DWORD elapsed = (DWORD)(NowMs() - t0);
        out.summary = L"网段 " + prefix + L".0/24：扫描 " + ToW(scanned) + L" 个地址，存活 " +
                      ToWU(aliveIdx.size()) + L" 台，用时 " + ToWU(elapsed) + L" ms";
        if (cancelled) out.summary += L"（已取消，结果不完整）";
        const wstring rawNote =
            st.rawMode.load() ? L"；ICMP 辅助 API 被系统拒绝，已自动改用原始套接字 Ping" : L"";
        const wstring dnsNote =
            skippedNames > 0
                ? (L"；为控制耗时，跳过了 " + ToW(skippedNames) + L" 台主机的反向 DNS 解析")
                : L"";
        const wstring macNote =
            missingMac > 0
                ? (L"；" + ToW(missingMac) + L" 台主机 MAC 显示 —（ARP 缓存暂无记录，跨网段时正常）")
                : L"";
        if (aliveIdx.empty())
            out.hint = T(L"未发现存活主机：可能网段错误、防火墙禁 ping，或设备不在线") + rawNote;
        else
            out.hint = T(L"基于 ICMP 回显判断存活；MAC 取自 ARP 缓存") + dnsNote + macNote + rawNote;
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    wstring target_;
};

// ===========================================================================
//  5. net.diag 网络诊断
// ===========================================================================
class NetDiag final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.diag"; }
    const wchar_t* Group() const override { return L"网络诊断"; }
    const wchar_t* Title() const override { return L"网络诊断"; }
    const wchar_t* Description() const override {
        return L"一键诊断：Ping 4 次 + DNS 解析 + TCP 80/443 连通性";
    }
    ModuleKind Kind() const override { return ModuleKind::Console; }
    bool NeedsTarget() const override { return true; }
    const wchar_t* TargetHint() const override { return L"目标 IP 或域名，如 8.8.8.8"; }
    void SetInput(const wstring& /*path*/, const wstring& target) override {
        target_ = Trim(target);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        WsaGuard wsa;
        wstring target = EffectiveTarget(target_, ctx);
        if (target.empty()) {
            out.summary = L"请输入要诊断的目标（IP 或域名）";
            out.hint = T(L"例如 127.0.0.1、www.baidu.com、192.168.1.1");
            ctx.Report(100, out.summary);
            return;
        }
        ctx.Report(2, L"正在解析 " + target + L"…");
        ULONG64 t0 = NowMs();
        ULONG ip = 0;
        wstring err;
        if (!ResolveIpv4(target, &ip, &err)) {
            out.rows.push_back(MakeRow(L"目标解析", L"失败", L"—", target + L"：" + err, 100));
            out.summary = L"网络诊断失败：无法解析 " + target;
            out.hint = T(L"请检查域名拼写、DNS 设置或网络连接");
            ctx.Report(100, out.summary);
            return;
        }
        const wstring ipStr = Ip4Str(ip);
        const DWORD resolveMs = (DWORD)(NowMs() - t0);
        out.rows.push_back(
            MakeRow(L"目标解析", ipStr, ToWU(resolveMs) + L" ms", L"输入：" + target, 100));

        // ---------------- Ping ----------------
        ctx.Report(10, L"Ping " + ipStr + L"（最多 4 次）…");
        PingEngine ping;
        int sent = 0, recv = 0;
        DWORD minRtt = 0xFFFFFFFF, maxRtt = 0;
        unsigned long long sumRtt = 0;
        bool cancelled = false;
        for (int i = 1; i <= 4; ++i) {
            if (ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            ++sent;
            DWORD rtt = 0;
            BYTE ttl = 0;
            if (ping.Ping(ip, 1000, &rtt, &ttl)) {
                ++recv;
                sumRtt += rtt;
                if (rtt < minRtt) minRtt = rtt;
                if (rtt > maxRtt) maxRtt = rtt;
                out.rows.push_back(MakeRow(L"Ping 第 " + ToW(i) + L" 包", L"成功", RttStr(rtt),
                                           L"TTL=" + ToWU(ttl), 60 - i * 2));
            } else {
                out.rows.push_back(MakeRow(L"Ping 第 " + ToW(i) + L" 包", L"超时", L"—",
                                           L"1000 ms 内无响应", 60 - i * 2));
            }
            ctx.Report(10 + i * 10, L"Ping 第 " + ToW(i) + L" 包完成");
        }
        if (recv == 0 && !ping.icmp.Valid() && ping.raw.s == INVALID_SOCKET) {
            out.rows.push_back(MakeRow(L"Ping", L"不可用", L"—",
                                       L"IcmpSendEcho 与原始套接字均不可用", 60));
        } else {
            int lost = sent - recv;
            wstring stat2 = L"发送 " + ToW(sent) + L" / 接收 " + ToW(recv) + L" / 丢失 " +
                            ToW(lost) + L"（" + ToW(sent ? lost * 100 / sent : 0) + L"%）";
            wstring stat3 = recv ? (L"最小 " + RttStr(minRtt) + L" / 平均 " +
                                    RttStr((DWORD)(sumRtt / (unsigned)recv)) + L" / 最大 " +
                                    RttStr(maxRtt))
                                 : wstring(L"—");
            out.rows.push_back(MakeRow(
                L"Ping 统计", stat2, stat3,
                recv ? (ping.UsingRaw() ? L"ICMP 回显可达（原始套接字）" : L"ICMP 回显可达")
                     : L"目标不可达或被防火墙拦截",
                55));
        }

        // ---------------- DNS ----------------
        ctx.Report(55, L"DNS 解析 " + target + L"…");
        ULONG64 td = NowMs();
        vector<wstring> v4, v6;
        ADDRINFOW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        ADDRINFOW* res = nullptr;
        int grc = GetAddrInfoW(target.c_str(), nullptr, &hints, &res);
        if (grc == 0) {
            for (auto* p = res; p; p = p->ai_next) {
                if (!p->ai_addr) continue;
                wstring s = SockAddrStr(p->ai_addr);
                if (s.empty()) continue;
                auto& bucket = (p->ai_family == AF_INET6) ? v6 : v4;
                if (std::find(bucket.begin(), bucket.end(), s) == bucket.end())
                    bucket.push_back(s);
            }
            FreeAddrInfoW(res);
        }
        const DWORD dnsMs = (DWORD)(NowMs() - td);
        wstring dns2 = (grc == 0) ? (L"A 记录 " + ToWU(v4.size()) + L" 条 / AAAA 记录 " +
                                     ToWU(v6.size()) + L" 条")
                                  : wstring(L"失败");
        out.rows.push_back(MakeRow(L"DNS 解析", dns2, ToWU(dnsMs) + L" ms",
                                   grc == 0 ? (L"GetAddrInfoW 共解析到 " + ToWU(v4.size() + v6.size()) +
                                               L" 个地址")
                                            : GaiErrorText(grc),
                                   50));
        int ridx = 0;
        for (auto& s : v4)
            out.rows.push_back(MakeRow(L"DNS A 记录", s, L"IPv4",
                                       L"第 " + ToW(++ridx) + L" 条", 40));
        for (auto& s : v6)
            out.rows.push_back(MakeRow(L"DNS AAAA 记录", s, L"IPv6",
                                       L"第 " + ToW(++ridx) + L" 条", 39));

        // ---------------- TCP 80/443 ----------------
        ctx.Report(80, L"TCP 连通性测试（80/443）…");
        int openCount = 0;
        const int testPorts[2] = {80, 443};
        for (int port : testPorts) {
            if (ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            DWORD ms = 0;
            DWORD werr = 0;
            bool ok = TcpProbe(ip, port, 1500, &ms, &werr);
            if (ok) ++openCount;
            wstring svc = (port == 80) ? L"HTTP" : L"HTTPS";
            out.rows.push_back(MakeRow(
                L"TCP " + ToW(port) + L"（" + svc + L"）", ok ? L"可连接" : L"不可连接",
                ToWU(ms) + L" ms",
                ok ? (ipStr + L":" + ToW(port) + L" 连接成功")
                   : (ipStr + L":" + ToW(port) + L"：" + WsaErrorText((int)werr)),
                ok ? 45 : 10 - port / 100));
        }

        out.summary = L"目标 " + ipStr + (target != ipStr ? (L"（" + target + L"）") : L"") +
                      L"：DNS " + ToWU(v4.size() + v6.size()) + L" 条，Ping " + ToW(recv) + L"/" +
                      ToW(sent) + L"，TCP 开放 " + ToW(openCount) + L"/2";
        if (cancelled) out.summary += L"（已取消，结果不完整）";
        const wstring pingNote =
            ping.UsingRaw() ? L"；ICMP 辅助 API 被系统拒绝，Ping 使用原始套接字" : L"";
        if (recv == 0)
            out.hint = T(L"Ping 无响应：目标可能禁 ping、离线或被防火墙拦截，请以 TCP 结果为准") +
                       pingNote;
        else if (openCount == 0)
            out.hint = T(L"Ping 可达但 80/443 均不可连接：可能只开放了其它端口或被防火墙过滤") +
                       pingNote;
        else
            out.hint = T(L"诊断完成：目标网络层与应用层均可访问") + pingNote;
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    wstring target_;
};

// ===========================================================================
//  6. net.route 路由表
// ===========================================================================
class NetRoute final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.route"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"路由表"; }
    const wchar_t* Description() const override {
        return L"IPv4 路由表：目标网络/掩码、网关、接口、metric 与路由类型";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(-1, L"正在读取 IPv4 路由表…");
        ULONG size = 0;
        DWORD rc = GetIpForwardTable(nullptr, &size, FALSE);
        if (rc == ERROR_NO_DATA) {
            out.summary = L"IPv4 路由表为空";
            out.hint = T(L"系统可能没有配置任何 IPv4 路由");
            ctx.Report(100, out.summary);
            return;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            out.summary = L"读取路由表失败：" + FormatWinError(rc);
            out.hint = T(L"可尝试以管理员身份运行");
            ctx.Report(100, out.summary);
            return;
        }
        vector<BYTE> buf(size + 2048);
        ULONG sz = (ULONG)buf.size();
        rc = GetIpForwardTable((PMIB_IPFORWARDTABLE)buf.data(), &sz, FALSE);
        if (rc != NO_ERROR) {
            out.summary = L"读取路由表失败：" + FormatWinError(rc);
            ctx.Report(100, out.summary);
            return;
        }
        auto* tab = (PMIB_IPFORWARDTABLE)buf.data();
        auto ifMap = BuildIfIndexNames();
        int defaultRoutes = 0, direct = 0;
        bool cancelled = false;
        for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
            if ((i & 63) == 0 && ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            const MIB_IPFORWARDROW& r = tab->table[i];
            const int plen = PrefixLenFromMask(r.dwForwardMask);
            wstring dest = Ip4Str(r.dwForwardDest) + L"/" + ToW(plen);
            bool isDirect = (r.dwForwardNextHop == 0);
            if (isDirect) ++direct;
            if (r.dwForwardDest == 0 && r.dwForwardMask == 0) ++defaultRoutes;
            wstring gw = isDirect ? wstring(L"直接连接") : Ip4Str(r.dwForwardNextHop);
            out.rows.push_back(MakeRow(dest, gw, IfName(ifMap, r.dwForwardIfIndex),
                                       L"metric " + ToWU(r.dwForwardMetric1) + L" · " +
                                           RouteTypeStr(r.dwForwardType),
                                       1000000ULL - r.dwForwardMetric1));
        }
        out.summary = L"IPv4 路由 " + ToWU(tab->dwNumEntries) + L" 条（默认路由 " +
                      ToW(defaultRoutes) + L" 条，直连 " + ToW(direct) + L" 条）";
        if (cancelled) out.summary += L"（已取消，结果不完整）";
        out.hint = T(L"仅列出 IPv4 路由；IPv6 路由可在命令行用 route print -6 查看");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  7. net.arp ARP 表
// ===========================================================================
class NetArp final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.arp"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"ARP 表"; }
    const wchar_t* Description() const override {
        return L"IPv4 邻居缓存：IP、MAC 地址、动态/静态类型与所属接口";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(-1, L"正在读取 ARP 表…");
        ULONG size = 0;
        DWORD rc = GetIpNetTable(nullptr, &size, FALSE);
        if (rc == ERROR_NO_DATA) {
            out.summary = L"ARP 表为空";
            out.hint = T(L"可能尚未与任何局域网设备通信");
            ctx.Report(100, out.summary);
            return;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            out.summary = L"读取 ARP 表失败：" + FormatWinError(rc);
            out.hint = T(L"可尝试以管理员身份运行");
            ctx.Report(100, out.summary);
            return;
        }
        vector<BYTE> buf(size + 2048);
        ULONG sz = (ULONG)buf.size();
        rc = GetIpNetTable((PMIB_IPNETTABLE)buf.data(), &sz, FALSE);
        if (rc != NO_ERROR) {
            out.summary = L"读取 ARP 表失败：" + FormatWinError(rc);
            ctx.Report(100, out.summary);
            return;
        }
        auto* tab = (PMIB_IPNETTABLE)buf.data();
        auto ifMap = BuildIfIndexNames();
        int dyn = 0, sta = 0;
        for (DWORD i = 0; i < tab->dwNumEntries; ++i) {
            if ((i & 63) == 0 && ctx.Cancelled()) {
                out.summary = L"ARP 表读取已取消（结果不完整）";
                ctx.Report(100, out.summary);
                return;
            }
            const MIB_IPNETROW& r = tab->table[i];
            wstring type;
            switch (r.dwType) {
                case 3: type = L"动态"; ++dyn; break;
                case 4: type = L"静态"; ++sta; break;
                case 2: type = L"无效"; break;
                default: type = L"其他"; break;
            }
            out.rows.push_back(
                MakeRow(Ip4Str(r.dwAddr), MacStr(r.bPhysAddr, r.dwPhysAddrLen), type,
                        L"接口 " + IfName(ifMap, r.dwIndex) + L"（#" + ToWU(r.dwIndex) + L"）"));
        }
        out.summary = L"ARP 记录 " + ToWU(tab->dwNumEntries) + L" 条（动态 " + ToW(dyn) +
                      L" / 静态 " + ToW(sta) + L"）";
        out.hint = T(L"ARP 表为 IPv4 邻居缓存，会随通信自动增删；需管理员权限可完整读取");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  8. net.flushdns 刷新 DNS 缓存
// ===========================================================================
class NetFlushDns final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.flushdns"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"刷新 DNS 缓存"; }
    const wchar_t* Description() const override {
        return L"清空本机 DNS 解析缓存（dnsapi!DnsFlushResolverCache，失败时回退 ipconfig）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(-1, L"正在刷新 DNS 解析缓存…");
        ULONG64 t0 = NowMs();
        BOOL ok = DnsFlushResolverCache();
        DWORD lastError = GetLastError();
        wstring detail;
        if (ok) {
            detail = L"已调用 dnsapi!DnsFlushResolverCache";
        } else {
            wstring outp;
            int rc = RunHidden(L"ipconfig /flushdns", &outp);
            if (rc == 0) {
                ok = TRUE;
                detail = L"DnsFlushResolverCache 失败，已回退 ipconfig /flushdns 并成功";
            } else {
                detail = L"DnsFlushResolverCache 失败：" + FormatWinError(lastError) +
                         L"；ipconfig /flushdns 退出码 " + ToW(rc);
            }
        }
        const DWORD ms = (DWORD)(NowMs() - t0);
        out.rows.push_back(MakeRow(L"刷新 DNS 解析缓存", ok ? L"成功" : L"失败",
                                   ToWU(ms) + L" ms", detail, ok ? 100 : 0));
        out.summary = ok ? (L"DNS 解析缓存已刷新，用时 " + ToWU(ms) + L" ms")
                         : L"刷新 DNS 缓存失败";
        out.hint = ok ? T(L"若域名仍解析异常，可再执行 ipconfig /release 再 /renew（需管理员）")
                      : T(L"请尝试以管理员身份运行本工具");
        ctx.Log(out.summary + L"：" + detail);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  9. net.wifi 无线网络
// ===========================================================================
class NetWifi final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.wifi"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"无线网络"; }
    const wchar_t* Description() const override {
        return L"无线接口当前 SSID、信号质量、认证/加密方式、信道与协商速率";
    }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(-1, L"正在打开 WLAN 服务…");
        HANDLE h = nullptr;
        DWORD negotiated = 0;
        // WLAN_API_VERSION_2_0 在部分 MinGW 头里没有宏定义，文档值即 2（Win7+）
        DWORD rc = WlanOpenHandle(2, nullptr, &negotiated, &h);
        if (rc != ERROR_SUCCESS) {
            out.summary = L"无法打开无线服务：" + FormatWinError(rc);
            out.hint = T(L"请确认「WLAN AutoConfig (WlanSvc)」服务已启动，且本机有无线网卡");
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        rc = WlanEnumInterfaces(h, nullptr, &list);
        if (rc != ERROR_SUCCESS || !list) {
            WlanCloseHandle(h, nullptr);
            out.summary = L"枚举无线接口失败：" + FormatWinError(rc);
            out.hint = T(L"无线服务可能异常，可尝试重启 WLAN AutoConfig 服务");
            ctx.Report(100, out.summary);
            return;
        }

        const int n = (int)list->dwNumberOfItems;
        if (n == 0) {
            WlanFreeMemory(list);
            WlanCloseHandle(h, nullptr);
            out.summary = L"未检测到无线网卡";
            out.hint = T(L"本机可能没有 Wi-Fi 硬件，或无线网卡驱动未安装");
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        int connected = 0, done = 0;
        for (int i = 0; i < n; ++i) {
            if (ctx.Cancelled()) break;
            const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
            const wstring desc =
                info.strInterfaceDescription[0] ? wstring(info.strInterfaceDescription)
                                                : (L"无线接口 #" + ToW(i));
            const bool isConn = (info.isState == wlan_interface_state_connected);
            if (isConn) ++connected;

            wstring ssid = L"(未连接)";
            wstring c3 = WlanStateStr(info.isState);
            wstring c4 = L"GUID " + GuidStr(info.InterfaceGuid);
            bool pushed = false;

            if (isConn) {
                DWORD qsize = 0;
                PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
                DWORD qrc = WlanQueryInterface(h, &info.InterfaceGuid,
                                               wlan_intf_opcode_current_connection, nullptr,
                                               &qsize, (PVOID*)&ca, nullptr);
                if (qrc == ERROR_SUCCESS && ca) {
                    const WLAN_ASSOCIATION_ATTRIBUTES& aa = ca->wlanAssociationAttributes;
                    const WLAN_SECURITY_ATTRIBUTES& sa = ca->wlanSecurityAttributes;
                    ssid = SsidStr(aa.dot11Ssid);
                    const int signal = (int)aa.wlanSignalQuality;

                    ULONG channel = 0;
                    bool hasChannel = false;
                    DWORD csize = 0;
                    PULONG pc = nullptr;
                    if (WlanQueryInterface(h, &info.InterfaceGuid,
                                           wlan_intf_opcode_channel_number, nullptr, &csize,
                                           (PVOID*)&pc, nullptr) == ERROR_SUCCESS &&
                        pc) {
                        channel = *pc;
                        hasChannel = true;
                        WlanFreeMemory(pc);
                    }

                    c3 = L"信号 " + ToW(signal) + L"% · 信道 " +
                         (hasChannel ? ToWU(channel) : wstring(L"未知"));
                    c4 = AuthAlgoStr((int)sa.dot11AuthAlgorithm) + L" · 加密 " +
                         CipherAlgoStr((int)sa.dot11CipherAlgorithm) + L" · BSSID " +
                         MacStr(aa.dot11Bssid, 6) + L" · 收/发 " +
                         FmtRateKbps(aa.ulRxRate) + L" / " + FmtRateKbps(aa.ulTxRate);
                    if (ca->strProfileName[0]) c4 += L" · 配置文件 " + wstring(ca->strProfileName);
                    c4 += L" · " + desc;
                    WlanFreeMemory(ca);
                    out.rows.push_back(MakeRow(desc, ssid, c3, c4,
                                               1000000ULL + (unsigned long long)signal));
                    pushed = true;
                } else {
                    c4 += L" · 连接属性查询失败：" + FormatWinError(qrc);
                }
            }
            if (!pushed) out.rows.push_back(MakeRow(desc, ssid, c3, c4, isConn ? 500000 : 0));
            ++done;
            ctx.Report(5 + 90 * done / n, L"正在读取 " + desc);
        }

        WlanFreeMemory(list);
        WlanCloseHandle(h, nullptr);

        out.summary = L"检测到 " + ToW(n) + L" 个无线接口，已连接 " + ToW(connected) + L" 个";
        out.hint = connected ? T(L"信号质量为 Windows 综合评分（0-100），高于 70 一般较稳定")
                             : T(L"当前没有已连接的无线网络");
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }
};

// ===========================================================================
//  注册（注册顺序即界面中的显示顺序）
// ===========================================================================
REGISTER_MODULE(NetAdapters);
REGISTER_MODULE(NetConnections);
REGISTER_MODULE(NetPortScan);
REGISTER_MODULE(NetLanScan);
REGISTER_MODULE(NetDiag);
REGISTER_MODULE(NetRoute);
REGISTER_MODULE(NetArp);
REGISTER_MODULE(NetFlushDns);
REGISTER_MODULE(NetWifi);

} // namespace mz
