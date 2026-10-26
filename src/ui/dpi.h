// ============================================================================
//  DPI 感知与缩放
//  ---------------------------------------------------------------------------
//  * 启动时优先声明 Per-Monitor-V2（Win10 1703+），失败逐级回退
//  * 布局尺寸全部以 96 DPI 为基准编写，经 S() 缩放后使用
//  * 窗口跨屏拖动时响应 WM_DPICHANGED，按建议矩形重排
// ============================================================================
#pragma once

#include "design.h"

namespace mz {
namespace ui {
namespace dpi {

// 进程启动时调用一次（放在窗口创建前）
void EnableAwareness();

// 当前窗口的 DPI（UI 线程调用；未附着窗口时返回进程 DPI）
UINT ForWindow(HWND hwnd);

// 96 基准像素 → 当前 DPI 像素（含用户界面缩放倍率）
int S(int px);
int S(double px);

// 供 WM_DPICHANGED 使用：更新缓存值
void SetCurrent(UINT dpi);

// ---- 用户界面缩放（让用户自己把界面调大，无需改代码重编译）----
//  与 DPI 相乘生效：最终倍率 = dpi/96 × userScale。
//  取值 0.85 ~ 1.80（默认 1.0），字体与全部布局尺寸同步等比缩放。
void SetUserScale(double s);
double UserScale();
void StepUserScale(int dir);        // +1 放大一档 / -1 缩小一档，返回是否变化
void ResetUserScale();              // 回到 1.0

} // namespace dpi
} // namespace ui
} // namespace mz
