// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 组网助手（v2.16 需求）
//  ---------------------------------------------------------------------------
//  net.mesh  对接自托管 Tailscale 控制服务器（headscale）的客户端接入：
//    * 配置字符串 NATBMESH1|<login-server>|<preauthkey>：剪贴板一键导入
//      （容忍首尾空白/引号，格式头不符给中文归因），字段持久化沿用框架
//      （密钥 secret 字段 DPAPI 加密落盘）
//    * 加入网络：检测 tailscale.exe（注册表 HKLM\SOFTWARE\Tailscale IPN
//      的 Path 值 + 常见路径 + PATH 探测）；未安装给出 winget 引导并可
//      静默安装（tailscale.tailscale）；已装执行
//      tailscale up --login-server=… --authkey=… --accept-dns=false，
//      完成后 status 确认并输出本机 Tailscale IP；非提权失败如实中文归因
//    * 退出网络（tailscale down）/ 注销并重置（tailscale logout，danger）
//    * 生成接入二维码：自研 QR 编码器（src/qr.h，byte mode + ECC M +
//      v1~10）；GUI 弹独立窗口位图绘制（黑模块白底 quiet zone 4），
//      CLI 输出 Unicode 半块字符二维码到 stdout（终端可直接扫码）
//    * 扫描：tailscale status --json 解析——本机节点 + 对端列表
//      （主机名 / Tailscale IP / 在线状态 / 系统·版本）
//  链接：无新增依赖库（build.bat 已统一追加 user32 等）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../procrun.h"
#include "../cleanresult.h"
#include "../qr.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mz {
namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32m(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring TrimWs(wstring s) {
    const wchar_t* ws = L" \t\r\n\u3000";
    size_t b = s.find_first_not_of(ws);
    if (b == wstring::npos) return wstring();
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// 去掉首尾成对引号（导入容错：终端复制常带引号）
wstring StripQuotes(wstring s) {
    s = TrimWs(std::move(s));
    while (s.size() >= 2) {
        wchar_t a = s.front(), b = s.back();
        bool pair = (a == L'"' && b == L'"') || (a == L'\u201C' && b == L'\u201D') ||
                    (a == L'\'' && b == L'\'');
        if (!pair) break;
        s = s.substr(1, s.size() - 2);
        s = TrimWs(std::move(s));
    }
    return s;
}

// 密钥脱敏展示：保留前 4 后 3，中间省略
wstring MaskKey(const wstring& k) {
    if (k.size() <= 8) return L"***";
    return k.substr(0, 4) + L"…" + k.substr(k.size() - 3);
}

// 服务器地址脱敏（二维码窗口标题用）：去协议头 + 截断
wstring SanitizeServer(const wstring& s) {
    wstring out = s;
    size_t p = out.find(L"://");
    if (p != wstring::npos) out = out.substr(p + 3);
    if (out.size() > 28) out = out.substr(0, 28) + L"…";
    return out;
}

wstring Lower(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

// ---------------------------------------------------------- Tailscale 客户端探测

bool FileExistsM(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 注册表直读字符串值（HKLM，沙箱友好——不经 reg.exe）
bool RegReadString(HKEY root, const wchar_t* sub, const wchar_t* name, wstring* out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG rc = RegQueryValueExW(h, name, nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    *out = wstring(buf);
    return true;
}

// tailscale.exe 探测顺序：注册表 → 常见安装路径 → PATH
wstring DetectTailscale() {
    wstring p;
    if (RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Tailscale IPN", L"Path", &p)) {
        p = TrimWs(std::move(p));
        if (!p.empty()) {
            if (FileExistsM(p) && Lower(p).ends_with(L"tailscale.exe")) return p;
            wstring cand = p;
            if (cand.back() != L'\\') cand += L'\\';
            cand += L"tailscale.exe";
            if (FileExistsM(cand)) return cand;
        }
    }
    for (const wchar_t* cand : { L"C:\\Program Files\\Tailscale\\tailscale.exe",
                                 L"C:\\Program Files (x86)\\Tailscale\\tailscale.exe" }) {
        if (FileExistsM(cand)) return cand;
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"tailscale.exe", nullptr, MAX_PATH, found, nullptr) > 0)
        return found;
    return wstring();
}

// 经 procrun 流式执行 tailscale 子命令
int RunTailscale(const wstring& exe, const wstring& args, ModuleContext& ctx,
                 wstring* fullOut, int timeoutMs = 60000) {
    ProcRunOptions opt;
    opt.timeoutMs = timeoutMs;
    opt.cancel = ctx.cancel;
    opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  [tailscale] " + line); };
    return RunProcess(L"\"" + exe + L"\" " + args, opt, fullOut);
}

// ---------------------------------------------------------- 极简 JSON 读取
// 只面向 tailscale status --json 的机器生成格式（"Key": value，无注释）。

size_t SkipWs(const wstring& s, size_t i) {
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n')) ++i;
    return i;
}

// 找 "key" 对应值的起始下标（key 必须整串匹配）；找不到返回 npos
size_t JsonFindValue(const wstring& s, const wstring& key, size_t from = 0) {
    wstring pat = L"\"" + key + L"\"";
    size_t p = s.find(pat, from);
    while (p != wstring::npos) {
        size_t i = SkipWs(s, p + pat.size());
        if (i < s.size() && s[i] == L':') return SkipWs(s, i + 1);
        p = s.find(pat, p + 1);
    }
    return wstring::npos;
}

// 平衡读取一段（obj/str/array），返回结束下标（含尾字符）
size_t JsonSpan(const wstring& s, size_t start) {
    if (start >= s.size()) return start;
    wchar_t open = s[start];
    wchar_t close = open == L'{' ? L'}' : (open == L'[' ? L']' : L'"');
    int depth = 0;
    bool inStr = false;
    for (size_t i = start; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (inStr) {
            if (c == L'\\') { ++i; continue; }
            if (c == L'"') inStr = false;
            continue;
        }
        if (open == L'"') return i;                       // 单字符串在收尾引号结束
        if (c == L'"') { inStr = true; continue; }
        if (c == open) ++depth;
        else if (c == close && --depth == 0) return i;
    }
    return s.size() - 1;
}

bool JsonGetObj(const wstring& s, const wstring& key, wstring* out, size_t from = 0) {
    size_t v = JsonFindValue(s, key, from);
    if (v == wstring::npos || v >= s.size() || s[v] != L'{') return false;
    size_t e = JsonSpan(s, v);
    *out = s.substr(v, e - v + 1);
    return true;
}

wstring JsonGetString(const wstring& s, const wstring& key) {
    size_t v = JsonFindValue(s, key);
    if (v == wstring::npos || v >= s.size() || s[v] != L'"') return wstring();
    size_t e = JsonSpan(s, v);
    wstring raw = s.substr(v + 1, e - v - 1);
    wstring out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\\' && i + 1 < raw.size()) {
            ++i;
            out += raw[i] == L'n' ? L'\n' : raw[i];
            continue;
        }
        out += raw[i];
    }
    return out;
}

bool JsonGetBool(const wstring& s, const wstring& key) {
    size_t v = JsonFindValue(s, key);
    if (v == wstring::npos) return false;
    return s.compare(v, 4, L"true") == 0;
}

vector<wstring> JsonGetArrayStrings(const wstring& s, const wstring& key) {
    vector<wstring> out;
    size_t v = JsonFindValue(s, key);
    if (v == wstring::npos || v >= s.size() || s[v] != L'[') return out;
    size_t e = JsonSpan(s, v);
    wstring body = s.substr(v + 1, e - v - 1);
    size_t i = 0;
    while ((i = body.find(L'"', i)) != wstring::npos) {
        size_t j = JsonSpan(body, i);
        if (j == wstring::npos || j <= i) break;
        out.push_back(body.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    return out;
}

// ---------------------------------------------------------- 剪贴板读取

bool ReadClipboardText(HWND hwnd, wstring* out) {
    if (!OpenClipboard(hwnd)) hwnd = nullptr;
    if (!OpenClipboard(hwnd)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            *out = p;
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

// ---------------------------------------------------------- QR 弹窗（工作线程自有消息循环）

constexpr int kIdField0Gui = 1032;   // 主窗口扩展字段输入行 ID（v2.12 R3 框架约定）

struct QrWindowState {
    const std::vector<std::vector<bool>>* m = nullptr;
    int scale = 6;
};

LRESULT CALLBACK QrWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        auto* st = (QrWindowState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (st && st->m) {
            const int n = (int)st->m->size(), q = 4;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) {
                    if (!(*st->m)[r][c]) continue;
                    RECT cell{ (q + c) * st->scale, (q + r) * st->scale,
                               (q + c + 1) * st->scale, (q + r + 1) * st->scale };
                    FillRect(dc, &cell, black);
                }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// 弹出二维码窗口（模态于本工作线程：阻塞至关闭；不触碰主窗口消息循环）
void ShowQrWindow(const std::vector<std::vector<bool>>& m, const wstring& title) {
    const int n = (int)m.size();
    const int total = n + 8;                                  // quiet zone 4
    int scale = std::max(3, 460 / total);                     // 窗口约 ≤460px
    WNDCLASSW wc = {};
    wc.lpfnWndProc = QrWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MzMeshQrWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);

    RECT rc{ 0, 0, total * scale, total * scale };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"MzMeshQrWindow", title.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                    WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;
    QrWindowState st{ &m, scale };
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(L"MzMeshQrWindow", wc.hInstance);
}

// ================================================================ net.mesh

class NetMesh final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.mesh"; }
    const wchar_t* Group() const override { return L"网络管理"; }
    const wchar_t* Title() const override { return L"组网助手"; }
    const wchar_t* Description() const override {
        return L"对接自托管 Tailscale 控制服务器（headscale）：配置字符串剪贴板一键"
               L"导入、生成接入二维码（扫码即配置）；一键加入/退出网络，未装客户端"
               L"自动引导 winget 安装；扫描展示本机与全部对端节点的 IP/在线/版本";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* RunLabel() const override { return L"扫描网络状态"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"服务器地址", L"headscale 服务器，如 https://mesh.example.com", false },
            { L"预授权密钥", L"headscale preauthkeys create 生成（仅内存/DPAPI 暂存）", true },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fServer_ = values[0];
        if (values.size() > 1) fKey_ = values[1];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"加入网络",     nullptr, false, 0xE703 },  // 0 → btnClean 位
            { L"退出网络",     nullptr, false, 0xE712 },  // 1 → btnUndo 位
            { L"导入配置字符串", nullptr, false, 0 },      // 2 → 更多 ▾
            { L"生成接入二维码", nullptr, false, 0 },      // 3 → 更多 ▾
            { L"检测客户端",   nullptr, false, 0 },        // 4 → 更多 ▾
            { L"注销并重置",   nullptr, true,  0 },        // 5 → 更多 ▾（danger）
        };
    }

    wstring ActionWarning(int actionIndex, const vector<int>&) const override {
        switch (actionIndex) {
        case 0:
            if (TrimWs(fServer_).empty() || TrimWs(fKey_).empty())
                return L"请先在上方填写「服务器地址」与「预授权密钥」"
                       L"（或「更多 ▾ → 导入配置字符串」自动回填）。";
            return L"将执行 tailscale up 接入：\n  服务器：" + SanitizeServer(TrimWs(fServer_)) +
                   L"\n  密钥：" + MaskKey(TrimWs(fKey_)) +
                   L"\n\n需管理员权限（tailscale 服务授权）；成功后自动查询本机 Tailscale IP。"
                   L"\n是否继续？";
        case 1:
            return L"将执行 tailscale down 断开组网（保留节点身份，可重新加入）。\n\n是否继续？";
        case 2:
            return L"将从剪贴板读取组网配置字符串（格式：NATBMESH1|服务器地址|预授权密钥），"
                   L"解析成功后自动回填上方两个字段。\n\n是否继续？";
        case 3:
            if (TrimWs(fServer_).empty() || TrimWs(fKey_).empty())
                return L"请先在上方填写「服务器地址」与「预授权密钥」。";
            return L"将把当前配置字符串编码为二维码"
                   L"（图形界面弹窗展示，命令行输出到终端，均可直接扫码）。\n\n是否继续？";
        case 4:
            return L"将检测本机 Tailscale 客户端安装状态与版本；"
                   L"未安装时提供 winget 静默安装。\n\n是否继续？";
        case 5:
            return L"将执行 tailscale logout：注销本机节点身份并删除密钥，"
                   L"重新入网需服务端重新授权。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    // ---------------------------------------------------------- 扫描：status --json

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        ctx.Report(-1, L"正在查询 Tailscale 状态…");

        wstring exe = DetectTailscale();
        if (exe.empty()) {
            ModuleRow r;
            r.c1 = L"Tailscale 客户端";
            r.c2 = L"未安装";
            r.c3 = L"客户端";
            r.c4 = L"更多 ▾ → 检测客户端 可静默安装（winget · tailscale.tailscale）";
            r.danger = true;
            out.rows.push_back(std::move(r));
            out.summary = L"未检测到 Tailscale 客户端";
            out.hint = wstring(T(L"「更多 ▾ → 导入配置字符串」回填服务器与密钥后即可「加入网络」；"))
                       + T(L"加入时若未安装会自动引导 winget 静默安装。");
            out.canClean = true;
            ctx.Report(100, L"Tailscale 未安装");
            return;
        }

        wstring full;
        int code = RunTailscale(exe, L"status --json", ctx, &full, 30000);
        if (code != 0 || full.empty()) {
            ModuleRow r;
            r.c1 = L"tailscale status";
            r.c2 = L"查询失败";
            r.c3 = L"状态";
            r.c4 = (code == -2) ? wstring(L"查询超时（tailscale 服务可能未响应）")
                                : (L"tailscale 服务未运行或未登录（退出码 " +
                                   U32m((unsigned)code) + L"）");
            r.danger = true;
            out.rows.push_back(std::move(r));
            out.summary = L"Tailscale 状态查询失败（服务未运行？）";
            out.hint = T(L"客户端已安装：") + exe;
            out.canClean = true;
            ctx.Report(100, L"status 查询失败");
            return;
        }

        wstring backend = JsonGetString(full, L"BackendState");
        wstring clientVer = JsonGetString(full, L"Version");
        wstring self;
        bool online = false;
        wstring host, osName, selfIp;
        if (JsonGetObj(full, L"Self", &self)) {
            host = JsonGetString(self, L"HostName");
            osName = JsonGetString(self, L"OS");
            online = JsonGetBool(self, L"Online");
            vector<wstring> ips = JsonGetArrayStrings(self, L"TailscaleIPs");
            if (ips.empty()) ips = JsonGetArrayStrings(self, L"Addresses");
            if (!ips.empty()) selfIp = ips[0];
        }
        if (selfIp.empty()) {
            vector<wstring> ips = JsonGetArrayStrings(full, L"TailscaleIPs");
            if (!ips.empty()) selfIp = ips[0];
        }

        ModuleRow me;
        me.c1 = host.empty() ? wstring(L"(本机)") : host;
        me.c2 = selfIp.empty() ? L"—" : selfIp;
        me.c3 = L"本机";
        me.c4 = backend + (osName.empty() ? wstring() : L" · " + osName) +
                L" · " + (online ? L"在线" : L"离线");
        me.tag = -1;
        out.rows.push_back(std::move(me));

        wstring peerObj;
        int peerCnt = 0, peerOnline = 0;
        if (JsonGetObj(full, L"Peer", &peerObj) && peerObj.size() > 2) {
            size_t pos = 1;
            for (;;) {
                size_t q1 = peerObj.find(L'"', pos);
                if (q1 == wstring::npos) break;
                size_t q2 = peerObj.find(L'"', q1 + 1);
                if (q2 == wstring::npos) break;
                size_t v = SkipWs(peerObj, q2 + 1);
                if (v >= peerObj.size() || peerObj[v] != L':') { pos = q2 + 1; continue; }
                v = SkipWs(peerObj, v + 1);
                if (v >= peerObj.size() || peerObj[v] != L'{') { pos = q2 + 1; continue; }
                size_t e = JsonSpan(peerObj, v);
                wstring peer = peerObj.substr(v, e - v + 1);
                pos = e + 1;

                ModuleRow r;
                r.c1 = JsonGetString(peer, L"HostName");
                if (r.c1.empty()) r.c1 = JsonGetString(peer, L"DNSName");
                vector<wstring> ips = JsonGetArrayStrings(peer, L"TailscaleIPs");
                if (ips.empty()) ips = JsonGetArrayStrings(peer, L"Addresses");
                r.c2 = ips.empty() ? L"—" : ips[0];
                bool pOn = JsonGetBool(peer, L"Online");
                r.c3 = pOn ? L"在线" : L"离线";
                if (pOn) ++peerOnline;
                wstring pv = JsonGetString(peer, L"Version");
                wstring po = JsonGetString(peer, L"OS");
                r.c4 = po + (pv.empty() ? wstring() : L" · " + pv);
                r.tag = -1;
                ++peerCnt;
                out.rows.push_back(std::move(r));
            }
        }

        wchar_t b[160];
        swprintf_s(b, 160, L"本机 %s（%s）· 对端 %d 台（%d 在线）· 客户端 v%s",
                   host.c_str(), backend.c_str(), peerCnt, peerOnline, clientVer.c_str());
        out.summary = b;
        out.hint = wstring(T(L"「加入网络」按上方字段接入；「导入配置字符串」可一键回填；"))
                   + T(L"「生成接入二维码」供手机 Tailscale 客户端扫码核对。");
        out.canClean = true;
        ctx.Report(100, L"状态查询完成");
    }

    // ---------------------------------------------------------- 动作

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: ActionJoin(ctx, out); break;
        case 1: ActionDown(ctx, out); break;
        case 2: ActionImport(ctx, out); break;
        case 3: ActionQr(ctx, out); break;
        case 4: ActionDetect(ctx, out); break;
        case 5: ActionLogout(ctx, out); break;
        default: break;
        }
    }

private:
    wstring fServer_, fKey_;

    // 安装引导：winget 静默安装（未安装时使用）
    bool TryInstallViaWinget(ModuleContext& ctx) {
        ctx.Log(L"尝试 winget 静默安装 Tailscale（tailscale.tailscale）…");
        ProcRunOptions opt;
        opt.timeoutMs = 600000;
        opt.cancel = ctx.cancel;
        opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  [winget] " + line); };
        wstring full;
        int code = RunProcess(
            L"winget.exe install --id tailscale.tailscale --silent "
            L"--accept-package-agreements --accept-source-agreements",
            opt, &full);
        if (code == 0) {
            ctx.Log(L"winget 安装完成");
            return true;
        }
        ctx.Log(wstring(L"winget 安装未成功：退出码 ") + U32m((unsigned)code) +
                L"（可在管理员终端手动执行 winget install tailscale.tailscale）");
        return false;
    }

    void ActionJoin(ModuleContext& ctx, ModuleResult& out) {
        wstring server = TrimWs(fServer_), key = TrimWs(fKey_);
        if (server.empty() || key.empty()) {
            out.summary = L"请先填写「服务器地址」与「预授权密钥」"
                          L"（或「导入配置字符串」自动回填）";
            out.canClean = true;
            return;
        }
        if (Lower(server).rfind(L"http://", 0) != 0 &&
            Lower(server).rfind(L"https://", 0) != 0) {
            out.summary = L"服务器地址需以 http:// 或 https:// 开头（当前：" + server + L"）";
            out.canClean = true;
            return;
        }
        ctx.Log(L"加入网络：" + SanitizeServer(server) + L"（密钥 " + MaskKey(key) + L"）");
        if (!IsElevated())
            ctx.Log(L"提示：当前非管理员运行，tailscale up 可能被拒绝"
                    L"（tailscale 需管理员或已配置 operator）");

        wstring exe = DetectTailscale();
        if (exe.empty()) {
            ctx.Log(L"未检测到 Tailscale 客户端，先安装…");
            if (!TryInstallViaWinget(ctx)) {
                out.summary = L"未检测到 Tailscale 客户端且 winget 安装未成功，无法加入网络";
                out.canClean = true;
                return;
            }
            exe = DetectTailscale();
            if (exe.empty()) {
                out.summary = L"winget 安装后仍未探测到 tailscale.exe（可能需重新扫描 PATH）";
                out.canClean = true;
                return;
            }
            ctx.Log(L"客户端就绪：" + exe);
        }

        wstring full;
        int code = RunTailscale(exe,
                                L"up --login-server=" + server +
                                    L" --authkey=" + key + L" --accept-dns=false",
                                ctx, &full, 120000);
        if (code != 0) {
            wstring low = Lower(full);
            wstring why;
            if (code == -1) why = L"tailscale.exe 启动失败";
            else if (code == -2) why = L"执行超时被终止（服务器不可达或密钥交换卡住）";
            else if (code == -3) why = L"已取消";
            else if (low.find(L"denied") != wstring::npos ||
                     low.find(L"administrator") != wstring::npos ||
                     low.find(L"insufficient") != wstring::npos)
                why = L"需要管理员权限（tailscale up 需提权，或 tailscale set --operator 配置当前用户）";
            else if (low.find(L"key") != wstring::npos &&
                     (low.find(L"invalid") != wstring::npos || low.find(L"expired") != wstring::npos))
                why = L"预授权密钥无效或已过期（服务端 headscale preauthkeys create 重新生成）";
            else
                why = (L"退出码 " + U32m((unsigned)code) +
                       (full.empty() ? wstring() : L" · " + full));
            out.summary = L"加入失败：" + why;
            out.canClean = true;
            return;
        }

        // 成功 → status 确认并输出本机 Tailscale IP
        wstring st;
        int sc = RunTailscale(exe, L"status --json", ctx, &st, 30000);
        wstring ip;
        if (sc == 0 && !st.empty()) {
            vector<wstring> ips = JsonGetArrayStrings(st, L"TailscaleIPs");
            if (ips.empty()) {
                wstring self;
                if (JsonGetObj(st, L"Self", &self))
                    ips = JsonGetArrayStrings(self, L"Addresses");
            }
            if (!ips.empty()) ip = ips[0];
        }
        out.summary = L"已加入网络" + (ip.empty() ? wstring(L"") : L"：本机 Tailscale IP " + ip);
        out.canClean = true;
        ctx.Report(100, L"加入完成");
    }

    void ActionDown(ModuleContext& ctx, ModuleResult& out) {
        wstring exe = DetectTailscale();
        if (exe.empty()) {
            out.summary = L"未检测到 Tailscale 客户端，无需退出";
            out.canClean = true;
            return;
        }
        wstring full;
        int code = RunTailscale(exe, L"down", ctx, &full, 30000);
        out.summary = (code == 0)
            ? wstring(L"已退出组网（tailscale down，节点身份保留）")
            : (wstring(L"退出失败：") +
               (code == -2 ? wstring(L"执行超时")
                           : (L"退出码 " + U32m((unsigned)code) +
                              (full.empty() ? wstring() : L" · " + full))));
        out.canClean = true;
    }

    void ActionLogout(ModuleContext& ctx, ModuleResult& out) {
        wstring exe = DetectTailscale();
        if (exe.empty()) {
            out.summary = L"未检测到 Tailscale 客户端，无需注销";
            out.canClean = true;
            return;
        }
        wstring full;
        int code = RunTailscale(exe, L"logout", ctx, &full, 60000);
        out.summary = (code == 0)
            ? wstring(L"已注销并重置（节点身份与密钥已删除；重新入网需服务端重新授权）")
            : (wstring(L"注销失败：") +
               (code == -2 ? wstring(L"执行超时")
                           : (L"退出码 " + U32m((unsigned)code) +
                              (full.empty() ? wstring() : L" · " + full))));
        out.canClean = true;
    }

    // 导入配置字符串：剪贴板 → 解析 → 回填字段
    void ActionImport(ModuleContext& ctx, ModuleResult& out) {
        wstring text;
        if (!ReadClipboardText(ctx.hwnd, &text)) {
            out.summary = L"读取剪贴板失败（剪贴板被其他程序占用）";
            out.canClean = true;
            return;
        }
        wstring cfg = StripQuotes(std::move(text));
        if (cfg.empty()) {
            out.summary = L"剪贴板为空：请先复制配置字符串（NATBMESH1|服务器地址|预授权密钥）";
            out.canClean = true;
            return;
        }
        // 分段（'|' 分隔，恰好 3 段）
        vector<wstring> parts;
        size_t start = 0;
        for (;;) {
            size_t p = cfg.find(L'|', start);
            if (p == wstring::npos) { parts.push_back(cfg.substr(start)); break; }
            parts.push_back(cfg.substr(start, p - start));
            start = p + 1;
        }
        if (parts.size() == 1) {
            out.summary = L"导入失败：剪贴板内容不是组网配置字符串（缺少分隔符 |）";
            out.canClean = true;
            return;
        }
        if (parts.size() != 3) {
            out.summary = L"导入失败：配置串应为 3 段（格式头|服务器地址|预授权密钥），实际 " +
                          U32m((unsigned)parts.size()) + L" 段";
            out.canClean = true;
            return;
        }
        wstring head = TrimWs(parts[0]);
        wstring server = StripQuotes(parts[1]);
        wstring key = StripQuotes(parts[2]);
        if (head != L"NATBMESH1") {
            out.summary = L"导入失败：配置格式头不匹配（期望 NATBMESH1，实际「" +
                          (head.size() > 24 ? head.substr(0, 24) + L"…" : head) + L"」）";
            out.canClean = true;
            return;
        }
        if (server.empty() || key.empty()) {
            out.summary = L"导入失败：服务器地址或预授权密钥为空";
            out.canClean = true;
            return;
        }
        if (Lower(server).rfind(L"http://", 0) != 0 &&
            Lower(server).rfind(L"https://", 0) != 0) {
            out.summary = L"导入失败：服务器地址需以 http:// 或 https:// 开头（当前：" +
                          (server.size() > 40 ? server.substr(0, 40) + L"…" : server) + L"）";
            out.canClean = true;
            return;
        }
        fServer_ = server;
        fKey_ = key;
        // GUI：尽力回填输入框（扩展字段 ID 1032/1033，v2.12 R3 框架约定）
        if (ctx.hwnd) {
            HWND e0 = GetDlgItem(ctx.hwnd, kIdField0Gui);
            HWND e1 = GetDlgItem(ctx.hwnd, kIdField0Gui + 1);
            if (e0) SetWindowTextW(e0, server.c_str());
            if (e1) SetWindowTextW(e1, key.c_str());
        }
        out.summary = L"导入成功：服务器 " + SanitizeServer(server) + L"，密钥 " + MaskKey(key) +
                      L"（已回填上方字段，可直接「加入网络」）";
        out.canClean = true;
        ctx.Report(100, L"配置导入完成");
    }

    // 生成接入二维码：GUI 弹窗 / CLI 终端输出
    void ActionQr(ModuleContext& ctx, ModuleResult& out) {
        wstring server = TrimWs(fServer_), key = TrimWs(fKey_);
        if (server.empty() || key.empty()) {
            out.summary = L"请先填写「服务器地址」与「预授权密钥」"
                          L"（或「导入配置字符串」自动回填）";
            out.canClean = true;
            return;
        }
        wstring cfg = L"NATBMESH1|" + server + L"|" + key;
        auto m = qr::Encode(cfg);
        if (m.empty()) {
            out.summary = L"二维码生成失败：配置串超出编码容量（>213 字节，当前 " +
                          U32m((unsigned)mz::WToUtf8(cfg).size()) + L" 字节）";
            out.canClean = true;
            return;
        }
        int side = (int)m.size();
        if (ctx.hwnd) {
            // GUI：独立窗口位图绘制（黑模块白底，quiet zone 4 由窗口留白承担）
            ShowQrWindow(m, L"接入二维码 · " + SanitizeServer(server) +
                                L"（手机 Tailscale 可扫码核对）");
            ctx.Log(L"二维码已生成并弹出窗口（version 边长 " + U32m((unsigned)side) +
                    L" 模块，quiet zone 4）");
            out.summary = L"接入二维码已弹出（服务器 " + SanitizeServer(server) +
                          L"，密钥 " + MaskKey(key) + L"）";
        } else {
            // CLI：Unicode 半块字符输出到 stdout（终端可直接扫码）
            SetConsoleOutputCP(65001);
            std::string txt = qr::ToConsoleUtf8(m);
            fwrite(txt.data(), 1, txt.size(), stdout);
            fflush(stdout);
            ctx.Log(L"二维码已输出到终端（边长 " + U32m((unsigned)side) +
                    L" 模块 + 4 模块静区，可直接扫码）");
            out.summary = L"接入二维码已打印到终端（内容：NATBMESH1|" +
                          SanitizeServer(server) + L"|<密钥已隐藏>）";
        }
        out.canClean = true;
        ctx.Report(100, L"二维码生成完成");
    }

    // 检测客户端：路径 + 版本；未安装给 winget 引导
    void ActionDetect(ModuleContext& ctx, ModuleResult& out) {
        wstring exe = DetectTailscale();
        if (exe.empty()) {
            ctx.Log(L"未检测到 Tailscale 客户端");
            ctx.Log(L"  探测范围：注册表 HKLM\\SOFTWARE\\Tailscale IPN\\Path、"
                    L"C:\\Program Files\\Tailscale\\、PATH");
            ctx.Log(L"  安装方式：「加入网络」会自动 winget 静默安装，或管理员终端执行：");
            ctx.Log(L"    winget install --id tailscale.tailscale --silent");
            out.summary = L"未检测到 Tailscale 客户端（可经 winget 静默安装）";
            out.canClean = true;
            return;
        }
        ctx.Log(L"客户端：" + exe);
        wstring ver;
        int code = RunTailscale(exe, L"version", ctx, &ver, 15000);
        if (code == 0 && !ver.empty()) {
            size_t nl = ver.find(L'\n');
            wstring first = nl == wstring::npos ? ver : ver.substr(0, nl);
            ctx.Log(L"版本：" + first);
            out.summary = L"Tailscale 客户端已安装：" + exe + L"（" + first + L"）";
        } else {
            out.summary = L"Tailscale 客户端已安装：" + exe + L"（版本查询失败，服务可能未运行）";
        }
        out.canClean = true;
    }
};

} // namespace

REGISTER_MODULE(NetMesh);

} // namespace mz
