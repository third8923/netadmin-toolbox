// ============================================================================
//  Windows 优化模块 - 注册表特性开关（可应用 / 可还原）
//  40 个高频特性，内嵌 .reg 内容，支持应用 / 还原 / 状态检测
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

struct DebloatFeature {
    wstring id;
    wstring category;      // 中文分类
    wstring categoryEn;
    wstring label;         // 中文标签
    wstring labelEn;
    wstring tip;           // 中文说明
    wstring applyKey;      // reg blob key
    wstring undoKey;
    wstring applyText;
    wstring undoLabel;
    bool needsExplorer = false;
    bool needsReboot = false;
};

// 特性表（来自内嵌 TSV，首次调用时解析）
const vector<DebloatFeature>& Features();
// 按分类分组（保持顺序）
vector<std::pair<wstring, vector<const DebloatFeature*>>> FeaturesByCategory();

struct ApplyOutcome {
    bool success = false;
    int valuesSet = 0;
    int errors = 0;
    wstring message;
    bool needsExplorer = false;
    bool needsReboot = false;
};

// 应用 / 还原单个特性
ApplyOutcome ApplyFeature(const DebloatFeature& f, bool undo, const LogSink& log);

// 批量应用：返回成功数量
struct BatchOutcome {
    int ok = 0;
    int failed = 0;
    bool needExplorer = false;
    vector<wstring> failedIds;
};
BatchOutcome ApplyFeatures(const vector<const DebloatFeature*>& feats, bool undo,
                           const LogSink& log, const ProgressFn& progress,
                           const CancelFn& cancel);

// 获取 reg 内容
bool GetRegBlob(const wstring& key, vector<unsigned char>* out);

// 状态检测：根据 apply/undo 的注册表内容反查当前值是否已是目标值
// 返回 true=已应用
bool IsFeatureApplied(const DebloatFeature& f);

} // namespace mz
