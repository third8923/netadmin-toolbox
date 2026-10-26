// ============================================================================
//  清理引擎 - 规则驱动的磁盘清理与空间分析
//  扫描类: 基础/系统/微信/QQ/大文件/重复/相似/目录大小/软件占用
//  迁移类: 软件搬家 / 用户目录迁移（复制+校验+Junction+清源）
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

struct ScanItem {
    wstring name;
    wstring path;
    unsigned long long size = 0;
    wstring category;
    bool isFile = true;
    wstring risk;       // 空 / 低 / 中 / 高
    wstring note;       // 风险说明
    int registryRefs = 0;
    bool migratable = false;

    // ---- 安全标记（重要）----
    // reportOnly = true 表示「仅统计，禁止本程序直接删除」。
    // 用于 pagefile.sys / hiberfil.sys / WinSxS / Windows.old 等：
    // 它们要么是系统关键文件，要么必须通过 DISM / 磁盘清理 / powercfg 处理。
    // 清理流程必须跳过这类项，只在界面/输出中提示可释放空间与正确处理方式。
    bool reportOnly = false;
    wstring hint;       // reportOnly 项的处理提示（如「请用 DISM 清理组件存储」）
    bool partial = false;  // size 是否因超时而只是部分统计

    // useEmptyRecycleBin = true 表示该项是回收站：清理时走
    // SHEmptyRecycleBinW（走系统回收站语义），而不是暴力删除 $Recycle.Bin
    // 目录内容（后者会破坏回收站元数据，导致「还原」失效）。
    bool useEmptyRecycleBin = false;
};

enum class ScanKind {
    BasicClean, SystemClean, WeChatClean, QQClean,
    LargeFiles, DuplicateFiles, SimilarFiles, DirSizes,
    SoftwareUsage, SoftwareMove, UserDirMigrate
};

struct ScanOptions {
    wstring rootPath = L"C:\\";
    int minSizeMB = 100;
};

struct ScanOutcome {
    vector<ScanItem> items;
    unsigned long long totalSize = 0;
    bool cancelled = false;
    wstring error;
    wstring note;       // 扫描附加说明（去重剪枝说明 / 多用户覆盖范围），上层追加到 summary
};

// 目录大小（递归，跳过链接）
unsigned long long DirSize(const wstring& dir);

// 扫描入口
ScanOutcome RunScan(ScanKind kind, const ScanOptions& opt,
                    const ProgressFn& progress, const CancelFn& cancel);

// 清理：清空目录内容（保留目录本身）。
// v2.8 起内部转发 CleanDirectoryContentsEx（cleanresult.h）：
// 逐文件智能删除 L1~L4 + 顶层危险路径终检，返回「真实释放字节数」，
// 失败原因汇总写入 *err（无失败时为空）。
unsigned long long CleanDirectoryContents(const wstring& dir, const CancelFn& cancel,
                                          wstring* err);
// 清理：删除单个文件
bool DeleteSingleFile(const wstring& file, wstring* err);

// ---- 迁移引擎 ----
struct TransferResult {
    bool success = false;
    unsigned long long movedBytes = 0;
    wstring message;
    bool junctionCreated = false;
};

// 生成并校验迁移计划（不执行）
TransferResult PlanTransfer(const wstring& source, const wstring& target);

// 执行迁移（dryRun=true 只返回计划）
TransferResult ExecuteTransfer(const wstring& source, const wstring& target,
                               bool dryRun, const ProgressFn& progress);

} // namespace mz
