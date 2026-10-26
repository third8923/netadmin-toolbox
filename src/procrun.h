// ============================================================================
//  流式进程执行器（v2.8 R6：安全体检长任务的基础设施）
//  ---------------------------------------------------------------------------
//  为什么不用 common.cpp 的 RunHidden：
//    * RunHidden 是「读完成才 Wait」的单线程模型，且固定 120s 上限、不可取消，
//      撑不住 sfc / DISM（5~30 分钟）量级的长任务，也没有实时日志。
//  本文件提供（对齐 docs/dev-plan-v28.md §2.4）：
//    * 读线程 + 等待双线程模型：读线程持续排空管道（防管道满死锁）并逐行回调；
//      主线程以 150ms 步进轮询 取消 / 超时 / 退出，任一触发即 TerminateProcess
//    * OEM→UTF16 转码（复用 common.cpp 思路，带 UTF-8 BOM 优先）
//    * \r 进度行合并节流（300ms），避免 sfc 的百分比刷新刷爆日志面板
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

struct ProcRunOptions {
    int timeoutMs = 1800000;                       // 默认 30 分钟（sfc/DISM 量级）
    CancelFn cancel;                               // 返回 true → TerminateProcess
    std::function<void(const wstring&)> onLine;    // 逐行实时回调（工作线程上调用）
};

// 隐藏窗口执行命令行，逐行回调输出，返回退出码：
//   -1 = 进程启动失败；-2 = 超时被终止；-3 = 被取消（opt.cancel）终止。
// fullOut（可空）收全量输出（UTF-16，按行合并，进度行取最后一次刷新）。
int RunProcess(const wstring& cmdline, const ProcRunOptions& opt, wstring* fullOut);

} // namespace mz
