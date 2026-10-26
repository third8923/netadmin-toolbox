// ============================================================================
//  NetAdmin Toolbox — 流氓软件净化器 · 查杀引擎（v2.13 T02/T03）
//  ---------------------------------------------------------------------------
//  职责：
//    * 11 类自启/劫持点全量枚举（进程/服务/驱动/启动项/计划任务/右键菜单
//      ShellEx/输入法/文件关联/浏览器扩展/hosts/代理）
//    * Authenticode 数字签名验证（WinVerifyTrust）+ 签名者主体名提取
//      （CryptQueryObject → CMSG_SIGNER_INFO → CertGetNameString），
//      带进程内缓存（同一 exe 多处出现只验证一次）
//    * 四步判定：白名单 → 微软签名放行 → 签名黑名单 → 目录特征
//      （SoftCnKiller 处置语义的匹配核心；微软放行是对 SoftCnKiller
//      把 cmd.exe 计划任务列出来这类误报的关键防线）
//    * 分级处置（全部先备份后动手）：结束进程/禁用服务·驱动/删启动项值/
//      删计划任务/删 ShellEx 注册/删输入法布局/还原关联/隔离扩展目录/
//      重写 hosts/关代理
//    * 处置日志 TSV（purg-backup.tsv）+ 全量还原
//  链接：-lwintrust -lcrypt32 -ladvapi32 -lshell32 -lole32 -loleaut32 -ltaskschd
// ============================================================================
#pragma once

#include "module.h"     // ModuleContext
#include "ruleset.h"

namespace mz {
namespace purg {

// ---------------------------------------------------------------- 发现类型
enum FindingKind {
    F_PROCESS = 0,   // 正在运行的可疑进程
    F_SERVICE,       // 可疑服务
    F_DRIVER,       // 可疑内核驱动（注册表服务枚举，v2.13）
    F_RUN,           // 可疑启动项（Run/RunOnce 注册表值 / 启动文件夹 lnk）
    F_TASK,          // 可疑计划任务
    F_SHELLEX,       // 可疑右键菜单扩展（Approved CLSID）
    F_IME,           // 可疑输入法布局
    F_ASSOC,         // 文件关联劫持
    F_BEXT,          // 可疑浏览器扩展
    F_HOSTS,         // hosts 劫持行
    F_PROXY,         // 系统代理（提示项，可一键关闭）
    F_KMOD,          // 已加载内核模块（v2.14 驱动增强：NT 内核模块表，
                     //   覆盖服务注册表被隐藏/清理 ImagePath 的驱动）
    F_HIDPROC,       // 隐藏进程（v2.14 驱动增强：NT 进程链表可见但
                     //   Toolhelp 快照不可见 —— 疑似 rootkit 摘链）
    F_COUNT
};

const wchar_t* KindName(int kind);       // 中文名：进程/服务/驱动/启动项/…
const wchar_t* KindActionName(int kind); // 处置动作名：结束进程/禁用服务/…

// 一条查杀发现
struct Finding {
    int kind = F_PROCESS;
    wstring name;       // 显示名（进程名/服务名/值名/扩展名…）
    wstring path;       // 关联可执行文件/目录（可空，如代理项）
    wstring location;   // 定位信息（注册表键、任务路径、CLSID、hosts 行号…）
    wstring valName;    // 注册表值名（F_RUN / F_SHELLEX 用）
    wstring hit;        // 命中的特征原文
    wstring hitType;    // L"签名" / L"目录" / L"劫持域名" / L"提示"
    wstring signer;     // 已验证的签名主体（可空）
    wstring extra;      // 处置/还原所需附加数据（任务 XML、原命令行、lnk 原路径…）
    bool advisory = false;   // 仅提示不主动危险（代理等），行不高亮
};

// ---------------------------------------------------------------- 签名验证

struct SignInfo {
    bool present = false;   // 文件有 Authenticode 签名
    bool trusted = false;   // WinVerifyTrust 通过
    wstring subject;        // 签名者主体（O 与 CN 拼接，O 优先展示）
};

// 验证并提取签名（进程内缓存：同路径只验证一次）
SignInfo QuerySign(const wstring& file);

// ---------------------------------------------------------------- 匹配判定

// 四步判定：白名单 → 微软签名放行 → 签名黑名单 → 目录特征。
// 返回命中描述（空串 = 安全不列报）；signerOut 带回签名主体（可空指针）。
// 说明：先验证签名再比对黑名单 —— 目录特征命中但微软签名的文件放行，
// 这是防误报的关键顺序（等价 SoftCnKiller 的签名优先策略）。
wstring CheckFile(const Ruleset& rs, const wstring& exePath, wstring* signerOut = nullptr);

// ---------------------------------------------------------------- 枚举

// 全量枚举 11 类点，逐条 CheckFile，命中的 push 进 out。
// 进度按 11 类均分上报；每类失败（如 COM 不可用）只记日志不中断。
void EnumAll(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out);

// ---------------------------------------------------------------- 处置备份

struct BackupEntry {
    int kind = F_PROCESS;
    wstring item;       // 显示名
    wstring location;   // 定位（键全名/任务路径/文件路径）
    wstring valName;    // 注册表值名
    wstring regType;    // REG_SZ / REG_BINARY(hex) / DWORD(十进制)
    wstring data;       // 原值内容（二进制为 hex）
    wstring extra;      // 附加（任务 XML / 隔离路径 / 备份文件路径）
};

// TSV 字段转义（\ → \\，制表 → \s，换行 → \n 两字面字符）
wstring EscapeField(const wstring& s);
wstring UnescapeField(const wstring& s);

// 追加一批备份记录（含头两行：格式版本 + 本次操作摘要）
bool AppendBackup(const vector<BackupEntry>& entries, const wstring& note, wstring* err);

// 读取全部备份记录（按文件顺序；还原时调用方倒序使用）
vector<BackupEntry> ReadBackupAll();

// ---------------------------------------------------------------- 处置

// 对单条发现执行处置；内部保证先备份后动手。
// 返回空串 = 成功；否则返回错误描述。
wstring ActOnFinding(const Finding& f);

// 全量还原：读取备份文件倒序逐条还原，返回成功条数（err 计失败数）
int UndoAll(ModuleContext& ctx, int* failCount);

} // namespace purg
} // namespace mz
