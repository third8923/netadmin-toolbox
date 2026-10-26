#include "gdi.h"

#include <cmath>

namespace mz {
namespace ui {

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill) {
    if (radius <= 0 || rc.right <= rc.left || rc.bottom <= rc.top) {
        RECT r = rc;
        if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, Brush(fill));
        return;
    }
    // 半径钳制在高度一半内，避免上下两条圆角弧自交
    int r = radius;
    int hgt = rc.bottom - rc.top;
    if (r * 2 > hgt) r = hgt / 2;
    if (r < 1) r = 1;

    // 单个圆角区域一次成型（旧实现先 FillRect 整矩形再补圆，等于没裁角）
    Brush b(fill);
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, r * 2, r * 2);
    if (rgn) {
        FillRgn(dc, rgn, b);
        DeleteObject(rgn);
    } else {
        RECT full = rc;
        FillRect(dc, &full, b);
    }
}

void StrokeRoundRect(HDC dc, const RECT& rc, int radius, COLORREF line, int width) {
    int r = radius;
    int hgt = rc.bottom - rc.top;
    if (r * 2 > hgt) r = hgt / 2;
    Pen p(line, width);
    Selected sp(dc, p);
    Selected sb(dc, GetStockObject(NULL_BRUSH));
    // 略微内缩，避免 1px 描边被裁掉一半
    RECT rr = { rc.left + width / 2, rc.top + width / 2,
                rc.right - width / 2 + 1, rc.bottom - width / 2 + 1 };
    RoundRect(dc, rr.left, rr.top, rr.right, rr.bottom, r * 2, r * 2);
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
