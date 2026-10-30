// ============================================================================
//  GDI RAII 包装 + 双缓冲渲染上下文
//  ---------------------------------------------------------------------------
//  设计原则：
//    * 每一个 GDI 对象都有唯一所有者，离开作用域自动释放 —— 杜绝句柄泄漏
//    * 所有窗口绘制一律走 DoubleBuffer，先画到内存 DC 再一次性呈现，
//      彻底消除 WM_PAINT 闪烁（旧版直接画屏幕，拖动/切换时明显闪）
//    * 不用智能指针管 GDI：显式类型更贴合 Win32 心智模型，也避免头文件膨胀
// ============================================================================
#pragma once

#include "design.h"

#include <memory>

namespace mz {
namespace ui {

// ---------------------------------------------------------------- RAII 包装
class Pen {
public:
    Pen(COLORREF c, int width = 1, int style = PS_SOLID)
        : h_(CreatePen(style, width, c)) {}
    ~Pen() { if (h_) DeleteObject(h_); }
    Pen(const Pen&) = delete;
    Pen& operator=(const Pen&) = delete;
    HPEN get() const { return h_; }
    operator HPEN() const { return h_; }
private:
    HPEN h_;
};

class Brush {
public:
    explicit Brush(COLORREF c) : h_(CreateSolidBrush(c)) {}
    ~Brush() { if (h_) DeleteObject(h_); }
    Brush(const Brush&) = delete;
    Brush& operator=(const Brush&) = delete;
    HBRUSH get() const { return h_; }
    operator HBRUSH() const { return h_; }
private:
    HBRUSH h_;
};

class Font {
public:
    Font(int height, int weight, const wchar_t* face)
        : h_(CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, face)) {}
    Font(int height, int weight, const wchar_t* face, bool mono)
        : h_(CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, face)) {}
    ~Font() { if (h_) DeleteObject(h_); }
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    HFONT get() const { return h_; }
    operator HFONT() const { return h_; }
private:
    HFONT h_;
};

// DC 中对象的临时替换，析构时自动还原（等价于 MFC 的 CGdiObject 守卫）
class Selected {
public:
    Selected(HDC dc, HGDIOBJ obj) : dc_(dc), old_(SelectObject(dc, obj)) {}
    ~Selected() { if (dc_ && old_) SelectObject(dc_, old_); }
    Selected(const Selected&) = delete;
    Selected& operator=(const Selected&) = delete;
private:
    HDC dc_;
    HGDIOBJ old_;
};

// ---------------------------------------------------------------- 双缓冲
// 用法：
//   DoubleBuffer db;
//   HDC dc = db.Begin(hwndDc, clientRect);   // 拿到内存 DC
//   ... 全部画在 dc 上 ...
//   db.End(hwndDc);                          // 一次性位块传送到屏幕
class DoubleBuffer {
public:
    DoubleBuffer() = default;
    ~DoubleBuffer() { Release(); }

    HDC Begin(HDC target, const RECT& rc) {
        Release();
        w_ = rc.right - rc.left;
        h_ = rc.bottom - rc.top;
        if (w_ <= 0 || h_ <= 0) return nullptr;
        mem_ = CreateCompatibleDC(target);
        if (!mem_) return nullptr;
        bmp_ = CreateCompatibleBitmap(target, w_, h_);
        if (!bmp_) { DeleteDC(mem_); mem_ = nullptr; return nullptr; }
        old_ = SelectObject(mem_, bmp_);
        SetRect(&rc_, rc.left, rc.top, rc.right, rc.bottom);
        return mem_;
    }

    void End(HDC target) {
        if (!mem_) return;
        BitBlt(target, rc_.left, rc_.top, w_, h_, mem_, 0, 0, SRCCOPY);
        Release();
    }

    // 直接暴露内存 DC（Begin 成功后有效）
    HDC dc() const { return mem_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void Release() {
        if (mem_ && old_) SelectObject(mem_, old_);
        if (bmp_) DeleteObject(bmp_);
        if (mem_) DeleteDC(mem_);
        mem_ = nullptr; bmp_ = nullptr; old_ = nullptr;
    }
    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    int w_ = 0, h_ = 0;
    RECT rc_ = {};
};

// ---------------------------------------------------------------- 绘制辅助
// GDI+ 生命周期（懒加载，首次 AA 绘制前自动 Startup；InitResources 显式预热）。
// Shutdown 与进程字体资源一起在 FreeResources 释放，崩溃域与 v2.16 一致。
void EnsureGdiplus();
void ShutdownGdiplus();
// GDI+ 是否已成功启动（EnsureGdiplus 之后查询）
bool GdiplusReady();

// 圆角矩形（r≤2 保留 GDI 快路径；r>2 走 GDI+ GraphicsPath 抗锯齿——
// CreateRoundRectRgn 的锯齿边在 VI 胶囊/大圆角上不可接受）
void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill);

// 圆角矩形描边（同上 AA 语义；width 走 GDI+ Pen）
void StrokeRoundRect(HDC dc, const RECT& rc, int radius, COLORREF line, int width = 1);

// 水平/垂直渐变填充（用于 Toast、进度条等需要质感的位置）
void FillGradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom);

// 文本绘制（透明底 + 可选省略号），返回绘制宽度
int DrawText(HDC dc, const wchar_t* text, const RECT& rc, UINT format,
             COLORREF color, HFONT font = nullptr);

// Segoe MDL2 字形绘制（图标）
int DrawGlyph(HDC dc, wchar_t glyph, const RECT& rc, COLORREF color, HFONT font);

// 在指定矩形内居中绘制图标字形
void DrawGlyphCentered(HDC dc, wchar_t glyph, const RECT& rc, COLORREF color, HFONT font);

// 简单的颜色混合（用于动画插值）
COLORREF BlendColor(COLORREF a, COLORREF b, int t /*0..1000*/);

} // namespace ui
} // namespace mz
