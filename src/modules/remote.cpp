// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 远程终端模块集（v2.15 需求 4，新分组）
//  ---------------------------------------------------------------------------
//  net.sessions  远程会话：SSH/SFTP/Telnet 会话档案管理（exe 同目录
//                netadmin-sessions.ini，参照输入持久化框架的 ini 路数）；
//                连接 = Windows 内置 OpenSSH（ssh.exe/sftp.exe）以
//                CREATE_NEW_CONSOLE 新控制台启动；不存密码（连接时交互输入；
//                密码管理器类存储由 pwd.cred 覆盖）。Telnet 缺失提示启用系统功能。
//  net.tunnel    端口转发隧道：-L 本地 / -R 远程 / -D 动态(SOCKS) 三型，
//                ssh -N 生成并启动（记录 PROCESS_INFORMATION 句柄），
//                支持 启动/停止/停止全部/定义持久化与活动状态展示。
//  net.serial    串口终端：QueryDosDevice 枚举 COM 口；DCB 配置
//                （波特率/数据位/停止位）；打开监视（收）与发送一行（发）。
//                图形化交互终端（方向键/滚屏回看）延后——见 README 已知限制。
//  链接：-ladvapi32 -luser32（build.bat 已统一追加；无新增依赖库）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"
#include "../procrun.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mz {
namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32s3(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring FoldLow3(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

wstring ExeDir3() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return wstring(L".");
    wstring p(buf, n);
    size_t s = p.find_last_of(L'\\');
    return (s == wstring::npos) ? wstring(L".") : p.substr(0, s);
}

bool FileExists3(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

wstring WinDir3() {
    wchar_t wd[1024] = {};
    UINT n = GetWindowsDirectoryW(wd, 1024);
    return (n > 0 && n < 1024) ? wstring(wd, n) : wstring(L"C:\\Windows");
}

wstring TrimSp(wstring s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == wstring::npos) return wstring();
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

// 以新控制台启动 exe（CREATE_NEW_CONSOLE；piOut 非空时带回句柄由调用方管理）
bool LaunchNewConsole(const wstring& exe, const wstring& args,
                      wstring* err, PROCESS_INFORMATION* piOut = nullptr) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wstring cmd = L"\"" + exe + L"\" " + args;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        if (err) *err = ErrorTextZh(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    if (piOut) *piOut = pi;
    else CloseHandle(pi.hProcess);
    return true;
}

// ================================================================ net.sessions

struct SessEntry {
    wstring name, proto, host, port;
};

wstring SessionsIni() { return ExeDir3() + L"\\netadmin-sessions.ini"; }

vector<SessEntry> LoadSessions() {
    vector<SessEntry> out;
    const wstring ini = SessionsIni();
    int cnt = GetPrivateProfileIntW(L"sessions", L"count", 0, ini.c_str());
    if (cnt < 0 || cnt > 200) cnt = 0;
    for (int i = 0; i < cnt; ++i) {
        wstring pre = L"s" + U32s3((unsigned)i) + L".";
        wchar_t buf[256] = {};
        SessEntry e;
        auto rd = [&](const wchar_t* key, wstring* dst) {
            GetPrivateProfileStringW(L"sessions", (pre + key).c_str(), L"",
                                     buf, 256, ini.c_str());
            *dst = buf;
        };
        rd(L"name", &e.name);
        rd(L"proto", &e.proto);
        rd(L"host", &e.host);
        rd(L"port", &e.port);
        if (!e.name.empty() && !e.host.empty()) out.push_back(std::move(e));
    }
    return out;
}

void SaveSessions(const vector<SessEntry>& list) {
    const wstring ini = SessionsIni();
    // 清空重写（WritePrivateProfileString 传空 value 删除整个节）
    WritePrivateProfileStringW(L"sessions", nullptr, nullptr, ini.c_str());
    WritePrivateProfileStringW(L"sessions", L"count", U32s3((unsigned)list.size()).c_str(),
                               ini.c_str());
    for (size_t i = 0; i < list.size(); ++i) {
        wstring pre = L"s" + U32s3((unsigned)i) + L".";
        WritePrivateProfileStringW(L"sessions", (pre + L"name").c_str(), list[i].name.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"sessions", (pre + L"proto").c_str(), list[i].proto.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"sessions", (pre + L"host").c_str(), list[i].host.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"sessions", (pre + L"port").c_str(), list[i].port.c_str(), ini.c_str());
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());   // 立即落盘
}

// 规范化协议：sftp / telnet / ssh（默认）
wstring NormProto(const wstring& p) {
    wstring low = FoldLow3(TrimSp(p));
    if (low.find(L"sftp") != wstring::npos) return L"SFTP";
    if (low.find(L"tel") != wstring::npos) return L"Telnet";
    return L"SSH";
}

wstring SshDir() { return WinDir3() + L"\\System32\\OpenSSH"; }

// 会话 → 连接命令行参数（ssh -p port user@host / sftp -P port user@host）
wstring SessArgs(const SessEntry& e) {
    wstring port = TrimSp(e.port);
    if (port.empty() || port == L"0") port = L"22";
    wstring host = TrimSp(e.host);
    if (e.proto == L"Telnet") return host + L" " + port;
    wstring flag = (e.proto == L"SFTP") ? L"-P " : L"-p ";
    return flag + port + L" " + host;
}

class NetSessions final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.sessions"; }
    const wchar_t* Group() const override { return L"远程终端"; }
    const wchar_t* Title() const override { return L"远程会话"; }
    const wchar_t* Description() const override {
        return L"SSH/SFTP/Telnet 会话档案管理（exe 同目录 ini 持久化）；一键连接"
               L"——调用 Windows 内置 OpenSSH 新控制台启动（不存密码，连接时"
               L"交互输入，可配公钥免密）；OpenSSH 缺失时给出安装引导";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    const wchar_t* RunLabel() const override { return L"刷新会话列表"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"会话名称", L"如 车间服务器（保存/更新会话用）", false },
            { L"协议",     L"SSH / SFTP / Telnet（默认 SSH）", false },
            { L"主机",     L"如 192.168.1.10 或 user@192.168.1.10", false },
            { L"端口",     L"默认 22", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fName_ = values[0];
        if (values.size() > 1) fProto_ = values[1];
        if (values.size() > 2) fHost_ = values[2];
        if (values.size() > 3) fPort_ = values[3];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"连接选中",     nullptr, false, 0xE768 },  // 0 → btnClean 位
            { L"删除选中会话", nullptr, true,  0xE74D },  // 1 → btnUndo 位
            { L"保存/更新会话", nullptr, false, 0 },      // 2 → 更多 ▾
            { L"安装 OpenSSH 客户端", nullptr, false, 0 }, // 3 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        switch (actionIndex) {
        case 0: {
            if (tags.empty()) return L"未选中任何会话。请先在列表中勾选要连接的会话。";
            wstring names;
            for (int t : tags)
                if (t >= 0 && t < (int)sess_.size())
                    names += sess_[t].name + L"（" + sess_[t].proto + L" · " +
                             SessArgs(sess_[t]) + L"）\n";
            return L"将为以下会话打开新控制台连接：\n" + names +
                   L"\n密码在新控制台内交互输入（本工具不存储密码）。\n是否继续？";
        }
        case 1: {
            if (tags.empty()) return L"未选中任何会话。";
            wstring names;
            for (int t : tags)
                if (t >= 0 && t < (int)sess_.size()) names += sess_[t].name + L"\n";
            return L"将删除以下会话档案：\n" + names + L"\n是否继续？";
        }
        case 2: {
            if (TrimSp(fName_).empty() || TrimSp(fHost_).empty())
                return L"请先在上方填写「会话名称」与「主机」。";
            return L"将" + wstring(L"保存") + L"会话档案：" + TrimSp(fName_) + L"（" +
                   NormProto(fProto_) + L" · " + TrimSp(fHost_) + L":" +
                   (TrimSp(fPort_).empty() ? wstring(L"22") : TrimSp(fPort_)) +
                   L"）。同名会话将被覆盖。\n是否继续？";
        }
        case 3:
            return L"将通过 PowerShell 安装 Windows 可选功能 OpenSSH.Client"
                   L"（需要管理员权限，可能弹出 UAC）。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        sess_ = LoadSessions();
        bool sshOk = FileExists3(SshDir() + L"\\ssh.exe");
        for (size_t i = 0; i < sess_.size(); ++i) {
            const SessEntry& e = sess_[i];
            ModuleRow r;
            r.c1 = e.name;
            r.c2 = e.proto == L"Telnet" ? e.host : (e.host + L":" +
                   (TrimSp(e.port).empty() ? wstring(L"22") : TrimSp(e.port)));
            r.c3 = e.proto;
            r.c4 = (e.proto == L"Telnet" ? L"telnet " : L"ssh ") + SessArgs(e);
            r.tag = (int)i;
            out.rows.push_back(std::move(r));
        }
        out.summary = L"会话 " + U32s3((unsigned)sess_.size()) + L" 个 · OpenSSH " +
                      (sshOk ? wstring(L"可用") : wstring(L"缺失"));
        out.hint = sshOk
            ? L"在上方填写会话信息后「更多 ▾ → 保存/更新会话」；勾选后「连接选中」。"
              L"推荐配公钥免密：ssh-keygen + type 公钥 >> 服务器 ~/.ssh/authorized_keys。"
            : L"未检测到 Windows 内置 OpenSSH（%WINDIR%\\System32\\OpenSSH）："
              L"「更多 ▾ → 安装 OpenSSH 客户端」，或 设置 → 应用 → 可选功能 安装。";
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 连接选中
            if (selection_.empty()) {
                out.summary = L"未选中任何会话";
                out.canClean = true;
                return;
            }
            int ok = 0, fail = 0;
            for (int t : selection_) {
                if (t < 0 || t >= (int)sess_.size()) continue;
                const SessEntry& e = sess_[t];
                wstring args = SessArgs(e);
                wstring exe, err;
                if (e.proto == L"Telnet") {
                    exe = WinDir3() + L"\\System32\\telnet.exe";
                    if (!FileExists3(exe)) {
                        ++fail;
                        ctx.Log(L"  Telnet 客户端未安装：" + e.name +
                                L" — 请在「设置 → 应用 → 可选功能 → 更多功能」启用"
                                L" Telnet 客户端，或运行 dism /online /Enable-Feature"
                                L" /FeatureName:TelnetClient");
                        continue;
                    }
                } else if (e.proto == L"SFTP") {
                    exe = SshDir() + L"\\sftp.exe";
                } else {
                    exe = SshDir() + L"\\ssh.exe";
                }
                if (!FileExists3(exe)) {
                    ++fail;
                    ctx.Log(L"  未找到：" + exe);
                    continue;
                }
                if (LaunchNewConsole(exe, args, &err)) {
                    ++ok;
                    ctx.Log(L"  已连接：" + e.name + L"（新控制台）");
                } else {
                    ++fail;
                    ctx.Log(L"  启动失败：" + e.name + L" — " + err);
                }
            }
            out.summary = L"连接完成：成功 " + U32s3((unsigned)ok) + L"，失败 " +
                          U32s3((unsigned)fail);
            out.canClean = true;
            break;
        }
        case 1: {   // 删除选中
            if (selection_.empty()) {
                out.summary = L"未选中任何会话";
                out.canClean = true;
                return;
            }
            vector<SessEntry> keep;
            int del = 0;
            for (int i = 0; i < (int)sess_.size(); ++i) {
                bool drop = false;
                for (int t : selection_) if (t == i) { drop = true; break; }
                if (drop) { ++del; ctx.Log(L"  已删除会话：" + sess_[i].name); }
                else keep.push_back(sess_[i]);
            }
            SaveSessions(keep);
            sess_ = keep;
            out.summary = L"已删除 " + U32s3((unsigned)del) + L" 个会话";
            out.canClean = true;
            break;
        }
        case 2: {   // 保存/更新会话
            SessEntry e;
            e.name = TrimSp(fName_);
            e.proto = NormProto(fProto_);
            e.host = TrimSp(fHost_);
            e.port = TrimSp(fPort_);
            if (e.name.empty() || e.host.empty()) {
                out.summary = L"请先在上方填写「会话名称」与「主机」";
                out.canClean = true;
                return;
            }
            if (!e.port.empty()) {
                bool digits = true;
                for (wchar_t c : e.port) if (c < L'0' || c > L'9') { digits = false; break; }
                if (!digits || e.port.size() > 5) {
                    out.summary = L"端口必须为 0~65535 的数字";
                    out.canClean = true;
                    return;
                }
            }
            bool updated = false;
            for (SessEntry& old : sess_) {
                if (FoldLow3(old.name) == FoldLow3(e.name)) {
                    old = e;
                    updated = true;
                    break;
                }
            }
            if (!updated) sess_.push_back(e);
            SaveSessions(sess_);
            out.summary = (updated ? L"已更新会话：" : L"已添加会话：") + e.name +
                          L"（" + e.proto + L"）";
            out.canClean = true;
            break;
        }
        case 3: {   // 安装 OpenSSH 客户端
            wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
                          L"Add-WindowsCapability -Online -Name OpenSSH.Client~~~~0.0.1.0";
            ProcRunOptions opt;
            opt.timeoutMs = 600000;
            opt.cancel = ctx.cancel;
            opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  [powershell] " + line); };
            wstring full;
            int code = RunProcess(cmd, opt, &full);
            out.summary = (code == 0)
                ? wstring(L"OpenSSH 客户端安装完成")
                : wstring(L"安装失败：退出码 ") + U32s3((unsigned)code) +
                  L"（需要管理员权限；也可在 设置 → 应用 → 可选功能 手动安装）";
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<int> selection_;
    vector<SessEntry> sess_;
    wstring fName_, fProto_, fHost_, fPort_;
};

REGISTER_MODULE(NetSessions);

// ================================================================ net.tunnel

struct TunnelDef {
    wstring type;     // L / R / D
    wstring lport;    // 本地端口
    wstring target;   // 目标主机:目标端口（D 型可空）
    wstring jump;     // 跳板 user@host[:端口]
};

struct TunnelRun {
    int defIdx = -1;
    HANDLE hProcess = nullptr;
};

wstring TunnelsIni() { return ExeDir3() + L"\\netadmin-tunnels.ini"; }

vector<TunnelDef> LoadTunnels() {
    vector<TunnelDef> out;
    const wstring ini = TunnelsIni();
    int cnt = GetPrivateProfileIntW(L"tunnels", L"count", 0, ini.c_str());
    if (cnt < 0 || cnt > 200) cnt = 0;
    for (int i = 0; i < cnt; ++i) {
        wstring pre = L"t" + U32s3((unsigned)i) + L".";
        wchar_t buf[256] = {};
        TunnelDef d;
        auto rd = [&](const wchar_t* key, wstring* dst) {
            GetPrivateProfileStringW(L"tunnels", (pre + key).c_str(), L"",
                                     buf, 256, ini.c_str());
            *dst = buf;
        };
        rd(L"type", &d.type);
        rd(L"lport", &d.lport);
        rd(L"target", &d.target);
        rd(L"jump", &d.jump);
        if (!d.lport.empty()) out.push_back(std::move(d));
    }
    return out;
}

void SaveTunnels(const vector<TunnelDef>& list) {
    const wstring ini = TunnelsIni();
    WritePrivateProfileStringW(L"tunnels", nullptr, nullptr, ini.c_str());
    WritePrivateProfileStringW(L"tunnels", L"count", U32s3((unsigned)list.size()).c_str(),
                               ini.c_str());
    for (size_t i = 0; i < list.size(); ++i) {
        wstring pre = L"t" + U32s3((unsigned)i) + L".";
        WritePrivateProfileStringW(L"tunnels", (pre + L"type").c_str(), list[i].type.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"tunnels", (pre + L"lport").c_str(), list[i].lport.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"tunnels", (pre + L"target").c_str(), list[i].target.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"tunnels", (pre + L"jump").c_str(), list[i].jump.c_str(), ini.c_str());
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

// 规范化转发类型：L / R / D
wchar_t NormTType(const wstring& t) {
    wstring low = FoldLow3(TrimSp(t));
    if (!low.empty()) {
        wchar_t c = low[0];
        if (c == L'l' || c == L'r' || c == L'd') return (wchar_t)(c - 32);
        if (c == L'本') return L'L';
        if (c == L'远') return L'R';
        if (c == L'动' || c == L's') return L'D';
    }
    return L'L';
}

wstring TypeName(wchar_t t) {
    switch (t) {
    case L'L': return L"本地转发 -L";
    case L'R': return L"远程转发 -R";
    case L'D': return L"动态 SOCKS -D";
    default:   return L"-L";
    }
}

class NetTunnel final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.tunnel"; }
    const wchar_t* Group() const override { return L"远程终端"; }
    const wchar_t* Title() const override { return L"端口转发隧道"; }
    const wchar_t* Description() const override {
        return L"基于 Windows 内置 OpenSSH 的端口转发：-L 本地 / -R 远程 /"
               L" -D 动态 SOCKS 三型，生成 ssh -N 命令并启动（记录进程句柄）；"
               L"启动/停止/停止全部；隧道定义持久化（exe 同目录 ini），"
               L"活动状态实时展示";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return false; }
    const wchar_t* RunLabel() const override { return L"刷新隧道列表"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"转发类型",  L"L 本地 / R 远程 / D 动态(SOCKS)，默认 L", false },
            { L"本地端口",  L"如 8080", false },
            { L"目标",      L"目标主机:目标端口，如 192.168.1.10:80（D 型留空）", false },
            { L"跳板登录",  L"user@host[:端口]，如 admin@192.168.1.5 或 admin@j.com:2222", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fType_ = values[0];
        if (values.size() > 1) fLport_ = values[1];
        if (values.size() > 2) fTarget_ = values[2];
        if (values.size() > 3) fJump_ = values[3];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"启动选中隧道", nullptr, false, 0xE768 },  // 0 → btnClean 位
            { L"停止选中隧道", nullptr, false, 0xE71A },  // 1 → btnUndo 位
            { L"保存为隧道",   nullptr, false, 0 },       // 2 → 更多 ▾
            { L"删除选中定义", nullptr, true,  0 },       // 3 → 更多 ▾
            { L"停止全部",     nullptr, false, 0 },       // 4 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        switch (actionIndex) {
        case 0: {
            if (tags.empty()) return L"未选中任何隧道定义。";
            wstring names;
            for (int t : tags)
                if (t >= 0 && t < (int)defs_.size())
                    names += DefSummary(defs_[t]) + L"\n";
            return L"将启动以下隧道（各开一个 ssh 新控制台窗口，关闭窗口=停止隧道）：\n" +
                   names + L"\n是否继续？";
        }
        case 1: {
            if (tags.empty()) return L"未选中任何隧道。";
            wstring names;
            for (int t : tags)
                if (t >= 0 && t < (int)defs_.size()) names += DefSummary(defs_[t]) + L"\n";
            return L"将终止以下隧道的 ssh 进程：\n" + names + L"\n是否继续？";
        }
        case 2: {
            wchar_t ty = NormTType(fType_);
            if (TrimSp(fLport_).empty() || (ty != L'D' && TrimSp(fTarget_).empty()))
                return L"请先在上方填写「本地端口」与「目标」（D 型目标可留空）。";
            return L"将保存隧道定义：" + DefSummary(FieldsToDef()) + L"\n是否继续？";
        }
        case 3: {
            if (tags.empty()) return L"未选中任何隧道定义。";
            return L"将删除 " + U32s3((unsigned)tags.size()) + L" 条隧道定义（运行中的进程不受影响）。\n是否继续？";
        }
        case 4: {
            int n = RunningCount();
            if (n == 0) return L"当前没有运行中的隧道。";
            return L"将终止全部 " + U32s3((unsigned)n) + L" 条运行中的隧道。\n是否继续？";
        }
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        defs_ = LoadTunnels();
        ReapExited();
        bool sshOk = FileExists3(SshDir() + L"\\ssh.exe");
        for (size_t i = 0; i < defs_.size(); ++i) {
            ModuleRow r;
            r.c1 = L"本地 " + defs_[i].lport;
            r.c2 = TypeName(NormTType(defs_[i].type));
            r.c3 = NormTType(defs_[i].type) == L'D' ? wstring(L"(SOCKS 代理)") : defs_[i].target;
            r.c4 = L"经 " + (defs_[i].jump.empty() ? wstring(L"(未填跳板)") : defs_[i].jump) +
                   L" · " + RunStateText((int)i);
            r.tag = (int)i;
            r.danger = IsRunning((int)i);
            out.rows.push_back(std::move(r));
        }
        out.summary = L"隧道定义 " + U32s3((unsigned)defs_.size()) + L" 条 · 运行中 " +
                      U32s3((unsigned)RunningCount()) + L" · OpenSSH " +
                      (sshOk ? wstring(L"可用") : wstring(L"缺失"));
        out.hint = sshOk
            ? L"在上方填写参数后「更多 ▾ → 保存为隧道」，再勾选「启动选中隧道」。"
              L"隧道在本程序运行期间持有句柄；退出程序前请先停止隧道。"
            : L"未检测到 Windows 内置 OpenSSH：请先在「远程会话」模块安装。";
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 启动选中
            if (selection_.empty()) {
                out.summary = L"未选中任何隧道定义";
                out.canClean = true;
                return;
            }
            int ok = 0, fail = 0;
            for (int t : selection_) {
                if (t < 0 || t >= (int)defs_.size()) continue;
                if (IsRunning(t)) {
                    ctx.Log(L"  已在运行，跳过：" + DefSummary(defs_[t]));
                    continue;
                }
                wstring err;
                if (StartTunnel(defs_[t], t, &err)) {
                    ++ok;
                    ctx.Log(L"  已启动：" + DefSummary(defs_[t]));
                } else {
                    ++fail;
                    ctx.Log(L"  启动失败：" + DefSummary(defs_[t]) + L" — " + err);
                }
            }
            out.summary = L"启动完成：成功 " + U32s3((unsigned)ok) + L"，失败 " +
                          U32s3((unsigned)fail);
            out.canClean = true;
            break;
        }
        case 1: {   // 停止选中
            int ok = 0;
            for (int t : selection_) {
                for (size_t i = 0; i < runs_.size(); ++i) {
                    if (runs_[i].defIdx == t && runs_[i].hProcess) {
                        TerminateProcess(runs_[i].hProcess, 1);
                        CloseHandle(runs_[i].hProcess);
                        runs_.erase(runs_.begin() + i);
                        ++ok;
                        ctx.Log(L"  已停止：本地 " + (t >= 0 && t < (int)defs_.size()
                                                     ? defs_[t].lport : wstring(L"?")));
                        break;
                    }
                }
            }
            out.summary = ok ? L"已停止 " + U32s3((unsigned)ok) + L" 条隧道"
                             : L"选中的隧道没有运行中的进程";
            out.canClean = true;
            break;
        }
        case 2: {   // 保存为隧道
            TunnelDef d = FieldsToDef();
            wchar_t ty = NormTType(fType_);
            if (TrimSp(fLport_).empty() || (ty != L'D' && TrimSp(fTarget_).empty())) {
                out.summary = L"请先在上方填写「本地端口」与「目标」（D 型目标可留空）";
                out.canClean = true;
                return;
            }
            bool updated = false;
            for (TunnelDef& old : defs_) {
                if (NormTType(old.type) == ty && FoldLow3(old.lport) == FoldLow3(d.lport) &&
                    FoldLow3(old.target) == FoldLow3(d.target) &&
                    FoldLow3(old.jump) == FoldLow3(d.jump)) {
                    updated = true;
                    break;
                }
            }
            if (!updated) defs_.push_back(d);
            SaveTunnels(defs_);
            out.summary = (updated ? L"该隧道定义已存在" : L"已保存隧道定义：") +
                          DefSummary(d);
            out.canClean = true;
            break;
        }
        case 3: {   // 删除选中定义
            if (selection_.empty()) {
                out.summary = L"未选中任何隧道定义";
                out.canClean = true;
                return;
            }
            vector<TunnelDef> keep;
            int del = 0;
            for (int i = 0; i < (int)defs_.size(); ++i) {
                bool drop = false;
                for (int t : selection_) if (t == i) { drop = true; break; }
                if (drop) { ++del; ctx.Log(L"  已删除定义：" + DefSummary(defs_[i])); }
                else keep.push_back(defs_[i]);
            }
            SaveTunnels(keep);
            defs_ = keep;
            out.summary = L"已删除 " + U32s3((unsigned)del) + L" 条定义";
            out.canClean = true;
            break;
        }
        case 4: {   // 停止全部
            int n = 0;
            for (TunnelRun& r : runs_) {
                if (r.hProcess) { TerminateProcess(r.hProcess, 1); CloseHandle(r.hProcess); ++n; }
            }
            runs_.clear();
            out.summary = L"已停止全部隧道：" + U32s3((unsigned)n) + L" 条";
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<int> selection_;
    vector<TunnelDef> defs_;
    mutable vector<TunnelRun> runs_;   // 本会话启动的隧道进程句柄（回收需可变）
    wstring fType_, fLport_, fTarget_, fJump_;

    TunnelDef FieldsToDef() const {
        TunnelDef d;
        d.type = wstring(1, NormTType(fType_));
        d.lport = TrimSp(fLport_);
        d.target = TrimSp(fTarget_);
        d.jump = TrimSp(fJump_);
        return d;
    }

    wstring DefSummary(const TunnelDef& d) const {
        wchar_t ty = NormTType(d.type);
        wstring s = L"本地:" + d.lport + L" " + TypeName(ty);
        if (ty != L'D' && !d.target.empty()) s += L" → " + d.target;
        if (!d.jump.empty()) s += L"（经 " + d.jump + L"）";
        return s;
    }

    bool IsRunning(int defIdx) const {
        ReapExited();
        for (const TunnelRun& r : runs_) if (r.defIdx == defIdx) return true;
        return false;
    }

    int RunningCount() const {
        ReapExited();
        return (int)runs_.size();
    }

    // 回收已退出的隧道进程（状态如实展示，不伪造）
    void ReapExited() const {
        for (size_t i = 0; i < runs_.size();) {
            DWORD code = 0;
            if (runs_[i].hProcess &&
                GetExitCodeProcess(runs_[i].hProcess, &code) &&
                code != STILL_ACTIVE) {
                CloseHandle(runs_[i].hProcess);
                runs_.erase(runs_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    wstring RunStateText(int defIdx) {
        return IsRunning(defIdx) ? wstring(L"运行中") : wstring(L"未运行");
    }

    // 生成并启动 ssh -N 隧道：
    //   L: ssh -N -L lport:target  jump
    //   R: ssh -N -R lport:target  jump
    //   D: ssh -N -D lport         jump
    // jump 支持 user@host 与 user@host:port（→ -p 端口）
    bool StartTunnel(const TunnelDef& d, int defIdx, wstring* err) {
        wstring exe = SshDir() + L"\\ssh.exe";
        if (!FileExists3(exe)) {
            if (err) *err = L"未找到 Windows 内置 OpenSSH（" + exe + L"）";
            return false;
        }
        wchar_t ty = NormTType(d.type);
        if (d.lport.empty()) { if (err) *err = L"本地端口为空"; return false; }
        if (ty != L'D' && d.target.empty()) { if (err) *err = L"目标为空"; return false; }
        wstring jump = TrimSp(d.jump);
        if (jump.empty()) { if (err) *err = L"跳板登录为空（需要 SSH 服务器）"; return false; }

        // 拆跳板 user@host[:port]
        wstring hostPart = jump;
        wstring portArg;
        size_t at = jump.find(L'@');
        size_t colon = (at == wstring::npos) ? jump.find(L':') : jump.find(L':', at);
        if (colon != wstring::npos) {
            hostPart = jump.substr(0, colon);
            wstring p = TrimSp(jump.substr(colon + 1));
            bool digits = !p.empty();
            for (wchar_t c : p) if (c < L'0' || c > L'9') { digits = false; break; }
            if (digits) portArg = L" -p " + p;
        }

        wstring fwd;
        if (ty == L'D') {
            fwd = L"-D " + d.lport;
        } else {
            wchar_t flag = (ty == L'R') ? L'R' : L'L';
            fwd = wstring(L"-") + flag + L" " + d.lport + L":" + d.target;
        }
        wstring args = L"-N " + fwd + portArg + L" " + hostPart +
                       L" -o ServerAliveInterval=30";

        PROCESS_INFORMATION pi{};
        if (!LaunchNewConsole(exe, args, err, &pi)) return false;
        TunnelRun r;
        r.defIdx = defIdx;
        r.hProcess = pi.hProcess;
        runs_.push_back(r);
        return true;
    }
};

REGISTER_MODULE(NetTunnel);

// ================================================================ net.serial

// DCB 停止位常量（winspool 无关；winbase.h 已含）
class NetSerial final : public IModule {
public:
    const wchar_t* Id() const override { return L"net.serial"; }
    const wchar_t* Group() const override { return L"远程终端"; }
    const wchar_t* Title() const override { return L"串口终端"; }
    const wchar_t* Description() const override {
        return L"枚举本机 COM 口（QueryDosDevice）；按波特率/数据位/停止位配置 DCB；"
               L"打开串口监视（接收实时上屏，Esc 停止）与发送一行并读取响应。"
               L"图形化交互终端（方向键/历史回看）暂不支持";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    const wchar_t* RunLabel() const override { return L"扫描串口"; }
    bool NeedsTarget() const override { return true; }   // 「发送一行」的发送内容
    const wchar_t* TargetHint() const override { return L"要发送的文本（配合「发送并读取响应」）"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"COM 口",   L"如 COM3（留空 = 第一个找到的口）", false },
            { L"波特率",   L"默认 115200（常用：9600/19200/38400/57600/115200）", false },
            { L"数据位",   L"默认 8", false },
            { L"停止位",   L"1 或 2，默认 1", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fCom_ = values[0];
        if (values.size() > 1) fBaud_ = values[1];
        if (values.size() > 2) fData_ = values[2];
        if (values.size() > 3) fStop_ = values[3];
    }
    void SetInput(const wstring& path, const wstring& target) override {
        (void)path;
        fSend_ = target;
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"打开串口监视",   nullptr, false, 0xE7F4 },  // 0 → btnClean 位
            { L"发送并读取响应", nullptr, false, 0xE724 },  // 1 → btnUndo 位
        };
    }

    wstring ActionWarning(int actionIndex, const vector<int>&) const override {
        switch (actionIndex) {
        case 0:
            return L"将打开串口并持续显示接收数据（按 Esc 或「停止」结束监视）。\n"
                   L"端口被其他程序（串口调试助手等）占用时会打开失败。\n\n是否继续？";
        case 1:
            return L"将按上方配置打开串口，发送目标框中的文本（自动补 \\r\\n），"
                   L"并读取 3 秒内返回的数据。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        vector<wstring> ports = EnumComPorts();
        for (size_t i = 0; i < ports.size(); ++i) {
            ModuleRow r;
            r.c1 = ports[i];
            r.c2 = ConfigSummary(ports[i]);
            r.c3 = L"串口";
            r.c4 = L"\\\\.\\" + ports[i];
            r.tag = (int)i;
            out.rows.push_back(std::move(r));
        }
        out.summary = ports.empty()
            ? wstring(L"未发现串口（无 COM 设备或驱动未装）")
            : L"发现串口 " + U32s3((unsigned)ports.size()) + L" 个";
        out.hint = L"在上方填写 COM 口与波特率后：「打开串口监视」实时查看接收；"
                   L"目标框填发送内容 →「发送并读取响应」。";
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: Monitor(ctx, out); break;
        case 1: SendAndRead(ctx, out); break;
        default: break;
        }
    }

private:
    wstring fCom_, fBaud_, fData_, fStop_, fSend_;

    vector<wstring> EnumComPorts() const {
        vector<wstring> out;
        DWORD sz = QueryDosDeviceW(nullptr, nullptr, 0);
        if (sz == 0) return out;
        vector<wchar_t> buf(sz);
        if (QueryDosDeviceW(nullptr, buf.data(), sz) == 0) return out;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
            // 形如 COM3（COM 后全数字才收，排除 COMDLG32 等误命中）
            if (wcsncmp(p, L"COM", 3) != 0) continue;
            const wchar_t* q = p + 3;
            if (!*q) continue;
            bool digits = true;
            for (const wchar_t* r = q; *r; ++r) if (*r < L'0' || *r > L'9') { digits = false; break; }
            if (digits) out.push_back(p);
        }
        std::sort(out.begin(), out.end(), [](const wstring& a, const wstring& b) {
            return wcstoul(a.c_str() + 3, nullptr, 10) < wcstoul(b.c_str() + 3, nullptr, 10);
        });
        return out;
    }

    wstring ConfigSummary(const wstring& port) const {
        if (!fCom_.empty() && FoldLow3(TrimSp(fCom_)) != FoldLow3(port))
            return wstring(L"（配置针对 " + TrimSp(fCom_) + L"）");
        return L"波特率 " + (TrimSp(fBaud_).empty() ? wstring(L"115200") : TrimSp(fBaud_)) +
               L" · " + (TrimSp(fData_).empty() ? wstring(L"8") : TrimSp(fData_)) +
               L" 数据位 · " + (TrimSp(fStop_).empty() ? wstring(L"1") : TrimSp(fStop_)) +
               L" 停止位 · 无校验";
    }

    // 解析字段 → 配置值；返回 false 时 err 带原因
    bool ParseConfig(wstring* com, DWORD* baud, BYTE* dataBits, BYTE* stopBits, wstring* err) const {
        *com = TrimSp(fCom_);
        if (com->empty()) {
            vector<wstring> ports = EnumComPorts();
            if (ports.empty()) { if (err) *err = L"未发现串口（COM 口为空且本机无 COM 设备）"; return false; }
            *com = ports[0];
        }
        unsigned b = 115200;
        if (!TrimSp(fBaud_).empty()) b = (unsigned)wcstoul(TrimSp(fBaud_).c_str(), nullptr, 10);
        if (b < 110 || b > 4000000) { if (err) *err = L"波特率超出范围（110~4000000）"; return false; }
        *baud = b;
        unsigned d = 8;
        if (!TrimSp(fData_).empty()) d = (unsigned)wcstoul(TrimSp(fData_).c_str(), nullptr, 10);
        if (d < 5 || d > 8) { if (err) *err = L"数据位应为 5~8"; return false; }
        *dataBits = (BYTE)d;
        unsigned s = 1;
        if (!TrimSp(fStop_).empty()) s = (unsigned)wcstoul(TrimSp(fStop_).c_str(), nullptr, 10);
        if (s != 1 && s != 2) { if (err) *err = L"停止位应为 1 或 2"; return false; }
        *stopBits = (s == 2) ? TWOSTOPBITS : ONESTOPBIT;
        return true;
    }

    // 打开并配置 COM 口；失败返回 INVALID_HANDLE_VALUE 且 err 带中文归因
    HANDLE OpenCom(const wstring& com, DWORD baud, BYTE dataBits, BYTE stopBits, wstring* err) {
        wstring dev = L"\\\\.\\" + com;
        HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (err) {
                *err = L"打开 " + com + L" 失败（" + ErrorTextZh(e) + L"）";
                if (e == ERROR_ACCESS_DENIED) *err += L"——端口被其他程序占用";
                if (e == ERROR_FILE_NOT_FOUND) *err += L"——端口不存在";
            }
            return h;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) {
            if (err) *err = L"读取 DCB 失败（" + ErrorTextZh(GetLastError()) + L"）";
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        dcb.BaudRate = baud;
        dcb.ByteSize = dataBits;
        dcb.StopBits = stopBits;
        dcb.Parity = NOPARITY;
        dcb.fBinary = TRUE;
        if (!SetCommState(h, &dcb)) {
            if (err) *err = L"配置 DCB 失败（" + ErrorTextZh(GetLastError()) +
                            L"）——波特率可能不被设备支持";
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;       // 有数据即返回（配合轮询）
        to.ReadTotalTimeoutConstant = 0;
        to.ReadTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = 2000;
        to.WriteTotalTimeoutMultiplier = 0;
        if (!SetCommTimeouts(h, &to)) {
            if (err) *err = L"设置超时失败（" + ErrorTextZh(GetLastError()) + L"）";
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        return h;
    }

    // 收到的字节 → 可显示行（可打印字符直出，其余 {XX} 十六进制）
    wstring BytesToText(const unsigned char* p, size_t n) {
        static const wchar_t* kHex = L"0123456789ABCDEF";
        wstring out;
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = p[i];
            if (c == '\r') continue;
            if (c == '\n') { out += L"⏎"; continue; }
            if (c >= 0x20 && c < 0x7F) out += (wchar_t)c;
            else {
                out += L"{";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
                out += L"}";
            }
        }
        return out;
    }

    void Monitor(ModuleContext& ctx, ModuleResult& out) {
        wstring com, err;
        DWORD baud; BYTE dataBits, stopBits;
        if (!ParseConfig(&com, &baud, &dataBits, &stopBits, &err)) {
            out.summary = err;
            out.canClean = true;
            return;
        }
        HANDLE h = OpenCom(com, baud, dataBits, stopBits, &err);
        if (h == INVALID_HANDLE_VALUE) {
            out.summary = err;
            out.canClean = true;
            return;
        }
        ctx.Log(L"串口监视已打开：" + com + L" @ " + U32s3((unsigned)baud) +
                L"（按 Esc 或「停止」结束）");
        unsigned char buf[512];
        wstring line;
        unsigned long long total = 0;
        while (!ctx.Cancelled()) {
            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) {
                Sleep(30);
                continue;
            }
            total += got;
            line += BytesToText(buf, got);
            // 按 ⏎ 断行输出，避免日志面板行无限增长
            for (size_t pos; (pos = line.find(L"⏎")) != wstring::npos; ) {
                ctx.Log(L"  << " + line.substr(0, pos));
                line = line.substr(pos + 1);
            }
            if (line.size() > 200) {              // 无换行的裸流：200 字符截断输出
                ctx.Log(L"  << " + line);
                line.clear();
            }
        }
        if (!line.empty()) ctx.Log(L"  << " + line);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(h);
        wchar_t b[128];
        swprintf_s(b, 128, L"串口监视结束：%s 共接收 %llu 字节", com.c_str(), total);
        out.summary = b;
        out.canClean = true;
    }

    void SendAndRead(ModuleContext& ctx, ModuleResult& out) {
        wstring com, err;
        DWORD baud; BYTE dataBits, stopBits;
        if (!ParseConfig(&com, &baud, &dataBits, &stopBits, &err)) {
            out.summary = err;
            out.canClean = true;
            return;
        }
        wstring text = fSend_;
        if (TrimSp(text).empty()) {
            out.summary = L"请先在目标框填写要发送的文本";
            out.canClean = true;
            return;
        }
        HANDLE h = OpenCom(com, baud, dataBits, stopBits, &err);
        if (h == INVALID_HANDLE_VALUE) {
            out.summary = err;
            out.canClean = true;
            return;
        }
        std::string u8 = WToUtf8(text + L"\r\n");
        DWORD w = 0;
        if (!WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr)) {
            out.summary = L"发送失败（" + ErrorTextZh(GetLastError()) + L"）";
            CloseHandle(h);
            out.canClean = true;
            return;
        }
        ctx.Log(L"  >> 已发送 " + U32s3((unsigned)w) + L" 字节：" + text);
        // 读 3 秒响应
        unsigned char buf[512];
        wstring line;
        unsigned long long total = 0;
        ULONGLONG deadline = GetTickCount64() + 3000;
        while (GetTickCount64() < deadline && !ctx.Cancelled()) {
            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) {
                Sleep(30);
                continue;
            }
            total += got;
            line += BytesToText(buf, got);
            for (size_t pos; (pos = line.find(L"⏎")) != wstring::npos; ) {
                ctx.Log(L"  << " + line.substr(0, pos));
                line = line.substr(pos + 1);
            }
        }
        if (!line.empty()) ctx.Log(L"  << " + line);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        CloseHandle(h);
        out.summary = total
            ? wstring(L"发送完成并收到响应：") + com + L" 共 " + U32s3((unsigned)total) + L" 字节（见日志）"
            : L"发送完成：" + com + L"（3 秒内无响应）";
        out.canClean = true;
    }
};

REGISTER_MODULE(NetSerial);

} // namespace
} // namespace mz
