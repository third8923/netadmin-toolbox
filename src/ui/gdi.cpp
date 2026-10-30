// ============================================================================
//  GDI RAII 包装 + 双缓冲渲染上下文 + GDI+ 抗锯齿圆角（v2.17）
//  ---------------------------------------------------------------------------
//  设计原则：
//    * 每一个 GDI 对象都有唯一所有者，离开作用域自动释放 —— 杜绝句柄泄漏
//    * 所有窗口绘制一律走 DoubleBuffer，先画到内存 DC 再一次性呈现，
//      彻底消除 WM_PAINT 闪烁
//    * v2.17：FillRoundRect / StrokeRoundRect 半径 >2 时改走 GDI+
//      GraphicsPath 抗锯齿路径（VI 胶囊 999px / 卡片 12px 圆角必须平滑）；
//      r≤2 保留旧 GDI 快路径（细线/分隔线场景，性能优先）。
//      GDI+ 对象栈上创建即用即弃（每帧数百次实测可承受，双缓冲已就位）。
// ============================================================================
#include "gdi.h"

#include <windows.h>
#include <objidl.h>     // PROPID（WIN32_LEAN_AND_MEAN 排除了 objidl，gdiplus 需要）
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

namespace mz {
namespace ui {

// ---------------------------------------------------------------- GDI+ 生命周期

namespace {
bool g_gdiplus = false;
ULONG_PTR g_gdiplusToken = 0;
} // namespace

void EnsureGdiplus() {
    if (g_gdiplus) return;
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &si, nullptr) == Gdiplus::Ok)
        g_gdiplus = true;
}

void ShutdownGdiplus() {
    if (g_gdiplus) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplus = false;
        g_gdiplusToken = 0;
    }
}

// ---------------------------------------------------------------- AA 圆角

namespace {

Gdiplus::Color GpColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 圆角路径（GDI+ 坐标系：右/下边界各内收 0.5px，避免被裁边）
void GpRoundPath(Gdiplus::GraphicsPath& p, const RECT& rc, int radius) {
    Gdiplus::REAL x = (Gdiplus::REAL)rc.left, y = (Gdiplus::REAL)rc.top;
    Gdiplus::REAL w = (Gdiplus::REAL)(rc.right - rc.left - 1), h = (Gdiplus::REAL)(rc.bottom - rc.top - 1);
    Gdiplus::REAL r = (Gdiplus::REAL)radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0.5f) {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    Gdiplus::REAL x2 = x + w, y2 = y + h;
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x2 - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x2 - r * 2, y2 - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y2 - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

} // namespace

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    // r≤2：GDI 快路径（细线/ hairline 场景，锯齿不可见）
    if (radius <= 2) {
        int r = radius;
        int hgt = rc.bottom - rc.top;
        if (r * 2 > hgt) r = hgt / 2;
        if (r < 1) {
            FillRect(dc, &rc, Brush(fill));
            return;
        }
        Brush b(fill);
        HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                      r * 2, r * 2);
        if (rgn) {
            FillRgn(dc, rgn, b);
            DeleteObject(rgn);
        } else {
            RECT full = rc;
            FillRect(dc, &full, b);
        }
        return;
    }

    // r>2：GDI+ 抗锯齿路径填充
    EnsureGdiplus();
    if (!g_gdiplus) {
        // GDI+ 启动失败：回退 GDI 圆角（保功能不保平滑）
        int r = std::min(radius, (int)((rc.bottom - rc.top) / 2));
        Brush b(fill);
        HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                      r * 2, r * 2);
        if (rgn) { FillRgn(dc, rgn, b); DeleteObject(rgn); }
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    GpRoundPath(path, rc, radius);
    Gdiplus::SolidBrush b(GpColor(fill));
    g.FillPath(&b, &path);
}

void StrokeRoundRect(HDC dc, const RECT& rc, int radius, COLORREF line, int width) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    // r≤2：GDI 快路径（原实现）
    if (radius <= 2) {
        int r = radius;
        int hgt = rc.bottom - rc.top;
        if (r * 2 > hgt) r = hgt / 2;
        Pen p(line, width);
        Selected sp(dc, p);
        Selected sb(dc, GetStockObject(NULL_BRUSH));
        RECT rr = { rc.left + width / 2, rc.top + width / 2,
                    rc.right - width / 2 + 1, rc.bottom - width / 2 + 1 };
        RoundRect(dc, rr.left, rr.top, rr.right, rr.bottom, r * 2, r * 2);
        return;
    }

    // r>2：GDI+ 抗锯齿描边
    EnsureGdiplus();
    if (!g_gdiplus) {
        int r = std::min(radius, (int)((rc.bottom - rc.top) / 2));
        Pen p(line, width);
        Selected sp(dc, p);
        Selected sb(dc, GetStockObject(NULL_BRUSH));
        RECT rr = { rc.left + width / 2, rc.top + width / 2,
                    rc.right - width / 2 + 1, rc.bottom - width / 2 + 1 };
        RoundRect(dc, rr.left, rr.top, rr.right, rr.bottom, r * 2, r * 2);
        return;
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    GpRoundPath(path, rc, radius);
    Gdiplus::Pen p(GpColor(line), (Gdiplus::REAL)width);
    g.DrawPath(&p, &path);
}

void FillGradientV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom) {
    int hgt = rc.bottom - rc.top;
    if (hgt <= 0) return;
    // 逐行插值：高度很小（<200px），性能开销可忽略
    for (int y = 0; y < hgt; ++y) {
        int t = hgt > 1 ? (y * 1000) / (hgt - 1) : 0;
        RECT line = { rc.left, rc.top + y, rc.right, rc.top + y + 1 };
        FillRect(dc, &line, Brush(BlendColor(top, bottom, t)));
    }
}

int DrawText(HDC dc, const wchar_t* text, const RECT& rc, UINT format,
             COLORREF color, HFONT font) {
    if (!text) return 0;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    if (font) {
        Selected sf(dc, font);
        return ::DrawTextW(dc, text, -1, const_cast<RECT*>(&rc), format);
    }
    return ::DrawTextW(dc, text, -1, const_cast<RECT*>(&rc), format);
}

int DrawGlyph(HDC dc, wchar_t glyph, const RECT& rc, COLORREF color, HFONT font) {
    wchar_t buf[2] = { glyph, 0 };
    return DrawText(dc, buf, rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, color, font);
}

void DrawGlyphCentered(HDC dc, wchar_t glyph, const RECT& rc, COLORREF color, HFONT font) {
    wchar_t buf[2] = { glyph, 0 };
    DrawText(dc, buf, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, color, font);
}

COLORREF BlendColor(COLORREF a, COLORREF b, int t) {
    if (t < 0) t = 0;
    if (t > 1000) t = 1000;
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = ar + (br - ar) * t / 1000;
    int g = ag + (bg - ag) * t / 1000;
    int bl = ab + (bb - ab) * t / 1000;
    return RGB(r, g, bl);
}

} // namespace ui
} // namespace mz
