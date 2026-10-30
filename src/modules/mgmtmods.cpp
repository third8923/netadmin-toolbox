// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 常用管理模块集（v2.8 T05/R5）
//  ---------------------------------------------------------------------------
//  六个模块，Group 均为「常用管理」：
//    * sys.restore  系统还原点：Checkpoint-Computer 经 procrun 流式创建；
//                   SR 禁用 / 24h 频率限制失败归因；NeedsPath 承载描述
//    * sys.users    本地用户：NetUserEnum level 3 枚举，NetUserSetInfo
//                   level 1008 启用/禁用（保留其余 flags），内置账户拦截
//    * sys.shares   共享目录：NetShareEnum level 2 枚举，NetShareDel 删除；
//                   IPC$/ADMIN$/X$ 管理共享只读拒绝
//    * sys.events   事件日志：EvtQuery System+Application Level<=2 近 7 天，
//                   按 来源+事件ID 聚合 Top 20（Security 通道需审计权限，跳过）
//    * sys.power    电源计划：PowerEnumerate/PowerReadFriendlyName 枚举，
//                   PowerSetActiveScheme 切换，powercfg duplicatescheme 卓越性能
//                   （MinGW powrprof.h 缺 Get/SetActiveScheme 声明 → 动态加载）
//    * sys.update   缺失更新：WUAPI COM IUpdateSession::Search，只查不装
//  ModuleRow.tag = 模块私有下标（排序安全回溯），与 sys.tasks 同一套路。
//  链接：-lnetapi32 -lwevtapi -lpowrprof（build.bat）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"   // ErrorTextZh（Win32 错误码中文归因）
#include "../procrun.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <map>
#include <utility>

#include <objbase.h>      // StringFromGUID2 / CoCreateInstance
#include <oleauto.h>      // VarR8FromDec
#include <lm.h>           // NetUserEnum / NetShareEnum（NetApiBufferFree）
#include <winevt.h>       // EvtQuery / EvtRender
#include <powrprof.h>     // PowerEnumerate / PowerReadFriendlyName

#include <initguid.h>     // 必须在 wuapi.h 之前：实例化 CLSID/IID 免链接 uuid 库
#include <wuapi.h>        // IUpdateSession / ISearchResult

namespace mz {
namespace {

// ---------------------------------------------------------------- 小工具

// 数量 → 宽字符串（摘要拼接用）
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

// 大小写不敏感包含
bool ContainsI(const wstring& hay, const wstring& needle) {
    if (needle.empty()) return true;
    wstring h = hay, n = needle;
    for (auto& c : h) c = (wchar_t)towlower(c);
    for (auto& c : n) c = (wchar_t)towlower(c);
    return h.find(n) != wstring::npos;
}

// BSTR 拷贝为 wstring 并释放
wstring TakeBstr(BSTR b) {
    wstring out = b ? wstring(b, SysStringLen(b)) : wstring();
    SysFreeString(b);
    return out;
}

// HRESULT → 中文（Win32 facility 展开为系统错误码归因）
wstring HrTextZh(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return ErrorTextZh(HRESULT_CODE(hr));
    return ErrorTextZh((DWORD)hr);
}

// LANMAN 错误码 → 中文（NERR 段 FormatMessage 覆盖不全，常见码本地归因）
wstring NerrTextZh(DWORD st) {
    switch (st) {
    case NERR_Success:         return L"成功";
    case ERROR_ACCESS_DENIED:  return ErrorTextZh(st);
    case 2221: /*NERR_UserNotFound*/   return L"用户不存在（可能已被删除）";
    case 2250: /*NERR_NetNameNotFound*/ return L"共享名不存在（可能已被删除）";
    case 2114: /*NERR_ServerNotStarted*/ return L"Server 服务未运行，无法操作用户/共享";
    default: return ErrorTextZh(st);
    }
}

// GUID → 无花括号字符串（如 381b4222-f694-...）
wstring GuidToStr(const GUID& g) {
    wchar_t b[64] = {};
    StringFromGUID2(g, b, 64);
    wstring s = b;
    if (!s.empty() && s.front() == L'{') s = s.substr(1, s.size() - 2);
    return s;
}

// FILETIME → 本地时间 "YYYY-MM-DD HH:MM"
wstring FormatFileTime(const FILETIME& ftUtc) {
    FILETIME ftLocal = {};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal)) return L"-";
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&ftLocal, &st)) return L"-";
    wchar_t b[40] = {};
    swprintf_s(b, L"%04u-%02u-%02u %02u:%02u",
               (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
               (unsigned)st.wHour, (unsigned)st.wMinute);
    return b;
}

// "YYYY-MM-DD HH:MM" → UTC FILETIME（本机时区，仅供 24h 判断）；失败返回 false
bool ParseLocalDateTime(const wstring& s, FILETIME* ftUtc) {
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (swscanf_s(s.c_str(), L"%4u-%2u-%2u %2u:%2u", &y, &mo, &d, &h, &mi) != 5)
        return false;
    SYSTEMTIME st = {};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi;
    FILETIME ftLocal = {};
    if (!SystemTimeToFileTime(&st, &ftLocal)) return false;
    return LocalFileTimeToFileTime(&ftLocal, ftUtc);
}

// unix 秒（UTC）→ "YYYY-MM-DD HH:MM"；0 → 从不
wstring FormatUnixTime(unsigned long long secs) {
    if (secs == 0) return L"从不";
    time_t t = (time_t)secs;
    struct tm lt = {};
    if (_localtime64_s(&lt, &t) != 0) return L"-";
    wchar_t b[40] = {};
    swprintf_s(b, L"%04d-%02d-%02d %02d:%02d",
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return b;
}

// 修剪首尾空白
wstring Trim(const wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == wstring::npos) return wstring();
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ------------------------------------------------ 1) sys.restore 系统还原点

// 还原点查询结果（PowerShell Get-ComputerRestorePoint）
struct RpQueryResult {
    bool ok = false;              // 命令执行并解析成功
    int count = 0;                // 还原点总数
    wstring latest;               // 最近一个的时间（"YYYY-MM-DD HH:MM"）
    wstring err;                  // PowerShell 异常消息（含 SR 禁用信息）
    bool srDisabled = false;      // 系统还原被禁用
    vector<ModuleRow> rows;       // 最近 8 个还原点（供列表展示）
};

// 调 PowerShell 查询还原点（同时作为 SR 可用性探测）
RpQueryResult QueryRestorePoints(ModuleContext& ctx) {
    const wchar_t* ps =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"$ErrorActionPreference='Stop'; try { "
        L"$p=@(Get-ComputerRestorePoint -ErrorAction Stop); "
        L"Write-Output ('COUNT|'+$p.Count); "
        L"foreach($r in ($p | Sort-Object SequenceNumber | Select-Object -Last 8)) { "
        L"Write-Output ('RP|'+$r.SequenceNumber+'|'"
        L"+[System.Management.ManagementDateTimeConverter]::ToDateTime($r.CreationTime)"
        L".ToString('yyyy-MM-dd HH:mm')+'|'+$r.Description) } } "
        L"catch { Write-Output ('ERR|'+$_.Exception.Message) }\"";

    ProcRunOptions opt;
    opt.timeoutMs = 120000;
    opt.cancel = [&ctx]() { return ctx.Cancelled(); };
    opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  " + line); };
    wstring full;
    int code = RunProcess(ps, opt, &full);

    RpQueryResult r;
    if (code == -3) { r.err = L"已取消"; return r; }
    if (code == -2 || code == -1) { r.err = L"PowerShell 执行失败（退出码 " + I64(code) + L"）"; return r; }

    size_t pos = 0;
    while (pos <= full.size()) {
        size_t nl = full.find(L'\n', pos);
        wstring line = Trim(nl == wstring::npos ? full.substr(pos)
                                                : full.substr(pos, nl - pos));
        pos = (nl == wstring::npos) ? full.size() + 1 : nl + 1;
        if (line.rfind(L"COUNT|", 0) == 0) {
            r.count = (int)wcstol(line.c_str() + 6, nullptr, 10);
            r.ok = true;
        } else if (line.rfind(L"RP|", 0) == 0) {
            // RP|序号|时间|描述
            wstring body = line.substr(3);
            size_t p1 = body.find(L'|');
            size_t p2 = (p1 == wstring::npos) ? wstring::npos : body.find(L'|', p1 + 1);
            if (p1 != wstring::npos && p2 != wstring::npos) {
                wstring seq = body.substr(0, p1);
                ModuleRow row;
                row.c1 = L"#" + seq;
                row.c2 = body.substr(p1 + 1, p2 - p1 - 1);
                row.c3 = body.substr(p2 + 1);
                row.c4 = L"系统还原点";
                row.sortKey = wcstoul(seq.c_str(), nullptr, 10);
                r.rows.push_back(row);
            }
        } else if (line.rfind(L"ERR|", 0) == 0) {
            r.err = line.substr(4);
            if (ContainsI(r.err, L"disabled") || ContainsI(r.err, L"禁用"))
                r.srDisabled = true;
        }
    }
    // 顺序输出（SequenceNumber 升序）→ 最后一条最近
    if (!r.rows.empty()) r.latest = r.rows.back().c2;
    return r;
}

class SysRestoreModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.restore"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"系统还原点"; }
    const wchar_t* Description() const override { return L"创建系统还原点（自动归因 SR 禁用 / 24h 频率限制）"; }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsPath() const override { return true; }
    const wchar_t* PathHint() const override { return L"还原点描述（留空使用默认）"; }
    const wchar_t* RunLabel() const override { return L"创建还原点"; }
    bool NeedsElevation() const override { return true; }

    void SetInput(const wstring& path, const wstring& target) override {
        (void)target;
        desc_ = path;
    }

    // 主按钮执行：先列出现有还原点（兼 SR 可用性探测），再创建
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};

        // ---- 第一步：查询现有还原点 ----
        ctx.Report(5, L"正在查询现有还原点...");
        RpQueryResult pre = QueryRestorePoints(ctx);
        for (auto& r : pre.rows) {
            r.danger = false;
            out.rows.push_back(r);
        }
        if (!pre.ok && pre.err.empty())
            pre.err = L"查询命令无有效输出";
        if (pre.ok) {
            ctx.Log(L"  现有还原点共 " + I64(pre.count) + L" 个（下方列出最近 8 个）");
        } else {
            ctx.Log(L"  查询失败: " + pre.err);
        }

        // ---- 第二步：创建（非提权一律拒绝）----
        if (!IsElevated()) {
            out.summary = L"创建还原点需要管理员权限，未执行";
            out.hint = T(L"已尽力列出可见的系统还原点；请以管理员身份运行本程序后再创建。");
            out.canClean = true;
            ctx.Log(L"  拒绝: 当前非管理员，创建还原点需要管理员权限");
            return;
        }
        if (pre.srDisabled) {
            out.summary = L"系统还原已被禁用，无法创建还原点";
            out.hint = T(L"启用方法：系统属性 → 系统保护 → 对 C 盘「配置」→「启用系统保护」。");
            out.canClean = true;
            ctx.Log(L"  归因: 系统还原服务被禁用（" + pre.err + L"）");
            return;
        }
        if (!pre.ok) {
            out.summary = L"创建前探测失败，为安全起见未执行";
            out.hint = T(L"请确认 System Restore 服务（VSS）正在运行后重试。");
            out.canClean = true;
            return;
        }

        ctx.Report(30, L"正在创建还原点（通常 10~60 秒）...");
        wstring desc = desc_.empty() ? wstring(L"NetAdmin Toolbox 手动还原点") : desc_;
        // 清理描述：控制字符去掉、单引号翻倍（PowerShell 单引号串转义）
        wstring esc;
        for (wchar_t c : desc) {
            if (c < 0x20) continue;
            if (c == L'\'') esc += L"''"; else esc += c;
        }
        if (esc.size() > 200) esc = esc.substr(0, 200);
        wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
                      L"\"Checkpoint-Computer -Description '" + esc +
                      L"' -RestorePointType 'MODIFY_SETTINGS'\"";
        ctx.Log(L"  $ " + cmd);
        ProcRunOptions opt;
        opt.timeoutMs = 600000;
        opt.cancel = [&ctx]() { return ctx.Cancelled(); };
        opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  " + line); };
        wstring full;
        int code = RunProcess(cmd, opt, &full);
        if (code == -3) {
            out.summary = L"已取消，还原点创建中止";
            out.canClean = true;
            return;
        }
        if (code != 0) {
            out.summary = L"Checkpoint-Computer 退出码 " + I64(code) + L"（详见上方日志）";
            out.canClean = true;
            return;
        }

        // ---- 第三步：复核数量（Checkpoint 遇 24h 频率限制会静默跳过）----
        ctx.Report(80, L"正在复核创建结果...");
        RpQueryResult post = QueryRestorePoints(ctx);
        if (post.ok && post.count > pre.count) {
            out.summary = L"还原点创建成功（现有 " + I64(post.count) + L" 个，已列出最近 8 个）";
            out.rows = post.rows;
            ctx.Report(100, L"还原点创建完成");
        } else {
            bool within24h = false;
            FILETIME ftLatest = {}, ftNow = {};
            GetSystemTimeAsFileTime(&ftNow);
            if (!pre.latest.empty() && ParseLocalDateTime(pre.latest, &ftLatest)) {
                ULARGE_INTEGER u1, u2;
                u1.LowPart = ftLatest.dwLowDateTime; u1.HighPart = ftLatest.dwHighDateTime;
                u2.LowPart = ftNow.dwLowDateTime; u2.HighPart = ftNow.dwHighDateTime;
                within24h = (u2.QuadPart > u1.QuadPart) &&
                            (u2.QuadPart - u1.QuadPart) < (24ULL * 3600ULL * 10000000ULL);
            }
            if (within24h) {
                out.summary = L"未创建新还原点：24 小时内已存在还原点（" + pre.latest + L"）";
                out.hint = wstring(T(L"Windows 默认限制 24 小时内只允许创建一个还原点；"))
                           + wstring(T(L"可设注册表 HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore "))
                           + T(L"下 SystemRestorePointCreationFrequency = 0 (DWORD) 解除限制。");
                ctx.Log(L"  归因: 24 小时频率限制（最近还原点 " + pre.latest + L"）");
            } else {
                out.summary = L"创建命令已执行，但未检测到新还原点（详见上方日志）";
                out.hint = T(L"请确认系统保护已对系统盘启用（系统属性 → 系统保护）。");
            }
            out.rows = post.ok ? post.rows : out.rows;
        }
        out.canClean = true;
    }

private:
    wstring desc_;   // NeedsPath 输入的还原点描述
};

// ------------------------------------------------ 2) sys.users 本地用户

// 内置账户（不允许启/禁操作）
bool IsBuiltinAccount(const wstring& name) {
    return _wcsicmp(name.c_str(), L"Administrator") == 0 ||
           _wcsicmp(name.c_str(), L"Guest") == 0 ||
           _wcsicmp(name.c_str(), L"DefaultAccount") == 0;
}

class SysUsersModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.users"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"本地用户"; }
    const wchar_t* Description() const override { return L"本地账户枚举与启用/禁用（内置账户受保护）"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        names_.clear();
        flags_.clear();

        DWORD resume = 0;
        for (;;) {
            USER_INFO_3* ui = nullptr;
            DWORD read = 0, total = 0;
            NET_API_STATUS st = NetUserEnum(nullptr, 3, FILTER_NORMAL_ACCOUNT,
                                            (LPBYTE*)&ui, MAX_PREFERRED_LENGTH,
                                            &read, &total, &resume);
            if (st == NERR_Success || st == ERROR_MORE_DATA) {
                for (DWORD i = 0; i < read; ++i) AddUserRow(ui[i], out);
            }
            if (ui) NetApiBufferFree(ui);
            if (st != ERROR_MORE_DATA) {
                if (st != NERR_Success) {
                    out.summary = L"枚举本地用户失败：" + NerrTextZh(st);
                    return;
                }
                break;
            }
            if (ctx.Cancelled()) break;
        }

        out.summary = L"共 " + U32((unsigned)out.rows.size()) + L" 个本地账户"
                      L"（管理员排前 · 禁用账户红标）";
        out.hint = wstring(T(L"启用/禁用通过 NetUserSetInfo 实现，保留账户其余属性；"))
                   + T(L"Administrator/Guest/DefaultAccount 为内置账户，不允许操作。");
        out.canClean = true;
        ctx.Report(100, L"本地用户枚举完成");
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"启用选中账户", nullptr, false, 0xE768 },
            { L"禁用选中账户", nullptr, true, 0xE72E },   // BlockContact
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        if (tags.empty()) return L"未选中任何账户。\n\n请先在列表中勾选要操作的账户。";
        wstring names;
        int builtin = 0;
        for (int t : tags) {
            wstring n = (t >= 0 && t < (int)names_.size())
                            ? names_[t] : (L"账户索引 " + I64(t));
            if (t >= 0 && t < (int)names_.size() && IsBuiltinAccount(names_[t])) ++builtin;
            names += n + L"\n";
        }
        wstring body = wstring(actionIndex == 0 ? L"将启用 " : L"将禁用 ") +
                       I64((long long)tags.size()) + L" 个账户：\n" + names;
        if (actionIndex == 1)
            body += L"禁用后该账户无法登录（可随时重新启用）。\n";
        if (builtin > 0)
            body += L"\n注意：其中 " + I64(builtin) +
                    L" 个为系统内置账户，将被拒绝操作。";
        body += L"\n是否继续？";
        return body;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex < 0 || actionIndex > 1) return;
        if (selection_.empty()) {
            out.summary = L"未选中任何账户";
            out.canClean = true;
            return;
        }
        bool enable = (actionIndex == 0);
        int ok = 0, skip = 0, fail = 0;
        for (int tag : selection_) {
            if (ctx.Cancelled()) break;
            if (tag < 0 || tag >= (int)names_.size()) continue;
            const wstring& name = names_[tag];
            if (IsBuiltinAccount(name)) {
                ++skip;
                ctx.Log(L"  拒绝: " + name + L" — 系统内置账户，不允许启/禁用操作");
                continue;
            }
            DWORD cur = flags_[tag];
            bool curDisabled = (cur & UF_ACCOUNTDISABLE) != 0;
            if (curDisabled != enable) {
                ++skip;
                ctx.Log(L"  跳过: " + name +
                        (enable ? L" — 已是启用状态" : L" — 已是禁用状态"));
                continue;
            }
            // level 1008 只改 flags 位：清/置 UF_ACCOUNTDISABLE，其余保留
            USER_INFO_1008 ui = {};
            ui.usri1008_flags = enable ? (cur & ~UF_ACCOUNTDISABLE)
                                       : (cur | UF_ACCOUNTDISABLE);
            DWORD parmErr = 0;
            NET_API_STATUS st = NetUserSetInfo(nullptr, name.c_str(), 1008,
                                               (LPBYTE)&ui, &parmErr);
            if (st == NERR_Success) {
                ++ok;
                flags_[tag] = ui.usri1008_flags;
                ctx.Log(wstring(enable ? L"  已启用: " : L"  已禁用: ") + name);
            } else {
                ++fail;
                ctx.Log(L"  失败: " + name + L" — " + NerrTextZh(st));
            }
        }
        out.summary = wstring(enable ? L"启用账户完成：" : L"禁用账户完成：") +
                      L"成功 " + I64(ok) + L"，跳过 " + I64(skip) +
                      L"，失败 " + I64(fail);
        if (fail > 0 && !IsElevated())
            out.hint = T(L"部分操作失败，可能需要管理员权限（请提权后重试）。");
        out.canClean = true;
    }

private:
    vector<int> selection_;
    vector<wstring> names_;   // tag → 用户名
    vector<DWORD> flags_;     // tag → usri3_flags（启/禁用后回写，跳过判断用）

    void AddUserRow(const USER_INFO_3& ui, ModuleResult& out) {
        wstring name = ui.usri3_name ? ui.usri3_name : wstring();
        int tag = (int)names_.size();
        names_.push_back(name);
        flags_.push_back(ui.usri3_flags);

        bool disabled = (ui.usri3_flags & UF_ACCOUNTDISABLE) != 0;
        wstring c2 = disabled ? L"已禁用" : L"启用";
        if (ui.usri3_flags & UF_LOCKOUT) c2 += L" · 锁定";

        // 权限：2=管理员 1=标准用户 0=来宾
        wstring c3;
        switch (ui.usri3_priv) {
        case USER_PRIV_ADMIN: c3 = L"管理员"; break;
        case USER_PRIV_USER:  c3 = L"标准用户"; break;
        default:              c3 = L"来宾/其他"; break;
        }
        if (ui.usri3_flags & UF_DONT_EXPIRE_PASSWD) c3 += L" · 密码永不过期";
        if (ui.usri3_flags & UF_PASSWD_CANT_CHANGE) c3 += L" · 密码不可更改";

        wstring c4;
        if (ui.usri3_full_name && *ui.usri3_full_name) c4 += ui.usri3_full_name;
        if (ui.usri3_comment && *ui.usri3_comment) {
            if (!c4.empty()) c4 += L" · ";
            c4 += ui.usri3_comment;
        }
        wstring last = FormatUnixTime(ui.usri3_last_logon);
        if (!c4.empty()) c4 += L" · ";
        c4 += L"最后登录 " + last;

        ModuleRow r;
        r.c1 = name;
        r.c2 = c2;
        r.c3 = c3;
        r.c4 = c4;
        r.sortKey = ui.usri3_priv;      // 管理员在前
        r.danger = disabled;
        r.tag = tag;
        out.rows.push_back(r);
    }
};

// ------------------------------------------------ 3) sys.shares 共享目录

// 管理共享：IPC$ / ADMIN$ / X$（单盘符+$）—— 只读，不允许删除
bool IsManagementShare(const wstring& name) {
    if (_wcsicmp(name.c_str(), L"IPC$") == 0) return true;
    if (_wcsicmp(name.c_str(), L"ADMIN$") == 0) return true;
    if (name.size() == 2 && name[1] == L'$' && iswalpha(name[0])) return true;
    return false;
}

class SysSharesModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.shares"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"共享目录"; }
    const wchar_t* Description() const override { return L"本机共享枚举与删除（管理共享只读保护）"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        names_.clear();

        DWORD resume = 0;
        for (;;) {
            SHARE_INFO_2* si = nullptr;
            DWORD read = 0, total = 0;
            NET_API_STATUS st = NetShareEnum(nullptr, 2, (LPBYTE*)&si,
                                             MAX_PREFERRED_LENGTH,
                                             &read, &total, &resume);
            if (st == NERR_Success || st == ERROR_MORE_DATA) {
                for (DWORD i = 0; i < read; ++i) AddShareRow(si[i], out);
            }
            if (si) NetApiBufferFree(si);
            if (st != ERROR_MORE_DATA) {
                if (st != NERR_Success) {
                    out.summary = L"枚举共享失败：" + NerrTextZh(st);
                    return;
                }
                break;
            }
            if (ctx.Cancelled()) break;
        }

        int mgmt = 0;
        for (auto& r : out.rows)
            if (r.c3.rfind(L"管理", 0) == 0) ++mgmt;
        out.summary = L"共 " + U32((unsigned)out.rows.size()) + L" 个共享"
                      L"（其中管理共享 " + I64(mgmt) + L" 个，只读保护）";
        out.hint = T(L"管理共享（IPC$、ADMIN$、X$）由系统维护，删除操作会被拒绝。");
        out.canClean = true;
        ctx.Report(100, L"共享枚举完成");
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"删除选中共享", nullptr, true, 0xE74D },   // Delete
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int, const vector<int>& tags) const override {
        if (tags.empty()) return L"未选中任何共享。\n\n请先在列表中勾选要删除的共享。";
        wstring names;
        int mgmt = 0;
        for (int t : tags) {
            wstring n = (t >= 0 && t < (int)names_.size())
                            ? names_[t] : (L"共享索引 " + I64(t));
            if (t >= 0 && t < (int)names_.size() && IsManagementShare(names_[t])) ++mgmt;
            names += n + L"\n";
        }
        wstring body = L"将永久删除 " + I64((long long)tags.size()) +
                       L" 个共享（共享本身不会被删除，仅取消共享状态）：\n" + names;
        if (mgmt > 0)
            body += L"\n注意：其中 " + I64(mgmt) +
                    L" 个为系统管理共享，将被拒绝删除。";
        body += L"\n是否继续？";
        return body;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (selection_.empty()) {
            out.summary = L"未选中任何共享";
            out.canClean = true;
            return;
        }
        int ok = 0, skip = 0, fail = 0;
        for (int tag : selection_) {
            if (ctx.Cancelled()) break;
            if (tag < 0 || tag >= (int)names_.size()) continue;
            const wstring& name = names_[tag];
            if (IsManagementShare(name)) {
                ++skip;
                ctx.Log(L"  拒绝: " + name + L" — 系统管理共享，不允许删除");
                continue;
            }
            NET_API_STATUS st = NetShareDel(nullptr, (LPWSTR)name.c_str(), 0);
            if (st == NERR_Success) {
                ++ok;
                ctx.Log(L"  已删除共享: " + name);
            } else {
                ++fail;
                ctx.Log(L"  失败: " + name + L" — " + NerrTextZh(st));
            }
        }
        out.summary = L"删除共享完成：成功 " + I64(ok) +
                      L"，跳过 " + I64(skip) + L"，失败 " + I64(fail);
        if (fail > 0 && !IsElevated())
            out.hint = T(L"部分操作失败，可能需要管理员权限（请提权后重试）。");
        out.canClean = true;
    }

private:
    vector<int> selection_;
    vector<wstring> names_;   // tag → 共享名

    void AddShareRow(const SHARE_INFO_2& si, ModuleResult& out) {
        wstring name = si.shi2_netname ? si.shi2_netname : wstring();
        int tag = (int)names_.size();
        names_.push_back(name);

        wstring c3 = ShareTypeText(si.shi2_type);
        bool mgmt = IsManagementShare(name);

        wstring c4;
        if (si.shi2_remark && *si.shi2_remark) c4 += si.shi2_remark;
        if (!c4.empty()) c4 += L" · ";
        c4 += L"连接 " + I64(si.shi2_current_uses) + L" / 上限 " + I64(si.shi2_max_uses);

        ModuleRow r;
        r.c1 = name;
        r.c2 = (si.shi2_path && *si.shi2_path) ? si.shi2_path : wstring(L"-");
        r.c3 = c3;
        r.c4 = c4;
        r.sortKey = mgmt ? 2 : 0;      // 管理共享排前
        r.danger = false;
        r.tag = tag;
        out.rows.push_back(r);
    }

    static wstring ShareTypeText(DWORD type) {
        // MinGW lmshare.h 未导出 STYPE_MASK：低 4 位为基本类型（0~3），高位为标志
        DWORD base = type & 0x0000000F;
        const wchar_t* s = L"其他";
        switch (base) {
        case STYPE_DISKTREE: s = L"磁盘共享"; break;
        case STYPE_PRINTQ:   s = L"打印共享"; break;
        case STYPE_DEVICE:   s = L"设备共享"; break;
        case STYPE_IPC:      s = L"IPC 通信"; break;
        }
        if (type & STYPE_SPECIAL) return wstring(L"管理 · ") + s;
        return s;
    }
};

// ------------------------------------------------ 4) sys.events 事件日志

// 聚合条目：来源 + 事件ID
struct EventAgg {
    wstring provider;
    unsigned id = 0;
    int count = 0;
    int worstLevel = 9;      // 级别越小越严重（1 严重 / 2 错误）
    wstring lastTime;
    wstring lastChannel;
};

class SysEventsModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.events"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"事件日志分析"; }
    const wchar_t* Description() const override { return L"近 7 天错误/严重事件聚合（来源+ID Top 20）"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        std::map<wstring, EventAgg> agg;
        int totalEvents = 0;

        // 近 7 天、级别 ≤ 2（严重/错误）
        const wchar_t* kXPath =
            L"*[System[(Level<=2) and TimeCreated[timediff(@SystemTime) <= 604800000]]]";
        static const wchar_t* kChannels[] = { L"System", L"Application" };
        const int kMaxPerChannel = 20000;   // 单通道扫描上限，防极端环境拖死

        EVT_HANDLE renderCtx = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
        if (!renderCtx) {
            out.summary = L"初始化事件渲染上下文失败：" +
                          ErrorTextZh(GetLastError());
            return;
        }

        for (const wchar_t* ch : kChannels) {
            if (ctx.Cancelled()) break;
            ctx.Report(-1, wstring(L"正在扫描 ") + ch + L" 通道错误事件...");
            EVT_HANDLE q = EvtQuery(nullptr, (LPWSTR)ch, (LPWSTR)kXPath,
                                    EvtQueryChannelPath | EvtQueryReverseDirection);
            if (!q) {
                // Security 等受限通道静默跳过；其余通道记录原因
                ctx.Log(L"  打开通道失败（跳过）: " + wstring(ch) +
                        L" — " + ErrorTextZh(GetLastError()));
                continue;
            }
            int scanned = 0;
            for (;;) {
                if (ctx.Cancelled() || scanned >= kMaxPerChannel) break;
                EVT_HANDLE ev = nullptr;
                ULONG ret = 0;
                if (!EvtNext(q, 1, &ev, 1000, 0, &ret) || ret == 0) {
                    DWORD e = GetLastError();
                    if (e != ERROR_NO_MORE_ITEMS)
                        ctx.Log(L"  读取事件失败: " + ErrorTextZh(e));
                    if (ev) EvtClose(ev);
                    break;
                }
                ++scanned;
                AggregateOne(renderCtx, ev, ch, agg);
                EvtClose(ev);
            }
            EvtClose(q);
            ctx.Log(L"  " + wstring(ch) + L" 通道扫描 " + I64(scanned) + L" 条");
            totalEvents += scanned;
        }
        EvtClose(renderCtx);

        // Top 20：按出现次数降序
        vector<EventAgg> list;
        list.reserve(agg.size());
        for (auto& kv : agg) list.push_back(kv.second);
        std::sort(list.begin(), list.end(),
                  [](const EventAgg& a, const EventAgg& b) {
                      if (a.count != b.count) return a.count > b.count;
                      return a.provider < b.provider;
                  });
        if (list.size() > 20) list.resize(20);

        for (auto& a : list) {
            ModuleRow r;
            r.c1 = a.provider;
            r.c2 = I64(a.count) + L" 次";
            r.c3 = L"ID " + I64(a.id) + L" · " + LevelText(a.worstLevel);
            r.c4 = L"最近 " + a.lastTime + L" · " + a.lastChannel;
            r.sortKey = (unsigned long long)a.count;
            r.danger = a.worstLevel <= 1;
            r.tag = -1;
            out.rows.push_back(r);
        }

        out.summary = L"近 7 天 System/Application 错误与严重事件 " +
                      I64(totalEvents) + L" 条，聚合为 " +
                      I64((long long)agg.size()) + L" 类（显示 Top 20）";
        out.hint = wstring(T(L"聚合口径：按「来源 + 事件 ID」；Security 通道需审计权限，未包含。"))
                   + T(L"事件详情可在事件查看器 (eventvwr) 中按来源与 ID 检索。");
        out.canClean = false;
        ctx.Report(100, L"事件日志分析完成");
    }

private:
    // 单条事件 → 聚合表（System 属性渲染上下文，按 EVT_SYSTEM_PROPERTY_ID 取值）
    void AggregateOne(EVT_HANDLE renderCtx, EVT_HANDLE ev,
                      const wchar_t* channel, std::map<wstring, EventAgg>& agg) {
        // EvtRender（EvtRenderEventValues）两段式：先取所需字节数再真正渲染
        DWORD used = 0, count = 0;
        if (!EvtRender(renderCtx, ev, EvtRenderEventValues, 0, nullptr, &used, &count)) {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0) return;
            vector<BYTE> buf(used);
            DWORD used2 = 0, count2 = 0;
            if (!EvtRender(renderCtx, ev, EvtRenderEventValues, used,
                           buf.data(), &used2, &count2)) return;
            auto* vals = reinterpret_cast<PEVT_VARIANT>(buf.data());

            // ProviderName（EvtVarTypeString）
            wstring provider;
            const EVT_VARIANT& vProv = vals[EvtSystemProviderName];
            if ((vProv.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && vProv.StringVal)
                provider = vProv.StringVal;
            if (provider.empty()) provider = L"(未知来源)";

            // EventID（UInt16 / UInt32 / Int32 兼容）
            unsigned id = 0;
            const EVT_VARIANT& vId = vals[EvtSystemEventID];
            switch (vId.Type & EVT_VARIANT_TYPE_MASK) {
            case EvtVarTypeUInt16: id = vId.UInt16Val; break;
            case EvtVarTypeInt16:  if (vId.Int16Val >= 0) id = (unsigned)vId.Int16Val; break;
            case EvtVarTypeUInt32: id = vId.UInt32Val; break;
            case EvtVarTypeInt32:  if (vId.Int32Val >= 0) id = (unsigned)vId.Int32Val; break;
            default: break;
            }

            // Level（UInt8 / UInt32 / Int32 兼容；越小越严重）
            int level = 9;
            const EVT_VARIANT& vLvl = vals[EvtSystemLevel];
            switch (vLvl.Type & EVT_VARIANT_TYPE_MASK) {
            case EvtVarTypeByte:   level = vLvl.ByteVal; break;
            case EvtVarTypeUInt32: level = (int)vLvl.UInt32Val; break;
            case EvtVarTypeInt32:  level = vLvl.Int32Val; break;
            default: break;
            }

            // TimeCreated（EvtVarTypeFileTime，UTC）
            wstring lastTime;
            const EVT_VARIANT& vTm = vals[EvtSystemTimeCreated];
            if ((vTm.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeFileTime) {
                FILETIME ft;
                ft.dwLowDateTime = (DWORD)(vTm.FileTimeVal & 0xFFFFFFFFULL);
                ft.dwHighDateTime = (DWORD)(vTm.FileTimeVal >> 32);
                lastTime = FormatFileTime(ft);
            }

            wstring key = provider + L"#" + I64(id);
            EventAgg& a = agg[key];
            a.provider = provider;
            a.id = id;
            ++a.count;
            if (level >= 1 && level < a.worstLevel) a.worstLevel = level;
            if (!lastTime.empty()) a.lastTime = lastTime;
            a.lastChannel = channel;
        }
    }

    static wstring LevelText(int level) {
        switch (level) {
        case 1: return L"严重";
        case 2: return L"错误";
        case 3: return L"警告";
        default: return L"未分级";   // 部分应用事件无 Level 字段
        }
    }
};

// ------------------------------------------------ 5) sys.power 电源计划

// MinGW powrprof.h 缺 PowerGetActiveScheme / PowerSetActiveScheme 声明，
// 动态加载（Vista+ 系统自带，GetProcAddress 失败视为系统过旧）。
typedef DWORD(WINAPI* PFN_PowerGetActiveScheme)(HANDLE, GUID**);
typedef DWORD(WINAPI* PFN_PowerSetActiveScheme)(HANDLE, const GUID*);

PFN_PowerGetActiveScheme LoadGetActive() {
    static PFN_PowerGetActiveScheme fn = reinterpret_cast<PFN_PowerGetActiveScheme>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"powrprof.dll"),
                                               "PowerGetActiveScheme")));
    return fn;
}
PFN_PowerSetActiveScheme LoadSetActive() {
    static PFN_PowerSetActiveScheme fn = reinterpret_cast<PFN_PowerSetActiveScheme>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"powrprof.dll"),
                                               "PowerSetActiveScheme")));
    return fn;
}

class SysPowerModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.power"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"电源计划"; }
    const wchar_t* Description() const override { return L"电源计划切换与「卓越性能」模式启用"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        guids_.clear();
        names_.clear();

        // 当前活动方案（动态加载；取不到则不做高亮）
        GUID active = {};
        bool haveActive = false;
        if (PFN_PowerGetActiveScheme fn = LoadGetActive()) {
            GUID* p = nullptr;
            if (fn(nullptr, &p) == ERROR_SUCCESS && p) {
                active = *p;
                haveActive = true;
                LocalFree(p);
            }
        }

        // 枚举全部电源方案（ACCESS_SCHEME）
        for (ULONG i = 0;; ++i) {
            if (ctx.Cancelled()) break;
            GUID g = {};
            DWORD sz = sizeof(g);
            DWORD err = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME,
                                       i, (UCHAR*)&g, &sz);
            if (err == ERROR_NO_MORE_ITEMS) break;
            if (err != ERROR_SUCCESS) {
                ctx.Log(L"  枚举电源计划失败: " + ErrorTextZh(err));
                break;
            }
            int tag = (int)guids_.size();
            guids_.push_back(g);

            wchar_t nm[128] = {};
            DWORD sz2 = sizeof(nm);
            wstring name;
            if (PowerReadFriendlyName(nullptr, &g, nullptr, nullptr,
                                      (UCHAR*)nm, &sz2) == ERROR_SUCCESS && sz2 > 0)
                name = nm;
            names_.push_back(name);

            bool isActive = haveActive &&
                            memcmp(&g, &active, sizeof(GUID)) == 0;
            ModuleRow r;
            r.c1 = name.empty() ? GuidToStr(g) : name;
            r.c2 = GuidToStr(g);
            r.c3 = isActive ? L"当前使用" : L"";
            r.c4 = L"选择后可一键切换";
            r.sortKey = isActive ? 1 : 0;   // 活动方案排前
            r.danger = false;
            r.tag = tag;
            out.rows.push_back(r);
        }

        out.summary = L"共 " + U32((unsigned)out.rows.size()) +
                      L" 个电源计划（当前使用的排前）";
        out.hint = T(L"「卓越性能」为系统隐藏计划，启用即复制一份到本机并可在列表中切换。");
        out.canClean = true;
        ctx.Report(100, L"电源计划枚举完成");
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"切换到选中方案", nullptr, false, 0xE768 },
            { L"启用卓越性能", nullptr, false, 0xE945 },   // LightningBolt
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        if (actionIndex == 1) {
            return L"将通过 powercfg -duplicatescheme 复制一份系统隐藏的"
                   L"「卓越性能」电源计划（GUID e9a42b02-d5df-448d-aa83-0b785bddbc56）。\n"
                   L"不会改动现有计划；复制后可在列表中随时切换。\n\n是否继续？";
        }
        if (tags.empty())
            return L"未选中任何电源计划。\n\n请先在列表中勾选要切换到的方案。";
        wstring names;
        for (int t : tags) {
            if (t >= 0 && t < (int)names_.size()) names += names_[t] + L"\n";
            else names += L"方案索引 " + I64(t) + L"\n";
        }
        return L"将把活动电源计划切换为：\n" + names + L"\n是否继续？";
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex == 0) {
            if (selection_.empty()) {
                out.summary = L"未选中任何电源计划";
                out.canClean = true;
                return;
            }
            int tag = selection_[0];   // 活动方案唯一，取第一个选中
            if (tag < 0 || tag >= (int)guids_.size()) {
                out.summary = L"选中项无效";
                out.canClean = true;
                return;
            }
            PFN_PowerSetActiveScheme fn = LoadSetActive();
            if (!fn) {
                out.summary = L"系统不支持电源计划切换（缺少 PowerSetActiveScheme）";
                out.canClean = true;
                return;
            }
            DWORD err = fn(nullptr, &guids_[tag]);
            if (err == ERROR_SUCCESS) {
                wstring n = (tag < (int)names_.size() && !names_[tag].empty())
                                ? names_[tag] : GuidToStr(guids_[tag]);
                out.summary = L"已切换电源计划：" + n;
                ctx.Log(L"  已激活: " + n + L" (" + GuidToStr(guids_[tag]) + L")");
            } else {
                out.summary = L"切换电源计划失败：" + ErrorTextZh(err);
                if (!IsElevated())
                    out.hint = T(L"部分系统策略要求管理员权限才能切换电源计划。");
            }
            out.canClean = true;
            return;
        }
        if (actionIndex == 1) {
            // 已存在「卓越性能」则跳过（按友好名识别，中英兼容）
            for (auto& n : names_) {
                if (ContainsI(n, L"卓越性能") || ContainsI(n, L"Ultimate Performance")) {
                    out.summary = L"已存在「卓越性能」计划，无需重复启用（可在列表中直接切换）";
                    out.canClean = true;
                    return;
                }
            }
            ctx.Report(-1, L"正在复制「卓越性能」电源计划...");
            wstring cmd =
                L"powercfg -duplicatescheme e9a42b02-d5df-448d-aa83-0b785bddbc56";
            ctx.Log(L"  $ " + cmd);
            ProcRunOptions opt;
            opt.timeoutMs = 60000;
            opt.cancel = [&ctx]() { return ctx.Cancelled(); };
            opt.onLine = [&ctx](const wstring& line) { ctx.Log(L"  " + line); };
            wstring full;
            int code = RunProcess(cmd, opt, &full);
            if (code == -3) { out.summary = L"已取消"; out.canClean = true; return; }
            if (code == 0) {
                out.summary = L"「卓越性能」计划已复制，列表中即可切换";
                ctx.Log(L"  复制成功（重新扫描后可见）");
            } else if (code == -1) {
                out.summary = L"powercfg 启动失败";
            } else {
                out.summary = L"复制失败（退出码 " + I64(code) + L"，详见上方日志）";
                if (!IsElevated())
                    out.hint = T(L"duplicatescheme 需要管理员权限（请提权后重试）。");
            }
            out.canClean = true;
        }
    }

private:
    vector<int> selection_;
    vector<GUID> guids_;      // tag → 电源计划 GUID
    vector<wstring> names_;   // tag → 友好名
};

// ------------------------------------------------ 6) sys.update 缺失更新

// WU 检索结果码 → 中文
wstring WuResultText(long rc) {
    switch (rc) {
    case 2: return L"成功";
    case 3: return L"成功（部分结果有误）";
    case 4: return L"失败";
    case 5: return L"已中止";
    default: return L"未知（" + I64(rc) + L"）";
    }
}

class SysUpdateModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.update"; }
    const wchar_t* Group() const override { return L"常用管理"; }
    const wchar_t* Title() const override { return L"缺失更新检查"; }
    const wchar_t* Description() const override { return L"Windows Update 缺失更新检索（只查不装）"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }

    // 扫描 = 连 WUAPI 检索（同步阻塞，约 1~5 分钟，暂不支持中途取消）
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool comUninit = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            out.summary = L"COM 初始化失败（错误码 " + I64((long long)hr) + L"）";
            return;
        }

        IUpdateSession* session = nullptr;
        IUpdateSearcher* searcher = nullptr;
        ISearchResult* result = nullptr;
        IUpdateCollection* coll = nullptr;
        wstring failReason;

        do {
            hr = CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUpdateSession, (void**)&session);
            if (FAILED(hr) || !session) {
                failReason = L"无法创建 Windows Update 会话：" + HrTextZh(hr);
                break;
            }
            hr = session->CreateUpdateSearcher(&searcher);
            if (FAILED(hr) || !searcher) {
                failReason = L"无法创建更新检索器：" + HrTextZh(hr);
                break;
            }

            ctx.Report(-1, L"正在连接 Windows Update 检索缺失更新"
                           L"（同步阻塞，约 1~5 分钟，请耐心等待）...");
            ctx.Log(L"  检索条件: IsInstalled=0 and IsHidden=0（只查不装）");

            BSTR criteria = SysAllocString(L"IsInstalled=0 and IsHidden=0");
            hr = criteria ? searcher->Search(criteria, &result) : E_OUTOFMEMORY;
            SysFreeString(criteria);
            if (FAILED(hr) || !result) {
                failReason = L"更新检索失败：" + HrTextZh(hr) +
                             L"（网络不通/WSUS 策略限制均会导致）";
                break;
            }

            OperationResultCode rc = orcNotStarted;
            result->get_ResultCode(&rc);
            if (rc != 2 && rc != 3) {
                failReason = L"检索未成功：" + WuResultText(rc);
                break;
            }
            hr = result->get_Updates(&coll);
            if (FAILED(hr) || !coll) {
                failReason = L"读取检索结果失败：" + HrTextZh(hr);
                break;
            }

            LONG n = 0;
            coll->get_Count(&n);
            unsigned long long totalBytes = 0;
            for (LONG i = 0; i < n; ++i) {
                if (ctx.Cancelled()) break;
                IUpdate* upd = nullptr;
                if (FAILED(coll->get_Item(i, &upd)) || !upd) continue;

                BSTR bTitle = nullptr;
                upd->get_Title(&bTitle);
                wstring title = TakeBstr(bTitle);

                DECIMAL dec = {};
                double bytes = 0.0;
                if (SUCCEEDED(upd->get_MaxDownloadSize(&dec)))
                    VarR8FromDec(&dec, &bytes);

                BSTR bSev = nullptr;
                upd->get_MsrcSeverity(&bSev);
                wstring sev = TakeBstr(bSev);

                ModuleRow r;
                r.c1 = title.empty() ? L"(无标题更新)" : title;
                r.c2 = FormatSize((unsigned long long)(bytes > 0 ? bytes : 0));
                r.c3 = SevText(sev);
                r.c4 = L"仅检索 · 安装请在系统更新设置中操作";
                r.sortKey = (unsigned long long)(bytes > 0 ? bytes : 0);
                r.danger = false;
                r.tag = -1;
                out.rows.push_back(r);
                totalBytes += (unsigned long long)(bytes > 0 ? bytes : 0);
                upd->Release();
            }

            out.summary = L"发现 " + I64(n) + L" 个缺失更新（合计约 " +
                          FormatSize(totalBytes) + L"）· 检索结果：" + WuResultText(rc);
            out.hint = wstring(T(L"本模块只查询不安装；安装请在「设置 → Windows 更新」中进行。"))
                       + T(L"检索为同步阻塞，如长时间无响应多为网络受限。");
            ctx.Report(100, L"缺失更新检索完成");
        } while (false);

        if (coll) coll->Release();
        if (result) result->Release();
        if (searcher) searcher->Release();
        if (session) session->Release();
        if (comUninit) CoUninitialize();

        if (!failReason.empty()) {
            out.summary = failReason;
            out.hint = T(L"常见原因：网络受限/离线、WSUS 组策略接管、Windows Update 服务未运行。");
            out.canClean = false;
            return;
        }
        out.canClean = false;
    }

private:
    static wstring SevText(const wstring& sev) {
        if (sev.empty()) return L"未分级";
        if (ContainsI(sev, L"Critical")) return L"严重";
        if (ContainsI(sev, L"Important")) return L"重要";
        if (ContainsI(sev, L"Moderate")) return L"中等";
        if (ContainsI(sev, L"Low")) return L"低";
        return sev;
    }
};

} // namespace

REGISTER_MODULE(SysRestoreModule);
REGISTER_MODULE(SysUsersModule);
REGISTER_MODULE(SysSharesModule);
REGISTER_MODULE(SysEventsModule);
REGISTER_MODULE(SysPowerModule);
REGISTER_MODULE(SysUpdateModule);

} // namespace mz
