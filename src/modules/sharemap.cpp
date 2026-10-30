// ============================================================================
//  网管工具箱 - net.sharemap 共享扫描与网盘映射 (v2.9 T04)
//  ---------------------------------------------------------------------------
//  Run() = 局域网共享扫描 + 已映射驱动器盘点：
//    主路径：/24 Ping 扫描（net.lanscan 48 线程池范式）→ 存活主机 445 端口
//    300ms 非阻塞批量预探测（net.portscan select 范式）→ 仅对 445 开放主机
//    NetShareEnum(1)。离线主机被 300ms 预探测拦截，杜绝 NetShareEnum 无超时
//    参数导致"单台离线机拖死全局"。
//    辅助路径：NetServerEnum(SV_TYPE_SERVER) 快速发现并入候选（返回空自动只
//    用 Ping 路径）。
//  Actions()：
//    0 映射选中为网盘   WNetAddConnection2(RESOURCETYPE_DISK, CONNECT_UPDATE_PROFILE)
//    1 断开选中连接     WNetCancelConnection2(本地盘符或远程名, 0, FALSE)
//    2 仅扫描共享（刷新）= 重跑 Run
//    3 打开共享文件夹   ShellExecute(\\host\share)
//    4 断开全部网盘(danger) 枚举 RESOURCE_CONNECTED 后逐个强制断（fForce=TRUE）
//  凭据：路径框 user:pass（首个冒号分隔），仅会话内传参，绝不落盘不缓存。
// ============================================================================
#include "module.h"
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <lm.h>
#include <winnetwk.h>
#include <shellapi.h>
#include <atomic>
#include <map>
#include <set>
#include <algorithm>

namespace mz {
namespace {

// ---------------------------------------------------------------- 小工具

wstring Trim(const wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

wstring I64(long long v) {
    wchar_t b[32];
    swprintf_s(b, L"%lld", v);
    return b;
}

wstring ToWU(unsigned long long v) {
    wchar_t b[32];
    swprintf_s(b, L"%llu", v);
    return b;
}

// Win32/网络错误 → 中文短句（常见值硬编码，其余走 FormatMessage）
wstring ErrZh(DWORD e) {
    switch (e) {
        case 0:     return L"成功";
        case 5:     return L"拒绝访问";
        case 53:    return L"找不到网络路径";
        case 67:    return L"找不到网络名";
        case 85:    return L"本地设备名已占用";
        case 86:    return L"指定的网络密码不正确";
        case 2250:  return L"此网络连接不存在";
        case 2401:  return L"此连接有打开的文件，请先关闭或强制断开";
        case 2404:  return L"设备正被活动进程使用，无法断开";
        case 1219:  return L"不允许到同一服务器的多个用户名连接（先断开已有连接）";
        case 1326:  return L"登录失败：用户名或密码不正确（目标需要凭据）";
        case ERROR_LOGON_TYPE_NOT_GRANTED: return L"账户未被授予该登录类型";
        default: break;
    }
    wchar_t buf[512] = {0};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, buf, 512, nullptr);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' '))
        buf[--n] = 0;
    if (n > 0) return wstring(buf) + L" (0x" + ToWU(e) + L")";
    return L"错误码 " + ToWU(e);
}

// NetServerEnum/NetShareEnum 的 NERR 错误补充说明
// （MinGW lmerr.h 无 NERR_AccessDenied；实测枚举被拒返回 ERROR_ACCESS_DENIED(5)）
wstring NerrZh(NET_API_STATUS ns) {
    switch (ns) {
        case NERR_Success:      return L"成功";
        case 5:                 return L"拒绝访问（目标要求凭据）";
        case NERR_ServerNotStarted: return L"Server 服务未运行";
        case NERR_InvalidComputer:  return L"计算机名无效";
        case NERR_NotPrimary:   return L"目标不是主控";
        default:
            if (ns >= 2100 && ns <= 2999) return L"网络错误码 " + ToWU(ns);
            return ErrZh(ns);
    }
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

wstring IwStr(ULONG hostOrder) {
    wchar_t b[32];
    swprintf_s(b, L"%lu.%lu.%lu.%lu", (unsigned long)((hostOrder >> 24) & 0xFF),
               (unsigned long)((hostOrder >> 16) & 0xFF),
               (unsigned long)((hostOrder >> 8) & 0xFF),
               (unsigned long)(hostOrder & 0xFF));
    return b;
}

// 主机序 IPv4 → "a.b.c"（/24 前缀）
wstring PrefixOf24(ULONG hostOrder) {
    wstring s = IwStr(hostOrder & 0xFFFFFF00u);
    size_t p = s.find_last_of(L'.');
    return (p == wstring::npos) ? s : s.substr(0, p);
}

bool LoadAdapterAddresses(vector<BYTE>& buf, DWORD* err) {
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
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

// 取本机第一个已连接 IPv4 网卡所在 /24 网段前缀与本机 IP
bool AutoSweepPrefix(wstring* prefix, wstring* localIp, wstring* err) {
    vector<BYTE> buf;
    DWORD e = 0;
    if (!LoadAdapterAddresses(buf, &e)) {
        if (err) *err = L"枚举网卡失败：" + ErrZh(e);
        return false;
    }
    for (auto* p = (PIP_ADAPTER_ADDRESSES)buf.data(); p; p = p->Next) {
        if ((ULONG)p->IfType == 24) continue;            // 环回
        if (p->OperStatus != IfOperStatusUp) continue;   // 只看已连接
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

// ------------------------------------------------------------------- ICMP
// 与 netmods.cpp PingEngine 同构：IcmpSendEcho 优先，被拒(5)时切换原始套接字。

struct IcmpHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    IcmpHandle() { h = IcmpCreateFile(); }
    ~IcmpHandle() {
        if (h != INVALID_HANDLE_VALUE) IcmpCloseHandle(h);
    }
    IcmpHandle(const IcmpHandle&) = delete;
    IcmpHandle& operator=(const IcmpHandle&) = delete;
    bool Valid() const { return h != INVALID_HANDLE_VALUE; }

    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs = nullptr) const {
        if (!Valid()) return false;
        BYTE req[32];
        memset(req, 'a', sizeof(req));
        BYTE reply[sizeof(ICMP_ECHO_REPLY) + 8 + 32 + 16];
        memset(reply, 0, sizeof(reply));
        DWORD n = IcmpSendEcho(h, (IPAddr)ipNetOrder, req, sizeof(req), nullptr, reply,
                               sizeof(reply), timeoutMs);
        if (n == 0) return false;
        auto* r = (PICMP_ECHO_REPLY)reply;
        if (r->Status != IP_SUCCESS) return false;
        if (rttMs) *rttMs = r->RoundTripTime;
        return true;
    }
};

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

    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs) {
        if (!Open()) return false;
        unsigned short mySeq = ++seq;
        if (mySeq == 0) mySeq = ++seq;

        unsigned char pkt[8 + 32];
        memset(pkt, 0, sizeof(pkt));
        auto* h = (IcmpEchoHeader*)pkt;
        h->type = 8;
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
            if (rh->type != 0) continue;
            if (ntohs(rh->id) != id || ntohs(rh->seq) != mySeq) continue;
            if (rttMs) *rttMs = (DWORD)(GetTickCount64() - t0);
            return true;
        }
    }
};

struct PingEngine {
    enum Mode { ModeUnknown, ModeIcmp, ModeRaw };
    IcmpHandle icmp;
    RawPing raw;
    Mode mode = ModeUnknown;

    bool Ping(ULONG ipNetOrder, DWORD timeoutMs, DWORD* rttMs) {
        if (mode != ModeRaw && icmp.Valid()) {
            if (icmp.Ping(ipNetOrder, timeoutMs, rttMs)) {
                mode = ModeIcmp;
                return true;
            }
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                mode = ModeRaw;
            } else {
                mode = ModeIcmp;
                return false;
            }
        }
        if (mode != ModeRaw) mode = ModeRaw;
        return raw.Ping(ipNetOrder, timeoutMs, rttMs);
    }
};

// ------------------------------------------------------- Ping 扫描线程池
// 与 netmods.cpp net.lanscan 的 SweepState/SweepWorker 同构（48 线程 + stop 标志）
struct SweepState {
    vector<wstring> ips;
    vector<char> alive;   // 每个线程只写自己领到的下标
    std::atomic<int> next{0};
    std::atomic<int> aliveCount{0};
    std::atomic<bool> stop{false};
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
            st->alive[i] = 1;
            st->aliveCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return 0;
}

// -------------------------------------------------------------- 共享模型

// NetServerEnum 快速发现的后台状态（可超时放弃：浏览器超时实测可达 20s，
// 不能让它拖死主路径；放弃后线程自行结束，只遗留一个小结构体不回收）
struct NseState {
    std::atomic<int> done{0};
    std::atomic<long> ns{0};        // NET_API_STATUS
    std::atomic<long> count{0};
    vector<wstring> names;          // 仅 done=1 后读
};

DWORD WINAPI NseWorker(LPVOID param) {
    auto* s = (NseState*)param;
    LPBYTE pb = nullptr;
    DWORD rd = 0, tt = 0, rh = 0;
    NET_API_STATUS ns = NetServerEnum(nullptr, 101, &pb, MAX_PREFERRED_LENGTH,
                                      &rd, &tt, SV_TYPE_SERVER, nullptr, &rh);
    if (ns == NERR_Success && pb && rd > 0) {
        auto* arr = (SERVER_INFO_101*)pb;
        for (DWORD i = 0; i < rd; ++i) {
            wstring nm = arr[i].sv101_name ? arr[i].sv101_name : L"";
            if (!nm.empty()) s->names.push_back(nm);
        }
        NetApiBufferFree(pb);
    } else if (pb) {
        NetApiBufferFree(pb);
    }
    s->ns.store((long)ns, std::memory_order_relaxed);
    s->count.store((long)s->names.size(), std::memory_order_relaxed);
    s->done.store(1, std::memory_order_release);
    return 0;
}


// 管理共享判定（与 mgmtmods.cpp IsManagementShare 同语义；跨编译单元复制）
bool IsManagementShare(const wstring& name) {
    if (_wcsicmp(name.c_str(), L"IPC$") == 0) return true;
    if (_wcsicmp(name.c_str(), L"ADMIN$") == 0) return true;
    if (name.size() == 2 && name[1] == L'$' && iswalpha(name[0])) return true;
    return false;
}

wstring ShareTypeStr(DWORD t) {
    DWORD base = t & 0x00000003u;   // 低 2 位 = 基础类型（STYPE_MASK）
    wstring s = (base == STYPE_DISKTREE) ? L"磁盘共享"
              : (base == STYPE_PRINTQ)   ? L"打印机共享"
              : (base == STYPE_DEVICE)   ? L"设备共享"
              : (base == STYPE_IPC)      ? L"IPC 通道"
                                         : L"共享";
    if (t & STYPE_SPECIAL) s += L"(特殊)";
    if (t & STYPE_TEMPORARY) s += L"(临时)";
    return s;
}

// 扫描结果缓存的一项（映射动作按 tag 取用）
struct ShareItem {
    wstring remote;      // \\host\share（host 可能是 IP）
    wstring host;        // 显示用主机名（无名字时 = IP）
    wstring share;
};

// 已映射驱动器一项
struct MappedPair {
    wstring local;       // Z: 或空（设备连接）
    wstring remote;      // \\host\share
};

// 扫描行 tag 与已映射行 tag 的分界（已映射区置顶，tag = kMapTagBase + 下标）
constexpr int kMapTagBase = 900000;

} // namespace

// ===========================================================================
class NetShareMapModule final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.sharemap"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"共享扫描与网盘映射"; }
    const wchar_t* Description() const override {
        return L"扫描局域网共享并一键映射为网络盘符；管理已映射驱动器（凭据可填 user:pass）";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsPath() const override { return true; }
    bool NeedsTarget() const override { return true; }
    const wchar_t* PathHint() const override {
        return L"凭据 user:pass（留空=用当前凭据，仅会话内使用不落盘）";
    }
    const wchar_t* TargetHint() const override {
        return L"盘符，如 Z:（留空=自动从 Z 往前分配）";
    }

    void SetInput(const wstring& path, const wstring& target) override {
        cred_ = Trim(path);
        drive_ = Trim(target);
    }
    void SetSelection(const vector<int>& tags) override { sel_ = tags; }

    // ---------------- Run = 扫描 + 已映射盘点 ----------------
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        sel_.clear();
        scanCache_.clear();
        const ULONG64 t0 = GetTickCount64();
        WsaGuard wsa;

        // ---- A) 已映射驱动器盘点（置顶区）----
        vector<MappedPair> mapped;
        EnumMapped(&mapped);
        for (size_t i = 0; i < mapped.size(); ++i) {
            ModuleRow r;
            r.c1 = mapped[i].local.empty() ? L"（无盘符）" : mapped[i].local;
            r.c2 = mapped[i].remote;
            r.c3 = L"已映射";
            r.c4 = L"可勾选后用「断开选中连接」断开";
            r.sortKey = 0xFFFFFFFFFFFFFFFFull - i;   // 恒排最前
            r.tag = kMapTagBase + (int)i;
            out.rows.push_back(r);
        }

        // ---- B) 网段确定 ----
        wstring prefix, localIp, err;
        if (!AutoSweepPrefix(&prefix, &localIp, &err)) {
            out.summary = L"共享扫描：无法确定网段";
            out.hint = err + T(L"；已映射区如上（若有）");
            ctx.Report(100, out.summary);
            return;
        }
        ctx.Log(L"共享扫描网段 " + prefix + L".0/24（本机 " + localIp + L"）");

        // ---- C) 辅助路径：NetServerEnum 快速发现（后台线程 + 1s 收割预算）----
        // 实测无主浏览器环境 NetServerEnum 阻塞可达 20s 才返回 6118，
        // 因此先起线程，与 Ping 扫描并行；扫描完成后最多再等 1s，超时放弃。
        // 名字 → 显示名表（供扫描行展示主机名；解析失败显 IP）
        NseState* nse = new NseState();   // 放弃时故意不回收（线程仍会写它）
        HANDLE nseThread = CreateThread(nullptr, 0, NseWorker, nse, 0, nullptr);
        std::map<wstring, wstring> nameByIp;
        vector<wstring> enumHosts;
        auto harvestNse = [&]() {
            if (!nseThread) return;
            if (WaitForSingleObject(nseThread, 1000) != WAIT_OBJECT_0) {
                ctx.Log(L"NetServerEnum 超时预算（1s）用尽，跳过快速发现，只用 Ping 路径");
                CloseHandle(nseThread);
                nseThread = nullptr;
                return;
            }
            CloseHandle(nseThread);
            nseThread = nullptr;
            if (nse->done.load(std::memory_order_acquire) && nse->count.load() > 0) {
                enumHosts = nse->names;
                ctx.Log(L"NetServerEnum 快速发现 " + I64((long long)enumHosts.size()) +
                        L" 台候选主机（并入 445 预探测）");
            } else {
                ctx.Log(L"NetServerEnum 无结果（" + NerrZh((NET_API_STATUS)nse->ns.load()) +
                        L"），自动只用 Ping 扫描路径");
            }
        };

        // ---- D) 主路径：/24 Ping 扫描（48 线程池范式）----
        SweepState st;
        st.ips.reserve(254);
        st.alive.assign(254, 0);
        for (int i = 1; i <= 254; ++i) st.ips.push_back(prefix + L"." + I64(i));
        const int total = (int)st.ips.size();
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
            SweepWorker(&st);   // 极端情况串行兜底
        } else {
            for (;;) {
                DWORD wr = WaitForMultipleObjects((DWORD)threads.size(), threads.data(),
                                                  TRUE, 120);
                if (wr == WAIT_OBJECT_0) break;
                if (wr != WAIT_TIMEOUT) break;
                int done = st.next.load(std::memory_order_relaxed);
                if (done > total) done = total;
                int alive = st.aliveCount.load(std::memory_order_relaxed);
                ctx.Report(2 + 38 * done / std::max(1, total),
                           L"已测 " + I64(done) + L"/" + I64(total) + L"，存活 " +
                               I64(alive));
                if (ctx.Cancelled()) {
                    cancelled = true;
                    st.stop.store(true);
                }
            }
            WaitForMultipleObjects((DWORD)threads.size(), threads.data(), TRUE, 3000);
            for (HANDLE h : threads) CloseHandle(h);
        }
        const int aliveN = st.aliveCount.load();
        ctx.Log(L"Ping 扫描完成：存活 " + I64(aliveN) + L" 台");
        harvestNse();   // Ping 期间 NetServerEnum 已并行跑，最多再等 1s

        // 候选 IP = 存活 IP ∪ NetServerEnum 主机名解析出的 IP
        vector<wstring> candidates;
        std::set<wstring> seen;
        for (int i = 0; i < total; ++i) {
            if (!st.alive[i]) continue;
            if (seen.insert(st.ips[i]).second) candidates.push_back(st.ips[i]);
        }
        for (const wstring& nm : enumHosts) {
            wstring ip = ResolveIpv4Str(nm);
            if (!ip.empty() && seen.insert(ip).second) {
                candidates.push_back(ip);
                nameByIp[ip] = nm;
            }
        }
        ctx.Log(L"去重后 445 预探测候选 " + I64((long long)candidates.size()) + L" 台");

        // ---- E) 445 端口批量预探测（24 路非阻塞 select，300ms）----
        ctx.Report(42, L"正在预探测 445 端口（SMB）…");
        vector<wstring> smbHosts;
        {
            const size_t kBatch = 24;
            for (size_t base = 0; base < candidates.size() && !cancelled;
                 base += kBatch) {
                if (ctx.Cancelled()) {
                    cancelled = true;
                    break;
                }
                const size_t n = std::min<size_t>(kBatch, candidates.size() - base);
                SOCKET socks[24];
                int pend[24];
                fd_set wf;
                FD_ZERO(&wf);
                bool waiting = false;
                for (size_t k = 0; k < n; ++k) {
                    socks[k] = INVALID_SOCKET;
                    pend[k] = 0;
                    IN_ADDR ia{};
                    if (InetPtonW(AF_INET, candidates[base + k].c_str(), &ia) != 1)
                        continue;
                    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s == INVALID_SOCKET) continue;
                    socks[k] = s;
                    u_long nb = 1;
                    ioctlsocket(s, FIONBIO, &nb);
                    sockaddr_in sa{};
                    sa.sin_family = AF_INET;
                    sa.sin_port = htons(445);
                    sa.sin_addr.S_un.S_addr = ia.S_un.S_addr;
                    int rc = connect(s, (sockaddr*)&sa, sizeof(sa));
                    if (rc == 0) {
                        pend[k] = 1;
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
                            if (getsockopt(socks[k], SOL_SOCKET, SO_ERROR,
                                           (char*)&soErr, &len) == 0 && soErr == 0)
                                pend[k] = 1;
                        }
                    }
                }
                for (size_t k = 0; k < n; ++k) {
                    if (pend[k] == 1) smbHosts.push_back(candidates[base + k]);
                    if (socks[k] != INVALID_SOCKET) closesocket(socks[k]);
                }
                ctx.Report(42 + (int)(18 * std::min(base + n, candidates.size()) /
                                       std::max<size_t>(1, candidates.size())),
                           L"445 预探测 " + I64((long long)std::min(base + n, candidates.size())) +
                               L"/" + I64((long long)candidates.size()));
            }
        }
        ctx.Log(L"445 开放主机 " + I64((long long)smbHosts.size()) + L" 台");

        // ---- F) 逐台 NetShareEnum(level 1) ----
        int hosts = 0, shares = 0, credDenied = 0, filtered = 0;
        for (size_t hi = 0; hi < smbHosts.size() && !cancelled; ++hi) {
            if (ctx.Cancelled()) {
                cancelled = true;
                break;
            }
            const wstring& ip = smbHosts[hi];
            wstring display;
            auto nit = nameByIp.find(ip);
            if (nit != nameByIp.end()) display = nit->second;
            if (display.empty() && ip == localIp) display = L"本机";
            if (display.empty()) display = ip;

            wstring server = L"\\\\" + ip;
            LPBYTE pb = nullptr;
            DWORD rd = 0, tt = 0, rh = 0;
            NET_API_STATUS ns = NetShareEnum((LMSTR)server.c_str(), 1, &pb,
                                             MAX_PREFERRED_LENGTH, &rd, &tt, &rh);
            if (ns != NERR_Success || !pb) {
                // 需凭据等错误也保留一行，不中断整体
                ++credDenied;
                ModuleRow r;
                r.c1 = display;
                r.c2 = L"—";
                r.c3 = L"需凭据";
                r.c4 = server + L" 枚举被拒：" + NerrZh(ns) +
                       L"；映射时可在路径框填 user:pass";
                r.sortKey = 5000000;
                r.danger = false;
                out.rows.push_back(r);
                ctx.Log(L"  " + display + L"（" + ip + L"）→ " + NerrZh(ns));
                if (pb) NetApiBufferFree(pb);
                continue;
            }
            ++hosts;
            auto* arr = (SHARE_INFO_1*)pb;
            int hostShares = 0;
            for (DWORD i = 0; i < rd; ++i) {
                wstring nm = arr[i].shi1_netname ? arr[i].shi1_netname : L"";
                if (nm.empty()) continue;
                if (IsManagementShare(nm)) {
                    ++filtered;
                    continue;   // 管理共享默认过滤（IPC$/ADMIN$/X$）
                }
                wstring remote = server + L"\\" + nm;
                ShareItem item;
                item.remote = remote;
                item.host = display;
                item.share = nm;
                int tag = (int)scanCache_.size();
                scanCache_.push_back(item);

                wstring c3 = ShareTypeStr(arr[i].shi1_type);
                if (arr[i].shi1_remark && *arr[i].shi1_remark) c3 += L" · " +
                    wstring(arr[i].shi1_remark);
                ModuleRow r;
                r.c1 = display;
                r.c2 = nm;
                r.c3 = c3;
                r.c4 = remote + L"（勾选后可映射为网盘）";
                r.sortKey = 1000000 + hostShares;
                r.tag = tag;
                out.rows.push_back(r);
                ++hostShares;
                ++shares;
            }
            NetApiBufferFree(pb);
            if (hostShares == 0)
                ctx.Log(L"  " + display + L"（" + ip + L"）→ 0 个用户共享");
        }

        // ---- G) 汇总 ----
        const DWORD elapsed = (DWORD)(GetTickCount64() - t0);
        wstring sum = L"发现 " + I64(hosts + (credDenied ? credDenied : 0)) + L" 台主机 / " +
                      I64(shares) + L" 个共享";
        if (cancelled) sum += L"（已取消，结果不完整）";
        out.summary = sum;
        wstring notes;
        if (filtered > 0)
            notes += L"；已默认过滤管理共享 " + I64(filtered) + L" 个（IPC$/ADMIN$/X$）";
        if (credDenied > 0)
            notes += L"；" + I64(credDenied) + L" 台需凭据（枚举被拒）";
        out.hint = T(L"勾选行后点「映射选中为网盘」；盘符在目标框指定（留空自动分配）") + notes;
        if (!scanCache_.empty())
            ctx.Log(L"扫描缓存 " + I64((long long)scanCache_.size()) +
                    L" 条共享路径供映射动作使用");
        ctx.Log(L"扫描用时 " + I64(elapsed) + L" ms");
        ctx.Report(100, out.summary);
    }

    // ---------------- 动作表 ----------------
    vector<ModuleAction> Actions() const override {
        return {
            { L"映射选中为网盘", nullptr, false, 0xEDA2 },   // HardDrive
            { L"断开选中连接", nullptr, false, 0xE74D },     // Delete
            { L"仅扫描共享（刷新）", nullptr, false, 0xE895 },  // Sync
            { L"打开共享文件夹", nullptr, false, 0xE8B7 },   // Folder
            { L"断开全部网盘", L"将强制断开本机全部已映射网络驱动器（fForce，有打开文件的连接也会断）。\n\n是否继续？",
              true, 0xE74D },
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        if (actionIndex == 0) {
            if (tags.empty()) return L"未勾选任何共享行。\n\n请先勾选要映射的共享（或改用「断开选中连接」操作已映射驱动器）。";
            wstring items;
            int n = 0;
            for (int t : tags) {
                if (t >= 0 && t < (int)scanCache_.size() && n < 12) {
                    items += L"  · " + scanCache_[t].remote + L"\n";
                    ++n;
                }
            }
            if ((int)tags.size() > 12) items += L"  …（共 " + I64((long long)tags.size()) + L" 项）\n";
            wstring driveNote;
            wchar_t c = DriveLetterFromTarget();
            if (c) driveNote = L"首项映射到指定盘符 ";
            else driveNote = L"盘符未指定：自动从 Z 往前分配";
            wstring credNote;
            if (!cred_.empty())
                credNote = L"\n将使用路径框提供的凭据（仅本次会话使用，不落盘）";
            return L"将映射 " + I64((long long)tags.size()) + L" 个共享为网络驱动器（登录时自动重连）：\n" +
                   items + L"\n" + driveNote + credNote + L"\n是否继续？";
        }
        if (actionIndex == 1) {
            if (tags.empty()) return L"未勾选任何行。";
            wstring items;
            int n = 0;
            for (int t : tags) {
                wstring nm = NameForTag(t);
                if (!nm.empty() && n < 12) {
                    items += L"  · " + nm + L"\n";
                    ++n;
                }
            }
            return L"将断开以下网络连接（不强制，有打开文件时会失败）：\n" + items +
                   L"\n是否继续？";
        }
        if (actionIndex == 4) {
            vector<MappedPair> mapped;
            EnumMapped(&mapped);
            if (mapped.empty()) return L"当前没有已映射的网络驱动器。";
            wstring items;
            for (size_t i = 0; i < mapped.size() && i < 12; ++i) {
                items += L"  · " +
                         (mapped[i].local.empty() ? mapped[i].remote
                                                  : mapped[i].local + L" → " + mapped[i].remote) +
                         L"\n";
            }
            return L"将强制断开全部 " + I64((long long)mapped.size()) +
                   L" 个网络连接（fForce，未保存的工作可能受影响）：\n" + items +
                   L"\n是否继续？";
        }
        return L"";
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        if (actionIndex < 0 || actionIndex >= (int)acts.size()) return;
        const wstring label = acts[actionIndex].label;

        int okN = 0, failN = 0, skipN = 0;
        auto tally = [&](bool ok, bool skipped, const wstring& msg) {
            if (skipped) { ++skipN; ctx.Log(L"  ○ 跳过: " + msg); }
            else if (ok) { ++okN; ctx.Log(L"  ✓ " + msg); }
            else { ++failN; ctx.Log(L"  ✗ " + msg); }
        };

        switch (actionIndex) {
        case 0: DoMap(ctx, tally); break;
        case 1: DoDisconnect(ctx, tally); break;
        case 2:   // 仅扫描共享（刷新）= 重跑扫描
            Run(ctx, out);
            return;
        case 3: DoOpenFolder(ctx, tally); break;
        case 4: DoDisconnectAll(ctx, tally); break;
        }

        out.summary = L"「" + label + L"」完成：成功 " + I64(okN) +
                      L"，跳过 " + I64(skipN) + L"，失败 " + I64(failN);
        out.canClean = true;
        ctx.Report(100, out.summary);
    }

private:
    wstring cred_;    // 路径框：user:pass（仅会话内）
    wstring drive_;   // 目标框：盘符
    vector<int> sel_;                 // 最近一次勾选的 tag 列表
    vector<ShareItem> scanCache_;     // 扫描结果缓存（tag → 共享）

    // 已映射行 tag → 本地盘符；扫描行 tag → 远程名（均可直接喂 WNetCancelConnection2）
    wstring NameForTag(int tag) const {
        if (tag >= kMapTagBase) {
            vector<MappedPair> mapped;
            EnumMapped(&mapped);
            int i = tag - kMapTagBase;
            if (i >= 0 && i < (int)mapped.size())
                return mapped[i].local.empty() ? mapped[i].remote : mapped[i].local;
            return L"";
        }
        if (tag >= 0 && tag < (int)scanCache_.size()) return scanCache_[tag].remote;
        return L"";
    }

    // 目标框 → 指定盘符字母（0 = 未指定/不合法）
    wchar_t DriveLetterFromTarget() const {
        for (wchar_t c : drive_)
            if (iswalpha(c)) return (wchar_t)towupper(c);
        return 0;
    }

    // 空闲盘符池：GetLogicalDrives 位图从 Z 往前
    vector<wchar_t> FreeLetters() const {
        DWORD mask = GetLogicalDrives();
        vector<wchar_t> out;
        for (wchar_t c = L'Z'; c >= L'A'; --c)
            if (!(mask & (1u << (c - L'A')))) out.push_back(c);
        return out;
    }

    // 枚举本机当前网络连接（RESOURCE_CONNECTED）
    static bool EnumMapped(vector<MappedPair>* outList) {
        outList->clear();
        HANDLE h = nullptr;
        if (WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr, &h) !=
            NO_ERROR)
            return false;
        char buf[16384];
        DWORD cnt = 0xFFFFFFFF, sz = sizeof(buf);
        for (;;) {
            DWORD rc = WNetEnumResourceW(h, &cnt, buf, &sz);
            if (rc != NO_ERROR) break;
            auto* r = (NETRESOURCEW*)buf;
            for (DWORD i = 0; i < cnt; ++i) {
                MappedPair m;
                if (r[i].lpLocalName) m.local = r[i].lpLocalName;
                if (r[i].lpRemoteName) m.remote = r[i].lpRemoteName;
                if (!m.local.empty() || !m.remote.empty()) outList->push_back(m);
            }
            cnt = 0xFFFFFFFF;
            sz = sizeof(buf);
        }
        WNetCloseEnum(h);
        return true;
    }

    // 主机名 → IPv4 字符串（失败返回空）
    static wstring ResolveIpv4Str(const wstring& host) {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        ADDRINFOW* res = nullptr;
        if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) != 0 || !res) return L"";
        wstring out;
        for (auto* p = res; p; p = p->ai_next) {
            if (p->ai_family == AF_INET && p->ai_addr) {
                const auto* s4 = (const sockaddr_in*)p->ai_addr;
                IN_ADDR a{};
                a.S_un.S_addr = s4->sin_addr.S_un.S_addr;
                wchar_t b[INET_ADDRSTRLEN] = {0};
                if (InetNtopW(AF_INET, &a, b, INET_ADDRSTRLEN)) {
                    out = b;
                    break;
                }
            }
        }
        FreeAddrInfoW(res);
        return out;
    }

    // 解析凭据 "user:pass"（首个冒号分隔；无冒号 = 纯用户名空密码）
    void SplitCred(wstring* user, wstring* pass) const {
        *user = L"";
        *pass = L"";
        wstring c = Trim(cred_);
        if (c.empty()) return;
        size_t p = c.find(L':');
        if (p == wstring::npos) {
            *user = c;
        } else {
            *user = c.substr(0, p);
            *pass = c.substr(p + 1);
        }
    }

    // ---------------- 动作 0：映射选中为网盘 ----------------
    void DoMap(ModuleContext& ctx,
               const std::function<void(bool, bool, const wstring&)>& tally) {
        // 过滤出扫描行 tag
        vector<int> tags;
        for (int t : sel_)
            if (t >= 0 && t < (int)scanCache_.size()) tags.push_back(t);
        if (tags.empty()) {
            tally(false, true, L"未勾选任何可映射的共享行（先扫描并勾选）");
            return;
        }
        wstring user, pass;
        SplitCred(&user, &pass);

        // 盘符池：用户指定字母优先消费一次，其余自动 Z→A
        vector<wchar_t> pool = FreeLetters();
        wchar_t fixedLetter = DriveLetterFromTarget();
        if (fixedLetter) {
            DWORD mask = GetLogicalDrives();
            if (mask & (1u << (fixedLetter - L'A'))) {
                ctx.Log(L"  指定盘符 " + wstring(1, fixedLetter) +
                        L": 已被占用，改用自动分配");
                fixedLetter = 0;
            }
        }

        ctx.Log(L"映射 " + I64((long long)tags.size()) + L" 项" +
                (user.empty() ? L"（当前凭据）" : L"（凭据用户 " + user + L"，仅会话内使用）"));
        for (int t : tags) {
            const wstring& remote = scanCache_[t].remote;
            wchar_t letter = 0;
            if (fixedLetter) {
                letter = fixedLetter;
                fixedLetter = 0;   // 指定盘符只给第一项
            } else if (!pool.empty()) {
                letter = pool.front();
                pool.erase(pool.begin());
            }
            if (!letter) {
                tally(false, true, remote + L" — 无空闲盘符可用");
                continue;
            }
            wchar_t local[4] = {letter, L':', 0, 0};
            NETRESOURCEW nr{};
            nr.dwType = RESOURCETYPE_DISK;
            nr.lpLocalName = local;
            nr.lpRemoteName = (LPWSTR)remote.c_str();
            DWORD rc = WNetAddConnection2W(&nr,
                                           pass.empty() ? nullptr : pass.c_str(),
                                           user.empty() ? nullptr : user.c_str(),
                                           CONNECT_UPDATE_PROFILE);
            if (rc == NO_ERROR) {
                tally(true, false, remote + L" → " + wstring(local));
            } else if (rc == ERROR_LOGON_FAILURE || rc == ERROR_LOGON_TYPE_NOT_GRANTED ||
                       rc == ERROR_ACCESS_DENIED) {
                tally(false, false, remote + L" — 需凭据：" +
                      ErrZh(rc) + L"；在路径框填 user:pass 格式后重试（本动作目标框填盘符）");
            } else if (rc == ERROR_ALREADY_ASSIGNED) {
                tally(false, false, remote + L" — 盘符 " + wstring(local) +
                      L" 已被占用：" + ErrZh(rc));
            } else {
                tally(false, false, remote + L" → " + wstring(local) + L" — " + ErrZh(rc));
            }
        }
    }

    // ---------------- 动作 1：断开选中连接 ----------------
    void DoDisconnect(ModuleContext& ctx,
                      const std::function<void(bool, bool, const wstring&)>& tally) {
        if (sel_.empty()) {
            tally(false, true, L"未勾选任何行");
            return;
        }
        for (int t : sel_) {
            wstring name = NameForTag(t);
            if (name.empty()) {
                tally(false, true, L"行已过期（扫描结果已刷新），tag=" + I64(t));
                continue;
            }
            DWORD rc = WNetCancelConnection2W(name.c_str(), 0, FALSE);
            if (rc == NO_ERROR)
                tally(true, false, L"已断开 " + name);
            else if (rc == ERROR_OPEN_FILES || rc == ERROR_DEVICE_IN_USE)
                tally(false, false, name + L" — 有打开的文件，未强制断开（可用「断开全部网盘」）：" +
                      ErrZh(rc));
            else
                tally(false, false, name + L" — " + ErrZh(rc));
        }
    }

    // ---------------- 动作 3：打开共享文件夹 ----------------
    void DoOpenFolder(ModuleContext& ctx,
                      const std::function<void(bool, bool, const wstring&)>& tally) {
        wstring target;
        for (int t : sel_) {
            if (t >= kMapTagBase) {
                vector<MappedPair> mapped;
                EnumMapped(&mapped);
                int i = t - kMapTagBase;
                if (i >= 0 && i < (int)mapped.size()) {
                    target = mapped[i].remote;
                    break;
                }
            }
            if (t >= 0 && t < (int)scanCache_.size()) {
                target = scanCache_[t].remote;
                break;
            }
        }
        if (target.empty()) {
            tally(false, true, L"未勾选任何可打开的共享行");
            return;
        }
        HINSTANCE h = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                                    SW_SHOWNORMAL);
        INT_PTR r = (INT_PTR)h;
        if (r > 32)
            tally(true, false, L"已在资源管理器打开 " + target);
        else
            tally(false, false, target + L" — 打开失败（ShellExecute 错误码 " +
                  I64((long long)r) + L"，目标可能不可访问）");
    }

    // ---------------- 动作 4：断开全部网盘（fForce）----------------
    void DoDisconnectAll(ModuleContext& ctx,
                         const std::function<void(bool, bool, const wstring&)>& tally) {
        vector<MappedPair> mapped;
        EnumMapped(&mapped);
        if (mapped.empty()) {
            tally(false, true, L"当前没有已映射的网络连接");
            return;
        }
        for (const MappedPair& m : mapped) {
            wstring name = m.local.empty() ? m.remote : m.local;
            DWORD rc = WNetCancelConnection2W(name.c_str(), 0, TRUE);   // fForce
            if (rc == NO_ERROR)
                tally(true, false, L"已强制断开 " + name + L" → " + m.remote);
            else
                tally(false, false, name + L" — " + ErrZh(rc));
        }
    }
};

REGISTER_MODULE(NetShareMapModule);

} // namespace mz
