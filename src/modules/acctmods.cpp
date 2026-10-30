// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 账户与域模块集（v2.9 T02）
//  ---------------------------------------------------------------------------
//  两个模块，Group 均为「系统管理」：
//    * sys.acct    账户部署：NetUserAdd(1)/NetUserSetInfo(1003)/NetUserDel
//                  本地账户枚举（内置账户保护）+ Winlogon 自动登录四键配置；
//                  空密码账户 UF_PASSWD_NOTREQD，密码值绝不回显
//    * sys.domain  计算机名与域：SetComputerNameExW(DNS 主机名) 重命名、
//                  NetJoinDomain/NetUnjoinDomain 加域/退域（危险动作均 danger
//                  确认、绝不代重启）；加入域连续失败 2 次即停（防计算机
//                  账户锁定，风险登记册 #1）
//  错误码中文：NERR 段（2221/2691-2703 等）+ Win32（5/86/1326/1355）本地归因。
//  链接：-lnetapi32（build.bat 已有）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"   // ErrorTextZh（Win32 错误码中文归因）

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include <lm.h>           // NetUser* / NetJoinDomain / NetWkstaGetInfo

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

// 内置账户（不允许创建覆盖/删除操作，与 sys.users 同一保护名单）
bool IsBuiltinAccount(const wstring& name) {
    return _wcsicmp(name.c_str(), L"Administrator") == 0 ||
           _wcsicmp(name.c_str(), L"Guest") == 0 ||
           _wcsicmp(name.c_str(), L"DefaultAccount") == 0;
}

// LANMAN/NERR 错误码 → 中文（覆盖用户与加退域常见码，其余回落 Win32 归因）
wstring NerrTextZh(DWORD st) {
    switch (st) {
    case NERR_Success:        return L"成功";
    case ERROR_ACCESS_DENIED: return ErrorTextZh(st);
    case 2102: /*NERR_NetNotFound*/       return L"网络组件未启动";
    case 2107: /*NERR_InvalidWorkstation*/return L"操作被拒绝：无效的操作数";
    case 2114: /*NERR_ServerNotStarted*/  return L"Server 服务未运行，无法操作账户";
    case 2202: /*NERR_BadUsername*/       return L"用户名无效";
    case 2221: /*NERR_UserNotFound*/      return L"用户不存在（可能已被删除）";
    case 2224: /*NERR_UserExists*/        return L"用户已存在";
    case 2242: /*NERR_BadPassword*/       return L"密码无效（不符合复杂度或长度策略）";
    case 2245: /*NERR_PasswordTooShort*/  return L"密码长度低于策略要求";
    case 2246: /*NERR_PasswordTooRecent*/ return L"密码最近刚改过，暂不允许重设";
    case 2247: /*NERR_InvalidDatabase*/   return L"账户数据库损坏，无法完成操作";
    case 2249: /*NERR_PasswordTooLong*/   return L"密码超长";
    case 2691: /*NERR_SetupAlreadyJoined*/    return L"本机已加入域（如需重新加入请先退域）";
    case 2692: /*NERR_SetupNotJoined*/        return L"本机未加入域";
    case 2693: /*NERR_SetupDomainExists*/     return L"域中已存在同名计算机账户";
    case 2694: /*NERR_DomainControllerNotFound*/ return L"找不到域控制器（检查 DNS/网络连通性）";
    case 2695: /*NERR_CouldNotOpenDomain*/    return L"无法联系域（网络不通或凭据不足）";
    case 2697: /*NERR_JoinInternalError*/    return L"加域内部错误";
    case 2698: /*NERR_InvalidWorkgroupName*/  return L"工作组名无效";
    case 2699: /*NERR_NameInUse*/             return L"名称已被使用";
    case 2701: /*NERR_ComputerAccountNotFound*/ return L"域中找不到本机计算机账户（可能已被删除）";
    case 2702: /*NERR_PasswdMustChange*/      return L"域要求账户密码必须更改";
    case 2703: /*NERR_AccountExpired*/        return L"域账户已过期";
    default: return ErrorTextZh(st);
    }
}

// Win32 错误码 → 中文（加域/重命名常见码前置，其余回落统一归因）
wstring Win32ErrZh(DWORD code) {
    switch (code) {
    case 5:    return L"权限不足（需要管理员权限）";
    case 86:   return L"密码错误（ERROR_INVALID_PASSWORD）";
    case 1326: return L"登录失败：用户名或密码不正确";
    case 1355: return L"找不到域或域控制器（检查 DNS/网络）";
    case 1907: return L"用户密码已过期，须先改密";
    case 2202: return L"用户名无效";
    case 2224: return L"用户已存在";
    default:   return ErrorTextZh(code);
    }
}

// 统一入口：NET_API_STATUS 可能落在 NERR 段或 Win32 段
wstring ApiErrTextZh(DWORD st) {
    if (st >= 2100) return NerrTextZh(st);
    return Win32ErrZh(st);
}

// ---------------------------------------------------------------- 注册表读写

// 读 REG_SZ（不存在/失败返回 false）
bool RegReadStr(HKEY root, const wchar_t* sub, const wchar_t* name, wstring& val) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    wchar_t buf[512] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    LONG r = RegGetValueW(hk, nullptr, name, RRF_RT_REG_SZ, &type, buf, &sz);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return false;
    val.assign(buf, sz / sizeof(wchar_t) - 1);
    return true;
}

// 读 REG_DWORD（不存在/失败返回 -1）
DWORD RegReadDword(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return (DWORD)-1;
    DWORD v = 0, sz = sizeof(v), type = 0;
    LONG r = RegGetValueW(hk, nullptr, name, RRF_RT_REG_DWORD, &type, &v, &sz);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return (DWORD)-1;
    return v;
}

// 写 REG_SZ（键不存在自动创建）
LONG RegWriteStr(HKEY root, const wchar_t* sub, const wchar_t* name,
                 const wstring& val) {
    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE,
                             nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(hk, name, 0, REG_SZ,
                       (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return r;
}

// 删除值（值不存在也算成功）
LONG RegDelValue(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY hk = nullptr;
    LONG r = RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &hk);
    if (r != ERROR_SUCCESS) return r;
    r = RegDeleteValueW(hk, name);
    RegCloseKey(hk);
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    return r;
}

const wchar_t* kWinlogon = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
const wchar_t* kLsaKey   = L"SYSTEM\\CurrentControlSet\\Control\\Lsa";

// ---------------------------------------------------------------- 提权拒绝
// 照抄 secmods.cpp:581-589 范式：非管理员拒绝写操作，中文归因进摘要+日志
bool RequireElevation(ModuleContext& ctx, const wstring& label, ModuleResult& out) {
    if (IsElevated()) return true;
    out.summary = L"需要管理员权限：「" + label + L"」未执行";
    out.hint = T(L"请以管理员身份运行本程序（启动时会自动请求提权）。");
    ctx.Log(L"  拒绝: 当前非管理员，动作「" + label + L"」需要管理员权限");
    out.canClean = true;
    return false;
}

// 用户名合法性（NetUserAdd 校验前置）：非空、≤20 字符、不含非法字符
wstring ValidateUserName(const wstring& name) {
    if (name.empty()) return L"用户名不能为空";
    if (name.size() > 20) return L"用户名超长（NetBIOS 限制 ≤20 字符）";
    const wchar_t* bad = L"\"/\\[]:;|=,+*?<>";
    if (name.find_first_of(bad) != wstring::npos)
        return L"用户名包含非法字符（不可含 \" / \\ [ ] : ; | = , + * ? < >）";
    if (name.find_first_not_of(L" ") == wstring::npos) return L"用户名不能全为空格";
    return L"";
}

// =========================================================== 1) sys.acct ===

class SysAcctModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.acct"; }
    const wchar_t* Group() const override { return L"系统管理"; }
    const wchar_t* Title() const override { return L"账户部署"; }
    const wchar_t* Description() const override {
        return L"本地账户创建/改密/删除 + 自动登录配置（空密码账户会提示共享限制）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsElevation() const override { return true; }
    bool NeedsTarget() const override { return true; }
    bool NeedsPath() const override { return true; }
    const wchar_t* TargetHint() const override {
        return L"目标用户名（创建/更新/删除，≤20 字符）";
    }
    const wchar_t* PathHint() const override {
        return L"密码（留空=无密码）；或「描述|密码」两段式";
    }
    void SetInput(const wstring& path, const wstring& target) override {
        path_ = path;
        target_ = target;
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};

        // ---- 1) 本地账户枚举（NetUserEnum level 3，sys.users 同款范式）----
        DWORD resume = 0;
        int users = 0;
        for (;;) {
            USER_INFO_3* ui = nullptr;
            DWORD read = 0, total = 0;
            NET_API_STATUS st = NetUserEnum(nullptr, 3, FILTER_NORMAL_ACCOUNT,
                                            (LPBYTE*)&ui, MAX_PREFERRED_LENGTH,
                                            &read, &total, &resume);
            if (st == NERR_Success || st == ERROR_MORE_DATA) {
                for (DWORD i = 0; i < read; ++i) {
                    const USER_INFO_3& u = ui[i];
                    wstring name = u.usri3_name ? u.usri3_name : L"";
                    wstring c2 = (u.usri3_flags & UF_ACCOUNTDISABLE) ? L"已禁用" : L"启用";
                    wstring c3;
                    switch (u.usri3_priv) {
                    case USER_PRIV_ADMIN: c3 = L"管理员"; break;
                    case USER_PRIV_USER:  c3 = L"标准用户"; break;
                    default:              c3 = L"来宾/其他"; break;
                    }
                    if (u.usri3_flags & UF_PASSWD_NOTREQD) c3 += L" · 无需密码";
                    wstring c4 = (u.usri3_comment && *u.usri3_comment)
                                     ? u.usri3_comment : wstring();
                    if (IsBuiltinAccount(name)) c4 += (c4.empty() ? L"" : L" · ") +
                                                     wstring(L"内置账户");
                    ModuleRow r;
                    r.c1 = name; r.c2 = c2; r.c3 = c3; r.c4 = c4;
                    r.sortKey = u.usri3_priv;
                    r.danger = (u.usri3_flags & UF_ACCOUNTDISABLE) != 0;
                    out.rows.push_back(r);
                    ++users;
                }
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

        // ---- 2) Winlogon 自动登录状态（密码值绝不回显）----
        wstring aal, dun, dpwd;
        bool hasAal = RegReadStr(HKEY_LOCAL_MACHINE, kWinlogon,
                                 L"AutoAdminLogon", aal) && aal == L"1";
        bool hasUser = RegReadStr(HKEY_LOCAL_MACHINE, kWinlogon,
                                  L"DefaultUserName", dun);
        bool hasPwd = RegReadStr(HKEY_LOCAL_MACHINE, kWinlogon,
                                 L"DefaultPassword", dpwd);
        {
            ModuleRow r;
            r.c1 = L"自动登录 (Winlogon)";
            r.c2 = hasAal ? L"已配置" : L"未配置";
            r.c3 = L"自动登录";
            if (hasAal) {
                r.c4 = wstring(L"账户 ") + (hasUser ? dun : L"(未指定)") +
                       L" · 密码" + (hasPwd ? L"已设置(不回显)" : L"未设置");
                r.danger = true;   // 明文密码落盘，红标提醒
            } else {
                r.c4 = L"开机需手动输入密码";
            }
            out.rows.push_back(r);
        }

        // ---- 3) Lsa 空密码网络登录限制（只读展示）----
        DWORD lbpu = RegReadDword(HKEY_LOCAL_MACHINE, kLsaKey,
                                  L"LimitBlankPasswordUse");
        {
            ModuleRow r;
            r.c1 = L"空密码网络登录限制";
            r.c2 = (lbpu == (DWORD)-1) ? L"默认(=1)"
                 : (lbpu == 0)         ? L"已关闭(=0)"
                                       : L"已启用(=1)";
            r.c3 = L"LSA 策略";
            r.c4 = L"LimitBlankPasswordUse：=1 时空密码账户不能用于共享访问/网络登录"
                   L"（交互登录与自动登录不受限）";
            out.rows.push_back(r);
        }

        out.summary = L"共 " + U32((unsigned)users) + L" 个本地账户 · 自动登录" +
                      (hasAal ? L"已配置" : L"未配置");
        out.hint = wstring(T(L"目标框=用户名，路径框=密码（留空=无密码）或「描述|密码」。"))
                   + T(L"动作：①创建/更新账户 ②配置自动登录；「更多 ▾」= 关闭自动登录 / 删除指定账户。");
        out.canClean = true;
        ctx.Report(100, L"账户盘点完成");
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"创建/更新账户", nullptr, false, 0xE77B },  // Contact
            { L"配置自动登录", nullptr, true,  0xE7EF },   // Admin（危险态）
            { L"关闭自动登录",
              L"将删除 Winlogon 的 AutoAdminLogon 与 DefaultPassword（保留 "
              L"DefaultUserName），重启后恢复手动登录。\n\n是否继续？",
              false, 0xE72C },
            { L"删除指定账户",
              L"将永久删除目标框指定的本地账户及其配置文件关联（账户本身不可恢复）。\n\n是否继续？",
              true, 0xE74D },   // Delete（危险态）
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)tags;
        const wstring user = target_;
        switch (actionIndex) {
        case 0: {
            wstring pwd, desc;
            SplitPath(pwd, desc);
            wstring body = L"将创建/更新本地账户「" + user + L"」\n";
            if (pwd.empty())
                body += L"密码：留空 → 无密码账户（自动设置 UF_PASSWD_NOTREQD|"
                        L"UF_DONT_EXPIRE_PASSWD）\n"
                        L"注意：空密码账户不能用于共享访问/网络登录（LSA "
                        L"LimitBlankPasswordUse 默认限制），如需共享请设置密码。\n";
            else
                body += L"密码：已提供（不回显）\n";
            if (!desc.empty()) body += L"描述：" + desc + L"\n";
            body += L"\n是否继续？";
            return body;
        }
        case 1: {
            wstring pwd, desc;
            SplitPath(pwd, desc);
            (void)desc;
            wstring body = L"将配置开机自动登录：\n"
                           L"  账户：" + (user.empty() ? wstring(L"(未填写)") : user) +
                           L"\n  密码：" + (pwd.empty() ? wstring(L"(空)") : L"(已提供，不回显)") +
                           L"\n\n⚠ 风险提示：\n"
                           L"  1. 密码将以【明文】写入注册表 Winlogon\\DefaultPassword，"
                           L"本机任何可读注册表的程序/用户都能读到；\n"
                           L"  2. 开机免密直接进入桌面，物理接触本机的人可直接使用该账户；\n"
                           L"  3. 更安全的做法：用微软 Sysinternals Autologon 工具，"
                           L"它把密码加密存入 LSA secret，不在注册表留明文。\n\n是否继续？";
            return body;
        }
        case 3: {
            wstring body = L"将删除本地账户「" + user + L"」：\n"
                           L"  · 账户不可恢复（内置账户 Administrator/Guest/DefaultAccount "
                           L"会被硬性拦截）\n"
                           L"  · 该账户的注册表配置文件不会随删除清理，如需彻底清理请删除后"
                           L"再用「系统属性→高级→用户配置文件」清理\n\n是否继续？";
            return body;
        }
        default:
            return L"";   // 动作 2 走 Actions() 里的静态 warning
        }
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        if (actionIndex < 0 || actionIndex >= (int)acts.size()) return;
        const wstring label = acts[actionIndex].label;
        // 动作 2「关闭自动登录」虽是写操作，但属风险回收（删明文密码），非管理员
        // 同样拒绝以保持一致性；全部动作要求提权
        if (!RequireElevation(ctx, label, out)) return;

        switch (actionIndex) {
        case 0: DoCreateOrUpdate(ctx, out); break;
        case 1: DoConfigAutologon(ctx, out); break;
        case 2: DoDisableAutologon(ctx, out); break;
        case 3: DoDeleteUser(ctx, out); break;
        }
        out.canClean = true;
    }

private:
    wstring path_, target_;

    // 路径框解析：「描述|密码」两段式或纯密码。竖线前=描述，竖线后=密码。
    void SplitPath(wstring& pwd, wstring& desc) const {
        size_t bar = path_.find(L'|');
        if (bar == wstring::npos) {
            pwd = path_;
            desc.clear();
        } else {
            desc = path_.substr(0, bar);
            pwd = path_.substr(bar + 1);
        }
    }

    bool AccountExists(const wstring& name, wstring* err) const {
        USER_INFO_0* u0 = nullptr;
        NET_API_STATUS st = NetUserGetInfo(nullptr, name.c_str(), 0, (LPBYTE*)&u0);
        if (u0) NetApiBufferFree(u0);
        if (st == NERR_Success) return true;
        if (st == NERR_UserNotFound) return false;
        if (err) *err = NerrTextZh(st);
        return false;
    }

    void DoCreateOrUpdate(ModuleContext& ctx, ModuleResult& out) {
        wstring name = target_;
        wstring v = ValidateUserName(name);
        if (!v.empty()) {
            out.summary = L"创建/更新账户：参数无效 — " + v;
            out.canClean = true;
            ctx.Log(L"  拒绝: " + v);
            return;
        }
        wstring pwd, desc;
        SplitPath(pwd, desc);

        wstring existErr;
        bool exists = AccountExists(name, &existErr);
        if (!exists && !existErr.empty()) {
            out.summary = L"查询账户失败：" + existErr;
            ctx.Log(L"  失败: NetUserGetInfo — " + existErr);
            return;
        }

        if (exists) {
            // 已有同名账户：提供了密码 → 改密（level 1003）；未提供 → 不动
            if (pwd.empty()) {
                out.summary = L"账户「" + name + L"」已存在，未提供密码，未做修改";
                ctx.Log(L"  跳过: " + name + L" 已存在（路径框留空不改密）");
                return;
            }
            USER_INFO_1003 ui = {};
            ui.usri1003_password = const_cast<wchar_t*>(pwd.c_str());
            DWORD parmErr = 0;
            NET_API_STATUS st = NetUserSetInfo(nullptr, name.c_str(), 1003,
                                               (LPBYTE)&ui, &parmErr);
            if (st == NERR_Success) {
                out.summary = L"账户「" + name + L"」密码已更新";
                ctx.Log(L"  已更新密码: " + name + L"（密码值不回显）");
            } else {
                out.summary = L"改密失败：" + ApiErrTextZh(st);
                ctx.Log(L"  失败: NetUserSetInfo(1003) — " + ApiErrTextZh(st));
            }
            return;
        }

        // 新建：NetUserAdd level 1。priv 必须显式 USER_PRIV_USER；
        // 空密码 → UF_PASSWD_NOTREQD|UF_DONT_EXPIRE_PASSWD（否则策略直接拒绝）
        USER_INFO_1 ui = {};
        // USER_INFO_1 字段为非 const LPWSTR：NetUserAdd 仅在调用期读取，const_cast 安全
        ui.usri1_name = const_cast<wchar_t*>(name.c_str());
        ui.usri1_password = const_cast<wchar_t*>(pwd.c_str());
        ui.usri1_priv = USER_PRIV_USER;
        ui.usri1_home_dir = nullptr;
        static wchar_t kDefaultComment[] = L"由网管工具箱创建";
        ui.usri1_comment = desc.empty() ? kDefaultComment
                                        : const_cast<wchar_t*>(desc.c_str());
        ui.usri1_flags = pwd.empty()
                             ? (UF_PASSWD_NOTREQD | UF_DONT_EXPIRE_PASSWD)
                             : UF_DONT_EXPIRE_PASSWD;
        ui.usri1_script_path = nullptr;
        DWORD parmErr = 0;
        NET_API_STATUS st = NetUserAdd(nullptr, 1, (LPBYTE)&ui, &parmErr);
        if (st == NERR_Success) {
            out.summary = L"账户「" + name + L"」创建成功" +
                          (pwd.empty() ? wstring(L"（无密码）") : wstring());
            ctx.Log(L"  已创建: " + name +
                    (pwd.empty() ? L"（空密码：UF_PASSWD_NOTREQD|UF_DONT_EXPIRE_PASSWD）"
                                 : L"（密码已设置，不回显）"));
            if (pwd.empty()) {
                out.hint = wstring(T(L"空密码账户不能用于共享访问/网络登录（LSA "))
                           + T(L"LimitBlankPasswordUse 默认限制），如需共享请设置密码。");
                ctx.Log(L"  提示: 空密码账户受 LSA LimitBlankPasswordUse 限制，"
                        L"不能用于共享访问；交互登录不受影响");
            }
        } else {
            out.summary = L"创建账户失败：" + ApiErrTextZh(st);
            ctx.Log(L"  失败: NetUserAdd(1) — " + ApiErrTextZh(st) +
                    (parmErr ? (L"（参数下标 " + I64(parmErr) + L"）") : wstring()));
        }
    }

    void DoConfigAutologon(ModuleContext& ctx, ModuleResult& out) {
        wstring name = target_;
        if (name.empty()) {
            out.summary = L"配置自动登录：目标框用户名为空";
            ctx.Log(L"  拒绝: 未填写自动登录账户名");
            return;
        }
        wstring existErr;
        if (!AccountExists(name, &existErr)) {
            if (!existErr.empty()) {
                out.summary = L"查询账户失败：" + existErr;
                return;
            }
            out.summary = L"账户「" + name + L"」不存在，请先创建（动作①）或改正用户名";
            ctx.Log(L"  拒绝: 自动登录账户 " + name + L" 不存在");
            return;
        }
        wstring pwd, desc;
        SplitPath(pwd, desc);   // 自动登录密码取路径框密码段
        (void)desc;

        wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD cl = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(comp, &cl);

        LONG r1 = RegWriteStr(HKEY_LOCAL_MACHINE, kWinlogon, L"AutoAdminLogon", L"1");
        LONG r2 = RegWriteStr(HKEY_LOCAL_MACHINE, kWinlogon, L"DefaultUserName", name);
        LONG r3 = RegWriteStr(HKEY_LOCAL_MACHINE, kWinlogon, L"DefaultPassword", pwd);
        LONG r4 = RegWriteStr(HKEY_LOCAL_MACHINE, kWinlogon, L"DefaultDomainName", comp);
        if (r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS &&
            r3 == ERROR_SUCCESS && r4 == ERROR_SUCCESS) {
            out.summary = L"自动登录已配置：账户 " + name +
                          L"（密码" + (pwd.empty() ? L"为空" : L"已写入(明文)") +
                          L"，重启后生效）";
            ctx.Log(L"  已写入 Winlogon：AutoAdminLogon=1, DefaultUserName=" + name +
                    L", DefaultPassword=" + (pwd.empty() ? wstring(L"(空)") : L"(已设置，不回显)") +
                    L", DefaultDomainName=" + comp);
            ctx.Log(L"  ⚠ DefaultPassword 为注册表明文，建议改用 Sysinternals "
                    L"Autologon（存 LSA secret）；关闭用「更多 ▾ → 关闭自动登录」");
        } else {
            out.summary = L"写入 Winlogon 失败：" + ErrorTextZh((DWORD)(r1 != ERROR_SUCCESS ? r1 :
                          r2 != ERROR_SUCCESS ? r2 : r3 != ERROR_SUCCESS ? r3 : r4));
            ctx.Log(L"  失败: RegSetValue — 请确认以管理员身份运行");
        }
    }

    void DoDisableAutologon(ModuleContext& ctx, ModuleResult& out) {
        wstring aal;
        bool hasAal = RegReadStr(HKEY_LOCAL_MACHINE, kWinlogon, L"AutoAdminLogon", aal);
        LONG r1 = RegDelValue(HKEY_LOCAL_MACHINE, kWinlogon, L"AutoAdminLogon");
        LONG r2 = RegDelValue(HKEY_LOCAL_MACHINE, kWinlogon, L"DefaultPassword");
        if (r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS) {
            if (!hasAal)
                out.summary = L"自动登录本就未配置（相关键已确保清理）";
            else
                out.summary = L"自动登录已关闭（AutoAdminLogon/DefaultPassword 已删除，"
                              L"DefaultUserName 保留），重启后生效";
            ctx.Log(L"  已删除 Winlogon\\AutoAdminLogon 与 DefaultPassword"
                    L"（DefaultUserName 保留）");
        } else {
            out.summary = L"清理 Winlogon 失败：" + ErrorTextZh((DWORD)(r1 != ERROR_SUCCESS ? r1 : r2));
            ctx.Log(L"  失败: RegDeleteValue — 请确认以管理员身份运行");
        }
    }

    void DoDeleteUser(ModuleContext& ctx, ModuleResult& out) {
        wstring name = target_;
        wstring v = ValidateUserName(name);
        if (!v.empty()) {
            out.summary = L"删除账户：参数无效 — " + v;
            ctx.Log(L"  拒绝: " + v);
            return;
        }
        if (IsBuiltinAccount(name)) {
            out.summary = L"拒绝删除：" + name + L" 为系统内置账户";
            ctx.Log(L"  拒绝: " + name + L" — 内置账户硬拦截，不允许删除");
            return;
        }
        NET_API_STATUS st = NetUserDel(nullptr, name.c_str());
        if (st == NERR_Success) {
            out.summary = L"账户「" + name + L"」已删除";
            ctx.Log(L"  已删除: " + name);
            // 删除的是自动登录账户时给出联动提示
            wstring dun;
            if (RegReadStr(HKEY_LOCAL_MACHINE, kWinlogon, L"DefaultUserName", dun) &&
                _wcsicmp(dun.c_str(), name.c_str()) == 0) {
                out.hint = wstring(T(L"注意：删除的账户正是自动登录账户，建议用「更多 ▾ → 关闭自动登录」"))
                           + T(L"清理残留配置，否则开机可能卡在登录界面。");
                ctx.Log(L"  ⚠ 已删除账户与 Winlogon\\DefaultUserName 一致，建议关闭自动登录");
            }
        } else {
            out.summary = L"删除账户失败：" + ApiErrTextZh(st);
            ctx.Log(L"  失败: NetUserDel — " + ApiErrTextZh(st));
        }
    }
};

// ========================================================== 2) sys.domain ===

class SysDomainModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.domain"; }
    const wchar_t* Group() const override { return L"系统管理"; }
    const wchar_t* Title() const override { return L"计算机名与域"; }
    const wchar_t* Description() const override {
        return L"重命名计算机 / 加域 / 退域回工作组（重启后生效，绝不代重启）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsElevation() const override { return true; }
    bool NeedsTarget() const override { return true; }
    bool NeedsPath() const override { return true; }
    const wchar_t* TargetHint() const override {
        return L"新计算机名 / 域名[ user:pass] / 退域凭据 user:pass";
    }
    const wchar_t* PathHint() const override { return L"加域 OU（可选，域管可控时才有效）"; }

    // ---- 结构化扩展字段（v2.12 R3）：替代过去把凭据挤在一个输入框的写法 ----
    vector<ModuleField> Fields() const override {
        return {
            { L"域管理员用户名", L"如 administrator 或 CORP\\admin（留空 = 使用当前用户）", false },
            { L"域管理员密码",   L"加域 / 退域凭据；仅本次操作在内存中使用，DPAPI 加密暂存", true },
            { L"计算机名前缀",   L"填写则自动生成：前缀-日期-4位随机数（如 PC-20261024-4821）", false },
            { L"工作组名",       L"退域后加入的工作组（留空 = WORKGROUP）", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        auto pick = [&](size_t i) -> wstring {
            return (i < values.size()) ? values[i] : wstring();
        };
        fUser_ = pick(0);
        fPass_ = pick(1);
        wstring prefix = pick(2);
        fWorkgroup_ = pick(3);
        // 前缀值变化 → 重新生成自动名（确认框与实际执行共用同一个名字，
        // 避免「确认框显示 A、实际改成 B」）；同前缀重复注入复用缓存
        if (prefix != genPrefix_) {
            genPrefix_ = prefix;
            genName_ = prefix.empty() ? wstring() : MakeAutoName(prefix);
        }
    }

    void SetInput(const wstring& path, const wstring& target) override {
        path_ = path;
        target_ = target;
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        wchar_t buf[256] = {};

        // DNS 主机名（重命名目标）
        wstring dnsHost = L"(读取失败)";
        DWORD sz = 256;
        if (GetComputerNameExW(ComputerNameDnsHostname, buf, &sz))
            dnsHost = buf;
        // NetBIOS 名（≤15，系统自动派生）
        wstring nbName = L"(读取失败)";
        sz = 256;
        if (GetComputerNameExW(ComputerNamePhysicalNetBIOS, buf, &sz))
            nbName = buf;
        // 域（DnsDomain 非空 = 已加域）
        wstring dnsDomain;
        sz = 256;
        bool joined = GetComputerNameExW(ComputerNamePhysicalDnsDomain, buf, &sz) && buf[0];
        if (joined) dnsDomain = buf;
        // 工作组名（未加域时读 lanman 工作站信息）
        wstring langroup;
        if (!joined) {
            WKSTA_INFO_100* wk = nullptr;
            if (NetWkstaGetInfo(nullptr, 100, (LPBYTE*)&wk) == NERR_Success && wk) {
                if (wk->wki100_langroup) langroup = wk->wki100_langroup;
                NetApiBufferFree(wk);
            }
        }

        auto row = [&out](const wstring& c1, const wstring& c2,
                          const wstring& c3, const wstring& c4) {
            ModuleRow r;
            r.c1 = c1; r.c2 = c2; r.c3 = c3; r.c4 = c4;
            out.rows.push_back(r);
        };
        row(L"DNS 主机名", dnsHost, L"标识", L"重命名目标（ComputerNameDnsHostname，≤63 字符）");
        row(L"NetBIOS 名", nbName, L"标识", L"由 DNS 名自动派生，≤15 字符（旧协议兼容）");
        if (joined) {
            row(L"域", dnsDomain, L"域加入", L"已加域（退域回工作组需管理员凭据，重启生效）");
        } else {
            row(L"工作组", langroup.empty() ? L"WORKGROUP" : langroup, L"域加入",
                L"未加域（加域需域名+有权限的域账户凭据，重启生效）");
        }

        out.summary = (joined ? L"已加域：" + dnsDomain
                              : L"工作组：" + (langroup.empty() ? wstring(L"WORKGROUP") : langroup)) +
                      L" · 主机名 " + dnsHost;
        out.hint = wstring(T(L"结构化输入：①重命名=名称框或「计算机名前缀」自动生成 ②加域=目标框填域名 + "))
                   + wstring(T(L"下方「域管理员用户名/密码」字段（路径框=OU 可选）；"))
                   + T(L"「更多 ▾」= 退域回工作组（凭据同上，工作组名字段生效）/ 查看当前状态。");
        out.canClean = true;
        ctx.Report(100, L"计算机标识读取完成");
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"重命名计算机", nullptr, true, 0xE8AC },   // Rename（危险态：域内配置文件风险）
            { L"加入域", nullptr, true, 0xE713 },          // Settings（危险态）
            { L"退域回工作组",
              L"将退出当前域并回到工作组（NETSETUP_ACCT_DELETE 同时删除域侧计算机账户）：\n"
              L"  · 域 profile 将无法再加载，桌面/文档看似「丢文件」（实为切到本地 profile）\n"
              L"  · 退域前必须确保本地管理员密码已知，否则可能无法登录\n"
              L"  · 完成后需重启生效\n\n是否继续？",
              true, 0xE72E },   // BlockContact（危险态）
            { L"查看当前状态",
              L"重新读取计算机名与域/工作组状态（只读，不修改任何设置）。",
              false, 0xE946 },
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)tags;
        switch (actionIndex) {
        case 0: {
            // v2.12 R3：填了「计算机名前缀」则用自动生成的名字（与执行共用缓存）
            const wstring name = genName_.empty() ? target_ : genName_;
            wstring v = ValidateHostName(name);
            wstring body = L"将把计算机名改为「" + name + L"」";
            if (!genName_.empty())
                body += L"（按前缀自动生成：前缀-日期-4位随机数）";
            body += L"\n  · 重启后生效（本工具绝不代重启）\n";
            if (!v.empty()) body += L"  · ⚠ 名称校验未通过：" + v + L"\n";
            body += L"  · 若本机已加域，改名后需同步在域控制器更新计算机账户，"
                    L"且域 profile 可能无法加载（桌面/文档看似丢失，实为切换 profile）\n\n"
                    L"是否继续？";
            return body;
        }
        case 1: {
            wstring domain = target_, u, p;
            size_t sp = domain.find(L' ');
            if (sp != wstring::npos) domain = domain.substr(0, sp);
            JoinCreds(u, p);
            wstring body = L"将把本机加入域「" + domain + L"」\n"
                           L"  · 凭据：" + (u.empty() ? wstring(L"使用当前用户")
                                                     : L"已提供（不回显）") +
                           L"\n  · 将在域中创建计算机账户（NETSETUP_ACCT_CREATE）\n"
                           L"  · ⚠ 完成后需重启生效；域环境将加载域 profile，"
                           L"首次登录桌面/开始菜单与现有关，请提前导出个人文件\n"
                           L"  · ⚠ 输错域名或反复用错误凭据重试会积累坏计算机账户\n\n是否继续？";
            return body;
        }
        default:
            return L"";   // 动作 2/3 走 Actions() 静态 warning
        }
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        auto acts = Actions();
        if (actionIndex < 0 || actionIndex >= (int)acts.size()) return;
        const wstring label = acts[actionIndex].label;
        if (actionIndex != 3 && !RequireElevation(ctx, label, out)) return;

        switch (actionIndex) {
        case 0: DoRename(ctx, out); break;
        case 1: DoJoinDomain(ctx, out); break;
        case 2: DoUnjoinDomain(ctx, out); break;
        case 3: Run(ctx, out); break;   // 只读刷新
        }
        out.canClean = true;
    }

private:
    wstring path_, target_;
    int joinFailStreak_ = 0;   // 连续加域失败计数（≥2 即停，防计算机账户锁死）

    // v2.12 R3 扩展字段缓存（SetFields 注入）
    wstring fUser_, fPass_, fWorkgroup_;
    wstring genPrefix_, genName_;   // 自动生成的计算机名（确认/执行共用）

    // 自动计算机名：前缀（清洗非法字符）-YYYYMMDD-4位随机数
    // 如前缀 "PC" → PC-20261024-4821（≤63 字符合规，净名 ≤ 5+1+8+1+4）
    static wstring MakeAutoName(const wstring& prefix) {
        wstring p;
        for (wchar_t c : prefix) {
            bool alnum = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                         (c >= L'A' && c <= L'Z');
            if (alnum) p += c;
            else if (c == L'-' || c == L'_' || c == L'.' || c == L' ') p += L'-';
            // 其余字符丢弃
        }
        while (!p.empty() && p.front() == L'-') p.erase(p.begin());
        while (!p.empty() && p.back() == L'-') p.pop_back();
        if (p.empty()) p = L"PC";
        SYSTEMTIME st;
        GetLocalTime(&st);
        static std::mt19937 rng((unsigned)std::chrono::steady_clock::now()
                                .time_since_epoch().count());
        static std::uniform_int_distribution<int> dist(0, 9999);
        wchar_t tail[16];
        wsprintfW(tail, L"%04u", (unsigned)dist(rng));
        wchar_t date[16];
        wsprintfW(date, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
        return p + L"-" + date + L"-" + tail;
    }

    // 凭据解析（v2.12 R3）：优先结构化字段；字段为空回落旧版「user:pass」行内格式
    void JoinCreds(wstring& user, wstring& pass) const {
        user = fUser_;
        pass = fPass_;
        if (!user.empty() || !pass.empty()) return;
        wstring dom, u, p;
        SplitJoinTarget(dom, u, p);
        user = u;
        pass = p;
    }

    // 计算机名合法性：1..63 字符；仅字母/数字/连字符；不以连字符开头或结尾；
    // 不为纯数字（防 NetBIOS 派生歧义）
    static wstring ValidateHostName(const wstring& name) {
        if (name.empty()) return L"名称不能为空";
        if (name.size() > 63) return L"名称超长（DNS 主机名限制 ≤63 字符，"
                                  L"超长部分不会自动截派 NetBIOS 名）";
        if (name[0] == L'-' || name[name.size() - 1] == L'-')
            return L"名称不能以连字符开头或结尾";
        bool allDigit = true;
        for (wchar_t c : name) {
            bool alnum = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                         (c >= L'A' && c <= L'Z');
            if (!alnum && c != L'-')
                return L"名称只能包含字母、数字和连字符（非法字符：" +
                       wstring(1, c) + L"）";
            if (!(c >= L'0' && c <= L'9')) allDigit = false;
        }
        if (allDigit) return L"名称不能为纯数字（避免 NetBIOS 派生歧义）";
        return L"";
    }

    // 加域目标解析：`域名` 或 `域名 user:pass`（空格分隔凭据段）
    void SplitJoinTarget(wstring& domain, wstring& user, wstring& pass) const {
        domain = target_;
        user.clear();
        pass.clear();
        size_t sp = target_.find(L' ');
        if (sp != wstring::npos) {
            domain = target_.substr(0, sp);
            wstring cred = target_.substr(sp + 1);
            size_t colon = cred.find(L':');
            if (colon != wstring::npos) {
                user = cred.substr(0, colon);
                pass = cred.substr(colon + 1);
            }
        }
    }

    // 退域凭据解析：`user:pass`（可空 = 当前用户）
    void SplitCred(wstring& user, wstring& pass) const {
        user.clear();
        pass.clear();
        if (target_.empty()) return;
        size_t colon = target_.find(L':');
        if (colon != wstring::npos) {
            user = target_.substr(0, colon);
            pass = target_.substr(colon + 1);
        } else {
            user = target_;   // 只有用户名 → 密码留空（交互场景少见，如实透传）
        }
    }

    bool CurrentlyJoined(wstring& domain) const {
        wchar_t buf[256] = {};
        DWORD sz = 256;
        if (GetComputerNameExW(ComputerNamePhysicalDnsDomain, buf, &sz) && buf[0]) {
            domain = buf;
            return true;
        }
        return false;
    }

    void DoRename(ModuleContext& ctx, ModuleResult& out) {
        // v2.12 R3：填了「计算机名前缀」→ 自动名优先；否则用名称框手填
        wstring name;
        if (!genName_.empty()) {
            name = genName_;
            ctx.Log(L"  自动名: 前缀「" + genPrefix_ + L"」→ " + name +
                    L"（前缀-日期-4位随机数）");
        } else {
            name = target_;
        }
        wstring v = ValidateHostName(name);
        if (!v.empty()) {
            out.summary = L"重命名计算机：名称校验未通过 — " + v;
            ctx.Log(L"  拒绝: " + v + L"（未调用系统 API）");
            return;
        }
        wstring curDomain;
        bool joined = CurrentlyJoined(curDomain);
        ctx.Log(L"  $ SetComputerNameExW(DnsHostname, " + name + L")");
        if (!SetComputerNameExW(ComputerNamePhysicalDnsHostname, name.c_str())) {
            DWORD e = GetLastError();
            out.summary = L"重命名失败：" + Win32ErrZh(e);
            ctx.Log(L"  失败: " + Win32ErrZh(e));
            return;
        }
        // 回读确认（重命名提交后新名立即可见，重启后完全生效）
        wchar_t buf[256] = {};
        DWORD sz = 256;
        wstring readback = L"(回读失败)";
        if (GetComputerNameExW(ComputerNameDnsHostname, buf, &sz))
            readback = buf;
        out.summary = L"重命名已提交：" + readback + L"（重启后完全生效）";
        ctx.Log(L"  已提交: 新 DNS 主机名回读 = " + readback);
        ctx.Log(L"  提示: 重启后生效，请自行重启（本工具不代重启）");
        out.hint = joined
            ? wstring(T(L"本机已加域（")) + curDomain +
              wstring(T(L"）：改名后建议在域侧同步计算机账户；域 profile 可能无法加载，"))
              + T(L"重启前请确认本地管理员密码已知。")
            : T(L"重启后生效，请自行重启。");
    }

    void DoJoinDomain(ModuleContext& ctx, ModuleResult& out) {
        // v2.12 R3：域名取目标框（空格前的部分）；凭据走结构化字段，空则回落旧行内格式
        wstring domain = target_, user, pass;
        size_t sp = domain.find(L' ');
        if (sp != wstring::npos) domain = domain.substr(0, sp);
        JoinCreds(user, pass);
        if (domain.empty()) {
            out.summary = L"加入域：目标框域名为空";
            ctx.Log(L"  拒绝: 未填写域名");
            return;
        }
        wstring cur;
        if (CurrentlyJoined(cur)) {
            out.summary = L"已加入域「" + cur + L"」，如需换域请先「退域回工作组」";
            ctx.Log(L"  拦截: 本机已加域 " + cur + L"（防误操作，未调用 NetJoinDomain）");
            return;
        }
        // 防计算机账户锁死：连续失败 ≥2 次即停，给手工指引（风险登记册 #1）
        if (joinFailStreak_ >= 2) {
            out.summary = L"已连续失败 " + I64(joinFailStreak_) +
                          L" 次，为防计算机账户锁定已停止重试";
            out.hint = wstring(T(L"请核对：①域名拼写 ②DNS 能否解析域 ③凭据是否为有加域权限的域账户"))
                       + wstring(T(L"（格式：域名 user:pass）。可先用「网络诊断」排查 DNS，"))
                       + T(L"或联系域管理员在 AD 中清理本机坏账户后再试。");
            ctx.Log(L"  停止: 连续失败达 2 次，不再自动重试");
            return;
        }

        wstring ou = path_;   // OU 可选，仅域管可控时有效
        ctx.Log(L"  $ NetJoinDomain(domain=" + domain +
                (ou.empty() ? wstring() : L", ou=" + ou) +
                L", user=" + (user.empty() ? wstring(L"(当前用户)") : L"(已提供)") +
                L", options=JOIN_DOMAIN|ACCT_CREATE)");
        DWORD opts = NETSETUP_JOIN_DOMAIN | NETSETUP_ACCT_CREATE;
        NET_API_STATUS st = NetJoinDomain(nullptr, domain.c_str(),
                                          ou.empty() ? nullptr : ou.c_str(),
                                          user.empty() ? nullptr : user.c_str(),
                                          pass.empty() ? nullptr : pass.c_str(),
                                          opts);
        if (st == NERR_Success) {
            joinFailStreak_ = 0;
            out.summary = L"加域成功：已加入「" + domain + L"」（重启后生效）";
            ctx.Log(L"  成功: 已加入域 " + domain);
            ctx.Log(L"  提示: 重启后生效，请自行重启；首次登录将加载域 profile");
        } else {
            ++joinFailStreak_;
            out.summary = L"加域失败（第 " + I64(joinFailStreak_) + L" 次）：" +
                          ApiErrTextZh(st);
            ctx.Log(L"  失败: NetJoinDomain — " + ApiErrTextZh(st));
            if (joinFailStreak_ >= 2)
                ctx.Log(L"  ⚠ 已连续失败 2 次：为防计算机账户锁定，下次将拒绝重试，"
                        L"请先排查域名/凭据或联系域管理员");
        }
    }

    void DoUnjoinDomain(ModuleContext& ctx, ModuleResult& out) {
        wstring cur;
        if (!CurrentlyJoined(cur)) {
            out.summary = L"本机未加入域（已是工作组），无需退域";
            ctx.Log(L"  跳过: 本机未加域");
            return;
        }
        wstring user, pass;
        JoinCreds(user, pass);   // v2.12 R3：结构化字段优先，回落行内 user:pass
        const wstring workgroup = fWorkgroup_.empty() ? wstring(L"WORKGROUP")
                                                      : fWorkgroup_;
        ctx.Log(L"  $ NetUnjoinDomain(domain=" + cur +
                L", user=" + (user.empty() ? wstring(L"(当前用户)") : L"(已提供)") +
                L", options=ACCT_DELETE)");
        NET_API_STATUS st = NetUnjoinDomain(nullptr,
                                            user.empty() ? nullptr : user.c_str(),
                                            pass.empty() ? nullptr : pass.c_str(),
                                            NETSETUP_ACCT_DELETE);
        if (st != NERR_Success) {
            out.summary = L"退域失败：" + ApiErrTextZh(st);
            ctx.Log(L"  失败: NetUnjoinDomain — " + ApiErrTextZh(st));
            ctx.Log(L"  提示: 域侧凭据不足时可用「域管 user:pass」格式重试一次，"
                    L"或联系域管理员手动删除计算机账户");
            return;
        }
        ctx.Log(L"  成功: 已退域（域侧计算机账户已删除）");
        ctx.Log(L"  $ NetJoinDomain(" + workgroup + L", options=0)");
        st = NetJoinDomain(nullptr, workgroup.c_str(), nullptr, nullptr, nullptr, 0);
        if (st == NERR_Success) {
            out.summary = L"退域成功，已加入工作组 " + workgroup + L"（重启后生效）";
            ctx.Log(L"  成功: 已加入工作组 " + workgroup);
        } else {
            out.summary = L"已退域，但加入工作组失败：" + ApiErrTextZh(st) +
                          L"（重启后可在系统属性手动改工作组）";
            ctx.Log(L"  部分完成: 退域成功，JoinDomain(" + workgroup + L") — " +
                    ApiErrTextZh(st));
        }
        ctx.Log(L"  ⚠ 域 profile 将无法再加载；重启前请确认本地管理员密码已知");
        ctx.Log(L"  提示: 重启后生效，请自行重启（本工具不代重启）");
        out.hint = T(L"重启前请确认本地管理员密码已知，否则可能无法登录。");
        joinFailStreak_ = 0;
    }
};

// 自动注册（v2.9 T02：模块文件自动被 build.bat 收编）
REGISTER_MODULE(SysAcctModule);
REGISTER_MODULE(SysDomainModule);

} // namespace
} // namespace mz
