// ============================================================================
//  品牌绘制层 v2.17「VI-D2 蜕变之翼」
//  ---------------------------------------------------------------------------
//  biohacker.fun VI 手册第 4 节构造参数的 GDI+ 矢量直译：
//    * 蝶形标志 A/B/C/E 四变体（构造参数常量表见 brand.cpp 顶部）
//    * 字标 DrawWordmark（"biohacker" + 金点焊盘 + "fun"，Baloo 2 800）
//    * 走线/焊盘/过孔 电路四元语法（仅 45° 折线，两档线宽）
//    * 空态 / 错误态线稿（不上 err 红——ch_err 只归数据层）
//    * 蜕变四态叙事（卵 v0.1 → 幼虫 v0.5 → 蛹 v0.9 → 蝶 v1.0，仅关于页）
//    * 启动闪屏（分层窗 ≤1.5s 淡出，仅 GUI 分支）
//  纪律：本层取色一律来自传入的 ThemeTokens；禁 skew/渐变/外发光/硬投影
//  （代码中不出现对应 GDI+ 调用即为合规）；金面积 ≤10%。
// ============================================================================
#pragma once

#include "ui/design.h"

namespace mz {
namespace ui {
namespace brand {

// 蝶形变体：
//   FullA    完整（纸底）：wing 上翅 + signal 下翅 + ink 蝶身
//   MonoB    单色墨（纸底单色场景）：全部 ink
//   ReverseC 反白（墨底）：蝶身 paper、上翅 wing_on_ink、焊盘保持金
//            —— C 变体代码路径中不存在 wing 原色（DrawButterfly 内断言）
//   SymbolE  24×24 方瓣圆头符号（图标蓝本，tools/gen_icon.py 同源坐标）
enum class MarkVariant { FullA, MonoB, ReverseC, SymbolE };

// 蝶形：fit 为净空间已含的适配矩形（内部按「画布 240×160」比例收缩净空间）
void DrawButterfly(HDC dc, const RECT& fit, MarkVariant v,
                   const design::ThemeTokens& t);

// 字标："biohacker" + 金点(焊盘) + "fun"，Baloo 2 800；位置大小固定不可拆
void DrawWordmark(HDC dc, const RECT& fit, const design::ThemeTokens& t);

// 走线：仅 45° 折线；unitW 为画布单元宽（3.5u 走线 / 1.5u 标注引线，无第三档）
void DrawTrace(HDC dc, const POINT* pts, int n, double unitW, COLORREF c);

// 焊盘：圆环外径 12u（金）；过孔：实心点 r 2.8u
void DrawPad(HDC dc, POINT center, double unit, COLORREF c);
void DrawVia(HDC dc, POINT center, double unit, COLORREF c);

// 空态：线稿走线 + 未画完的焊盘开口圆弧 + 金 via 一枚 + 引导文案
void DrawEmptyState(HDC dc, const RECT& rc, const design::ThemeTokens& t);

// 错误态：走线在中间断开（两段各留 6u 缺口）+ 1.5u 标注引线指向断点 +
// 说明与行动建议。不上红（ch_err 只归数据层徽标）。
void DrawErrorState(HDC dc, const RECT& rc, const design::ThemeTokens& t);

// 蜕变四态叙事（仅关于页）：卵 v0.1 → 幼虫 v0.5 → 蛹 v0.9 → 蝶 v1.0 四格
// 线稿（仅走线/焊盘/过孔语法），跳变点标 via；当前版本金点=焊盘语义高亮。
void DrawMorphTimeline(HDC dc, const RECT& rc, const design::ThemeTokens& t);

// 启动闪屏：纸底（暗主题=墨底）+ 变体 A/C + 字标 + 金点；
// 300ms 淡入，加载完成或 1.5s 后淡出销毁（非阻塞，内部自管理计时）。
void ShowSplash(HWND owner);

} // namespace brand
} // namespace ui
} // namespace mz
