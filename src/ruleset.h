// ============================================================================
//  NetAdmin Toolbox — 流氓软件净化器 · 特征库引擎（v2.13 T01）
//  ---------------------------------------------------------------------------
//  职责：
//    * 解析 SoftCnKiller 数据仓库三类特征文件（sign/folder/whitepath，
//      UTF-16 LE + BOM 或 UTF-8/GBK 自动识别）与本项目自有补充规则；
//    * 编译期内嵌 softcn_rules.h 基线（单文件绿色定位）；
//    * 运行期优先加载 exe 同目录 rules\ 下的更新版（在线更新 T05 落地点），
//      rules\ 不存在时从内嵌基线导出，形成"内嵌兜底 + 目录覆盖"双层结构；
//    * 提供大小写不敏感的签名/目录/白名单匹配接口与统计信息。
//  设计原则（对齐规划 docs/dev-plan-purge.md）：
//    * 微软/WHQL 签名默认放行 —— 调用方在匹配前先判定 IsMicrosoftSign；
//    * 白名单优先级最高 —— IsWhitelisted 命中即跳过一切黑名单匹配；
//    * 特征内容不改一字，致谢来源（作者：四海一叶秋）。
// ============================================================================
#pragma once

#include "common.h"

namespace mz {
namespace purg {

// 特征类别（处置/展示用）
enum class HitKind { None, Sign, Folder };

// 一条命中记录
struct Hit {
    HitKind kind = HitKind::None;
    wstring rule;       // 命中的特征原文
    wstring evidence;   // 匹配依据（签名主体名 / 完整路径）
    bool ok = false;
};

class Ruleset {
public:
    // 加载顺序：exe\rules\ 下 sign*.txt / folder*.txt / white*.txt /
    // extra_sign.txt / extra_folder.txt（自有补充）→ 与内嵌基线合并。
    // rules\ 不存在时自动从内嵌基线导出三份标准文件再加载。
    // 返回 false 表示连内嵌基线都不可用（理论不可能，防御性）。
    bool Load(wstring* stat = nullptr);

    // ---- 匹配接口（全部大小写不敏感）----
    // 签名主体名（证书 O/CN 拼接串）与黑名单双向子串匹配（≥4 字符防止超短误报）
    Hit MatchSign(const wstring& signerName) const;
    // 完整路径与目录特征匹配（特征 \xxx\ 为路径子串）
    Hit MatchPath(const wstring& fullPath) const;
    // 白名单命中（路径含白名单片段即视为安全，不再进入任何黑名单匹配）
    bool IsWhitelisted(const wstring& fullPath) const;
    // 微软签名主体判定（调用方放行：不列报、不处置）
    static bool IsMicrosoftSign(const wstring& signerName);

    bool Ready() const { return !signs_.empty() || !folders_.empty(); }
    wstring Stats() const;

private:
    vector<wstring> signs_;       // 签名主体黑名单（原文）
    vector<wstring> signsLow_;    // 对应小写副本（匹配加速）
    vector<wstring> folders_;
    vector<wstring> foldersLow_;
    vector<wstring> whitePaths_;
    vector<wstring> whiteLow_;

    void AddSign(const wstring& line);
    void AddFolder(const wstring& line);
    void AddWhite(const wstring& line);
    // 单文件解析：自动识别 UTF-16 LE(BOM) / UTF-8(BOM) / GBK
    size_t ParseFile(const wstring& path, bool sign, bool folder, bool white);
};

// exe 同目录 rules\ 子目录（规范化完整路径；GetModuleFileNameW 失败时退当前目录）
wstring RulesDir();
// 处置备份文件路径（exe 同目录 purg-backup.tsv）
wstring BackupFile();
// 浏览器扩展等隔离目录（exe 同目录 purg-quarantine\，处置时改名移入，可逆）
wstring QuarantineDir();

// 编码无关文本行读取工具（ruleset.cpp 实现，在线更新落盘后复验也用它）
// 返回剥离 BOM 并统一为 \n 后的原始文本
wstring ReadTextSmart(const wstring& path, bool* ok = nullptr);

} // namespace purg
} // namespace mz
