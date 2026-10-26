// ============================================================================
//  NetAdmin Toolbox — YARA 子集扫描引擎（v2.14 M5，零第三方库自研）
//  ---------------------------------------------------------------------------
//  为什么自研：libyara 引入会破坏「单文件免安装零依赖」定位；实际查杀场景
//  只需要 YARA 规则的最常用子集。本引擎支持：
//    字符串：  "文本" ascii wide nocase fullword
//    十六进制：{ 4D 5A ?? 90 [4] [2-6] EB FE }（? 通配、[n]/[n-m]/[n-] 跳转）
//    条件：    $a / and / or / not / ( ) / true / false /
//              any of them / all of them / N of them / N of ($a,$b*…) /
//              filesize <op> N [KB|MB] / filesize
//  不支持（解析即跳过该规则并记 error）：正则 /…/、xor/base64 修饰、
//  pe/elf/math/import 模块、at/in/for。规则文件编码自适应（UTF-8/UTF-16/GBK，
//  复用 purg::ReadTextSmart），.yar/.yara 后缀。
// ============================================================================
#pragma once

#include "common.h"

namespace mz {
namespace yara {

// 条件 token（词法阶段切好，求值阶段只做递归下降）
struct CondTok {
    wstring t;             // 词素（word / 运算符）
    bool isNum = false;
    long long num = 0;
};

// 单个十六进制 token
struct HexTok {
    int type = 0;      // 0=字节 1=??通配 2=[n]/[n-m]/[n-] 跳转
    BYTE byte = 0;
    ULONG jlo = 0, jhi = 0;   // type=2：jhi==ULONG_MAX 表示无上界
};

struct YaraString {
    wstring id;                    // $a / $ref1 …
    vector<BYTE> ascii;            // ascii 形态字节（UTF-8 源）
    vector<BYTE> wide;             // wide 形态字节（UTF-16LE，仅 wide 修饰时填充）
    bool nocase = false, fullword = false;
    vector<HexTok> hex;            // 十六进制串（非空时 ascii/wide 无效）
    // ---- 扫描时状态（ScanBuffer 每文件重置）----
    mutable bool matched = false;
    mutable size_t firstOff = 0;
};

struct Rule {
    wstring name;
    wstring tags;
    vector<YaraString> strings;    // 可为空（纯 filesize 条件）
    vector<CondTok> cond;          // 条件 token 流
    wstring error;                 // 非空 = 该规则解析失败（跳过并提示）
};

// 命中记录
struct Hit {
    wstring rule;       // 规则名
    wstring sids;       // 命中的字符串 ID（逗号分隔）
    wstring file;       // 文件路径
    size_t  offset = 0; // 首个命中偏移
};

// 解析规则文本（可含多条 rule，支持 // 与 /* */ 注释）。
// 单条规则失败不致命（记入该 Rule::error 保留条目，扫描时跳过）；
// err 仅在文本完全无法解析时置非空。返回规则条目数。
size_t ParseRules(const wstring& text, vector<Rule>& out, wstring* err);

// 扫描一段内存；命中 push 进 out（每规则最多一条 Hit，含全部命中 sid）。
void ScanBuffer(vector<Rule>& rules, const BYTE* data, size_t size,
                const wstring& label, vector<Hit>& out);

// 扫描路径（文件或目录递归）。单文件 ≤32MB、单次 ≤20000 文件。
// fileCount 回扫过的文件数；*cancelled 由调用方置 true 提前退出。
void ScanPath(vector<Rule>& rules, const wstring& path,
              vector<Hit>& out, int* fileCount, const bool* cancelled,
              wstring* err);

} // namespace yara
} // namespace mz
