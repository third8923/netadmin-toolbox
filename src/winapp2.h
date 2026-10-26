// ============================================================================
//  Winapp2.ini 规则引擎
//  ---------------------------------------------------------------------------
//  解析社区维护的 Winapp2.ini（CCleaner 扩展规则格式），展开 FileKey* 中的
//  环境变量 / 目录通配符，统计命中文件总大小（**只统计，不删除**）。
//
//  数据来源：优先读 exe 同目录的 Winapp2.ini；文件不存在则返回空规则集，
//  不报错、不联网。
//
//  格式要点：
//    [Section]                     节名
//    LangSecRef=3021               分类号（人类可读分组可自行映射）
//    Section=...                   短名
//    Default=True|False            缺省 true
//    Warning=...                   存在即视为危险规则，扫描时跳过
//    Detect=HKCU\Software\Foo      注册表/文件存在性检测（多条任一命中即通过）
//    DetectFile=%ProgramFiles%\Foo\foo.exe
//    DetectOS=Windows 10|Windows 11  本阶段一律视为通过（工具只跑 Win7+）
//    FileKey1=路径|文件名通配|标志
//    RegKey1=...                    解析保留，本阶段不应用
//    ExcludeKey1=FILE|路径|通配     排除项
//    Detect+DetectFile+DetectOS 之外的未知键静默忽略。
//
//  安全：展开后的路径若为盘符根、C:\Windows、C:\Program Files、C:\ProgramData、
//  用户目录根等顶层危险位置（或前两层的通配），跳过并通过 progress(-1, ...)
//  回一条警告；$Recycle.Bin / System Volume Information 整个子树跳过。
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

struct Wa2Entry {
    wstring section;       // 节名，如 L"Chrome"
    wstring langSecRef;    // 如 L"3021"（浏览器）
    wstring sectionShort;  // Section= 字段
    bool    isDefault = true;   // Default=True/False，缺省 true
    bool    warning = false;    // Warning= 存在则扫描时跳过（危险项）
    vector<wstring> files;      // FileKey1..N 原始值
    vector<wstring> regs;       // RegKey1..N（本阶段只解析）
    vector<wstring> excludes;   // ExcludeKey1..N 原始值
    vector<wstring> detect;     // Detect= / Detect1..N（注册表路径）
    vector<wstring> detectFile; // DetectFile= / DetectFile1..N（文件路径）
    wstring detectOS;           // DetectOS= 原始值
};

struct Wa2RuleSet {
    vector<Wa2Entry> entries;

    // 从 .ini 加载。支持 UTF-8(含 BOM) / UTF-16LE(含 BOM) / ANSI。
    // 成功返回 true；失败返回 false 并写 *err（err 可为 nullptr）。
    bool LoadFromFile(const wstring& path, wstring* err = nullptr);

    // 从内存缓冲加载（内嵌规则、测试用）。data 可为 UTF-8 或带 BOM 的 UTF-16。
    bool LoadFromMemory(const char* data, size_t len, wstring* err = nullptr);
};

struct Wa2ScanItem {
    wstring section;                 // 分组名（人类可读）
    wstring pattern;                 // 命中的 FileKey 原始值
    unsigned long long size = 0;     // 命中文件总大小（字节）
    vector<wstring> matchedPaths;    // 命中文件完整路径（每项最多保留 10000 条）
};

// 扫描：对每条规则展开环境变量与通配，统计命中文件总大小。
// 不删除任何文件。返回按 size 降序排列的结果（同一 section 可能出现多项）。
// 去重：同一文件被多条规则命中时只计入最先命中的那条，避免总大小重复。
// progress / cancel 均可为空。
vector<Wa2ScanItem> ScanWa2(const Wa2RuleSet& rs,
                            const ProgressFn& progress = nullptr,
                            const CancelFn& cancel = nullptr);

// exe 同目录 Winapp2.ini 的完整路径；文件不存在返回空串。
wstring DefaultWinapp2Path();

// 从 exe 同目录加载 Winapp2.ini。文件不存在 -> 清空规则集并返回 true。
bool LoadWinapp2FromExeDir(Wa2RuleSet& rs);

} // namespace mz
