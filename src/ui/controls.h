// ============================================================================
//  现代控件库（Win32 原生，零第三方依赖）
//  ---------------------------------------------------------------------------
//    * FluentButton  —— 四态（悬停/按下/聚焦/禁用）圆角按钮 + 平滑悬停
//    * SearchBox     —— 暗色搜索框（占位符 + 一键清除）
//    * Toast         —— 右下角浮动通知（淡入淡出 + 自动消失）
//    * 通用资源      —— 全应用共享的字体/图标字体，启动时统一初始化
// ============================================================================
#pragma once

#include "design.h"
#include "gdi.h"

#include <string>
#include <vector>

namespace mz {
namespace ui {

// ---------------------------------------------------------------- 按钮样式
enum class ButtonStyle { Primary, Secondary, Ghost, Danger };

// 全应用共享资源（AppWindow 启动时初始化一次）
namespace res {
extern HFONT Body;    // 正文 17
extern HFONT BodyStrong;  // 正文半粗（卡片段标题）
extern HFONT Small;   // 辅助 14
extern HFONT SmallStrong;  // 分组标题/列头 13 半粗
extern HFONT Title;   // 标题 18 半粗
extern HFONT PageTitle;  // 页面标题 24
extern HFONT Mono;    // 控制台 14
extern HFONT Icon;    // Segoe MDL2 字形 19
extern HFONT IconSm;  // 小号字形 14（内联徽标）
extern HFONT IconLg;  // 大号字形 28
} // namespace res

void InitResources(UINT dpi);
void FreeResources();

// 为 owner-draw 按钮安装悬停跟踪（自动失效重绘）
void InstallButtonHover(HWND btn);

// 绘制 owner-draw 按钮；glyph != 0 时图标与文字作为一组整体居中。
// bg 是按钮四周的底色，用来填掉圆角外的四角（不填会在深色主题露出白角）
void DrawFluentButton(const DRAWITEMSTRUCT* d, ButtonStyle style,
                      COLORREF accent = design::color::Primary,
                      wchar_t glyph = 0,
                      COLORREF bg = design::color::Bg);

// 图标按钮：方形、无边框、悬停出浅底（用于窗口右上角的次级入口）
void DrawIconButton(const DRAWITEMSTRUCT* d, wchar_t glyph,
                    COLORREF bg = design::color::Surface,
                    COLORREF accent = design::color::TextSub);

// ---------------------------------------------------------------- 搜索框
// 创建「无边框 EDIT + 清除按钮」组合；父窗口按常规 WM_COMMAND/EN_CHANGE 接收
HWND CreateSearchBox(HWND parent, int editId, int clearId, HINSTANCE hInst);
// 布局（父窗口 WM_SIZE 时调用）：按外部矩形摆放内部控件
void LayoutSearchBox(HWND edit, HWND clearBtn, const RECT& rc);
// 文本变化后刷新清除按钮显隐
void RefreshSearchBox(HWND edit, HWND clearBtn);
// 清除按钮的绘制（父窗口 WM_DRAWITEM 转发）
void DrawSearchClearButton(const DRAWITEMSTRUCT* d);

// 输入框占位提示：自绘（系统 EM_SETCUEBANNER 在深色系统下会被染成近白色，
// 落在浅色输入框上完全看不见，因此统一自己画）
void SetFieldPlaceholder(HWND edit, const wchar_t* text);

// ---------------------------------------------------------------- Toast
// 右下角浮出通知；accent 决定左侧色条与图标色
void ShowToast(HWND parent, const std::wstring& text,
               COLORREF accent = design::color::Primary,
               wchar_t glyph = 0xE73E /* CheckMark */);
// 立即关闭当前 Toast（切模块/退出前调用）
void DismissToast();

// ---------------------------------------------------------------- 日志控制台
//  自绘行缓冲视图（取代系统 EDIT 控件）。
//  为什么不用 EDIT：EDIT 拖动滚动条时 Windows 会将 ClearType 子像素抗锯齿
//  降级为灰度渲染并局部重绘，文字明显发虚拖影，且此行为无法从应用层关闭。
//  自绘 + 双缓冲：每次滚动都是整窗高质量重绘，文字始终清晰。
HWND CreateLogView(HWND parent, int id, HINSTANCE hInst);
void LogViewAppendBatch(HWND h, const std::vector<std::wstring>& lines);
void LogViewClear(HWND h);

// ---------------------------------------------------------------- 其它
void FillSolid(HDC dc, const RECT& rc, COLORREF c);
bool PtInRectStrict(const RECT& rc, int x, int y);

} // namespace ui
} // namespace mz
