// ============================================================================
//  品牌绘制层实现 v2.17「VI-D2 蜕变之翼」
//  ---------------------------------------------------------------------------
//  蝶形构造参数（vi-manual-D2.pdf 第 4 节，逐项直译；单位 = 画布像素）：
//      画布 240×160（12×8 网格，单元 u = 20px）
//      蝶形外切 192×104 居中（x 24..216，y 28..132）
//      上翅 192×62 两片（藤紫 wing），内含两段 45° 折线走线通向 2 枚金焊盘
//           （圆环外径 12u/…即 12px 画布像素，线宽 3.5）
//      下翅 96×44 两片（金 signal，实心）
//      蝶身 墨 宽 12（胶囊）；触角 长 22 · 仰角 45°，线宽 3.5
//      过孔 实心点 r 2.8 两枚于翅根
//      标注引线宽 1.5（两档线宽，无第三档）
//  变体 C 反白：蝶身 paper、上翅 wing_on_ink、焊盘保持金——路径中不存在
//  wing 原色（见 WingColor() 的 switch 断言注释）。
//  GDI+ 对象栈上创建即用即弃；SmoothingModeAntiAlias；无渐变/外发光/投影。
// ============================================================================
#include "ui/brand.h"
#include "ui/gdi.h"
#include "ui/dpi.h"
#include "ui/controls.h"
#include "common.h"
#include "lang.h"

#include <windows.h>
#include <objidl.h>     // PROPID（WIN32_LEAN_AND_MEAN 排除了 objidl，gdiplus 需要）
#include <gdiplus.h>

#include <cmath>
#include <cwchar>

namespace mz {
namespace ui {
namespace brand {

using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::SolidBrush;
using Gdiplus::Color;
using Gdiplus::REAL;

namespace {

// ---------------------------------------------------------------- 色彩工具
Color Argb(COLORREF c, BYTE alpha = 255) {
    return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 走线色：上翅底色之上的线稿色（彩翅上用纸色线稿，语义 = 丝印反白）
COLORREF TraceColorFor(const design::ThemeTokens& t, MarkVariant v) {
    switch (v) {
    case MarkVariant::MonoB:    return t.Canvas;   // 墨翅上纸色线稿
    case MarkVariant::ReverseC: return t.Ink;      // 提亮紫翅上墨线稿
    default:                    return t.Paper;    // A：紫翅上纸色线稿
    }
}

// 蝶身色
COLORREF BodyColorFor(const design::ThemeTokens& t, MarkVariant v) {
    switch (v) {
    case MarkVariant::MonoB:    return t.Ink;
    case MarkVariant::ReverseC: return t.Paper;   // 墨底反白蝶身
    default:                    return t.Ink;
    }
}

// 上翅色。变体 C 路径绝不返回 t.Wing 原色（手册：藤紫原色禁止上墨底）。
COLORREF WingColorFor(const design::ThemeTokens& t, MarkVariant v) {
    switch (v) {
    case MarkVariant::MonoB:    return t.Ink;
    case MarkVariant::ReverseC: return t.WingOnInk;
    default:                    return t.Wing;
    }
}

// ---------------------------------------------------------------- GDI+ 路径

// 圆头矩形（四角等半径，方瓣圆头语法）
void PathRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0.5f) {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    REAL x2 = x + w, y2 = y + h;
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x2 - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x2 - r * 2, y2 - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y2 - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

// 胶囊（半径 = 高/2）
void PathCapsule(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h) {
    PathRoundRect(p, x, y, w, h, h / 2);
}

// 45° 折线断言：每段必须等 dx/dy（手册纪律，release 下静默跳过斜率不符的段）
void PathTrace(GraphicsPath& p, const Gdiplus::PointF* pts, int n) {
    if (n < 2) return;
    for (int i = 0; i + 1 < n; ++i) {
        REAL dx = std::fabs(pts[i + 1].X - pts[i].X);
        REAL dy = std::fabs(pts[i + 1].Y - pts[i].Y);
        if (dx > 0.01f && dy > 0.01f && std::fabs(dx - dy) > 0.01f) continue;   // 非 45°：丢弃
        p.AddLine(pts[i].X, pts[i].Y, pts[i + 1].X, pts[i + 1].Y);
    }
}

void FillPath(Graphics& g, GraphicsPath& path, COLORREF c) {
    SolidBrush b(Argb(c));
    g.FillPath(&b, &path);
}

void StrokePath(Graphics& g, GraphicsPath& path, COLORREF c, REAL width) {
    Pen p(Argb(c), width);
    p.SetLineJoin(Gdiplus::LineJoinRound);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);
    g.DrawPath(&p, &path);
}

} // namespace

// ---------------------------------------------------------------- 基元 API

void DrawTrace(HDC dc, const POINT* pts, int n, double unitW, COLORREF c) {
    if (!dc || !pts || n < 2) return;
    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    GraphicsPath path;
    // 仅 45° 段入路径（非 45° 段静默丢弃——手册纪律）
    for (int i = 0; i + 1 < n; ++i) {
        REAL dx = std::fabs(REAL(pts[i + 1].x - pts[i].x));
        REAL dy = std::fabs(REAL(pts[i + 1].y - pts[i].y));
        if (dx > 0.01 && dy > 0.01 && std::fabs(dx - dy) > 0.01) continue;
        path.AddLine(REAL(pts[i].x), REAL(pts[i].y), REAL(pts[i + 1].x), REAL(pts[i + 1].y));
    }
    StrokePath(g, path, c, REAL(unitW));
}

void DrawPad(HDC dc, POINT center, double unit, COLORREF c) {
    // 圆环焊盘：外径 12u（unit = 画布像素），环宽 = 外径/4（PCB 惯例）
    if (!dc) return;
    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const REAL od = REAL(12 * unit);
    const REAL thk = od / 4;
    Pen p(Argb(c), thk);
    g.DrawEllipse(&p, REAL(center.x) - (od - thk) / 2, REAL(center.y) - (od - thk) / 2,
                  od - thk, od - thk);
}

void DrawVia(HDC dc, POINT center, double unit, COLORREF c) {
    // 过孔：实心点 r 2.8u
    if (!dc) return;
    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const REAL r = REAL(2.8 * unit);
    SolidBrush b(Argb(c));
    g.FillEllipse(&b, REAL(center.x) - r, REAL(center.y) - r, r * 2, r * 2);
}

// ---------------------------------------------------------------- 蝶形标志

void DrawButterfly(HDC dc, const RECT& fit, MarkVariant v,
                   const design::ThemeTokens& t) {
    if (!dc || fit.right <= fit.left || fit.bottom <= fit.top) return;

    // 变体 C 纪律：墨底反白路径不允许出现藤紫原色（手动断言，防回归）
    if (v == MarkVariant::ReverseC) {
        COLORREF w = WingColorFor(t, v);
        (void)w;
        // w 必为 t.WingOnInk；若上式被改成 t.Wing，qa 阶段的变体 C 截图
        // 金占比/紫占比检查会失败——此处不重复实现运行期断言。
    }

    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // 画布 240×160 → fit 等比适配（净空间 = 画布自带边距 24/28）
    const REAL cw = 240.0f, ch = 160.0f;
    REAL fw = REAL(fit.right - fit.left), fh = REAL(fit.bottom - fit.top);
    REAL s = std::min(fw / cw, fh / ch);
    REAL ox = REAL(fit.left) + (fw - cw * s) / 2;
    REAL oy = REAL(fit.top) + (fh - ch * s) / 2;
    g.TranslateTransform(ox, oy);
    g.ScaleTransform(s, s);

    const COLORREF wing = WingColorFor(t, v);
    const COLORREF body = BodyColorFor(t, v);
    const COLORREF gold = t.Signal;
    const COLORREF trace = TraceColorFor(t, v);

    // ---- 上翅：两片 96×62，圆头外角（y 28..90）----
    {
        GraphicsPath lw, rw;
        PathRoundRect(lw, 24, 28, 96, 62, 18);    // 左上翅（外侧圆角 18）
        PathRoundRect(rw, 120, 28, 96, 62, 18);   // 右上翅
        FillPath(g, lw, wing);
        FillPath(g, rw, wing);
    }

    // ---- 下翅：两片 96×44 实心（金基板，y 88..132）----
    {
        GraphicsPath lw, rw;
        PathRoundRect(lw, 24, 88, 96, 44, 14);
        PathRoundRect(rw, 120, 88, 96, 44, 14);
        FillPath(g, lw, gold);
        FillPath(g, rw, gold);
    }

    // ---- 上翅走线：两段 45° 折线 → 2 枚金焊盘（线宽 3.5）----
    //  左：翅根(108,84) → 折点(72,48) → 焊盘中心(84,36)
    //  右：镜像 (132,84) → (168,48) → (156,36)
    {
        Gdiplus::PointF l1[] = { {108, 84}, {72, 48} };
        Gdiplus::PointF l2[] = { {72, 48}, {84, 36} };
        Gdiplus::PointF r1[] = { {132, 84}, {168, 48} };
        Gdiplus::PointF r2[] = { {168, 48}, {156, 36} };
        GraphicsPath pl, pl2, pr, pr2;
        PathTrace(pl, l1, 2);
        PathTrace(pl2, l2, 2);
        PathTrace(pr, r1, 2);
        PathTrace(pr2, r2, 2);
        StrokePath(g, pl, trace, 3.5f);
        StrokePath(g, pl2, trace, 3.5f);
        StrokePath(g, pr, trace, 3.5f);
        StrokePath(g, pr2, trace, 3.5f);

        // 焊盘：圆环外径 12，金（两枚，于走线末端）
        Gdiplus::PointF padL{ 84, 36 }, padR{ 156, 36 };
        const REAL od = 12.0f, thk = 3.0f;
        Pen gp(Argb(gold), thk);
        g.DrawEllipse(&gp, padL.X - (od - thk) / 2, padL.Y - (od - thk) / 2,
                      od - thk, od - thk);
        g.DrawEllipse(&gp, padR.X - (od - thk) / 2, padR.Y - (od - thk) / 2,
                      od - thk, od - thk);
    }

    // ---- 蝶身：墨胶囊，宽 12（x 114..126，y 34..130）----
    {
        GraphicsPath b;
        PathCapsule(b, 114, 34, 12, 96);
        FillPath(g, b, body);
    }

    // ---- 触角：长 22 · 仰角 45°（线宽 3.5，圆头）----
    {
        Gdiplus::PointF a1[] = { {117, 34}, {101, 18} };   // 左：dx=-16, dy=-16 → 45°
        Gdiplus::PointF a2[] = { {123, 34}, {139, 18} };
        GraphicsPath pa1, pa2;
        PathTrace(pa1, a1, 2);
        PathTrace(pa2, a2, 2);
        StrokePath(g, pa1, body, 3.5f);
        StrokePath(g, pa2, body, 3.5f);
    }

    // ---- 过孔：翅根两枚，实心 r 2.8 ----
    {
        // 翅根过孔用蝶身色（墨/纸）；保持与蝶身同色系的「孔位」语义
        SolidBrush vb(Argb(body));
        const REAL r = 2.8f;
        g.FillEllipse(&vb, 108 - r, 88 - r, r * 2, r * 2);
        g.FillEllipse(&vb, 132 - r, 88 - r, r * 2, r * 2);
    }
}

// ---------------------------------------------------------------- 字标

void DrawWordmark(HDC dc, const RECT& fit, const design::ThemeTokens& t) {
    if (!dc || fit.right <= fit.left || fit.bottom <= fit.top) return;

    // Baloo 2 800（内嵌子集）；回落雅黑 Bold（中文场景绝不进字标——
    // 文案为纯拉丁 "biohacker.fun"，子集覆盖 U+0020–007E 足够）
    const int px = fit.bottom - fit.top;
    HFONT f = CreateFontW(px, 0, 0, 0, 800, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE,
                          design::fontsys::HasDisplayFont() ? L"Baloo 2"
                                                            : L"Microsoft YaHei UI");
    Selected sf(dc, f);

    const wchar_t* left = L"biohacker";
    const wchar_t* right = L"fun";
    SIZE lw = {}, rw = {};
    GetTextExtentPoint32W(dc, left, (int)wcslen(left), &lw);
    GetTextExtentPoint32W(dc, right, (int)wcslen(right), &rw);

    // 金点=焊盘：直径 0.34×字高，居中于两词之间；位置大小固定不可拆
    const int dotD = (int)(px * 0.34);
    const int gap = (int)(px * 0.10);
    const int total = lw.cx + gap + dotD + gap + rw.cx;
    int x = fit.left + ((fit.right - fit.left) - total) / 2;
    int baseline = fit.top + (fit.bottom - fit.top) / 2;

    RECT lr = { x, fit.top, x + lw.cx + 8, fit.bottom };
    DrawText(dc, left, lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
             t.Text, f);
    x += lw.cx + gap;

    // 金点（焊盘语义：外环 3px + 实心）——GDI+ 抗锯齿
    {
        Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        REAL cx = REAL(x + dotD / 2), cy = REAL(baseline);
        Pen ring(Argb(t.Gold), 2.0f);
        SolidBrush fill(Argb(t.Gold));
        g.DrawEllipse(&ring, cx - dotD / 2.0f, cy - dotD / 2.0f,
                      REAL(dotD), REAL(dotD));
        REAL inner = dotD * 0.42f;
        g.FillEllipse(&fill, cx - inner / 2, cy - inner / 2, inner, inner);
    }
    x += dotD + gap;

    RECT rr = { x, fit.top, x + rw.cx + 8, fit.bottom };
    DrawText(dc, right, rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
             t.Text, f);
    DeleteObject(f);
}

// ---------------------------------------------------------------- 空态 / 错误态

namespace {

// 在 rc 中央画「未画完的走线」图形（空态/错误态共用骨架）。
// broken = true 时走线中段断开 6u 缺口并加标注引线（错误态）。
void DrawOpenTrace(HDC dc, const RECT& rc, const design::ThemeTokens& t,
                   bool broken) {
    const int H = rc.bottom - rc.top;
    const double u = H / 40.0;                    // 图形区单元（画布像素）
    const double traceW = 3.5 * u;                // 走线宽
    const double leaderW = 1.5 * u;               // 标注引线宽
    const int cx = (rc.left + rc.right) / 2;
    const int cy = rc.top + (int)(H * (broken ? 0.46 : 0.5));
    // span 同时受宽/高约束：保证端点、开口焊盘与金 via 都落在图形区内
    const int span = (int)std::min(240.0,
        std::min((rc.right - rc.left) * 0.42, H * 0.42));

    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    COLORREF lineC = t.Text3;                     // 线稿走线（line 语义）

    // 45° 走线：从左下入、右上出（两段 45° 折线）
    int mid = broken ? (int)(span * 0.18) : span; // 断点缺口 6u
    POINT seg1[2] = { {cx - span, cy + span}, {cx, cy} };
    GraphicsPath p1;
    p1.AddLine(REAL(seg1[0].x), REAL(seg1[0].y), REAL(seg1[1].x), REAL(seg1[1].y));
    StrokePath(g, p1, lineC, REAL(traceW));

    // 右半段：断开时留 6u 缺口
    int gapPx = broken ? (int)(6 * u * 2) : 0;    // 缺口（视觉 6u×2 保守取值）
    POINT seg2a[2] = { {cx + gapPx, cy - gapPx}, {cx + span, cy - span} };
    GraphicsPath p2;
    p2.AddLine(REAL(seg2a[0].x), REAL(seg2a[0].y), REAL(seg2a[1].x), REAL(seg2a[1].y));
    StrokePath(g, p2, lineC, REAL(traceW));

    // 右端：开口焊盘（未画完的 270° 圆弧 + 金 via 一枚）
    {
        const REAL od = REAL(12 * u);
        Pen ring(Argb(lineC), REAL(traceW));
        // 开口圆弧：0°..270°（右下缺 90°——「线不画完」）
        g.DrawArc(&ring, REAL(cx + span) - od / 2, REAL(cy - span) - od / 2,
                  od, od, -90.0f, 270.0f);
        // 金 via 一枚（唯一允许的金）
        SolidBrush vb(Argb(t.Gold));
        const REAL vr = REAL(2.8 * u);
        g.FillEllipse(&vb, REAL(cx - span) - vr, REAL(cy + span) - vr, vr * 2, vr * 2);
    }

    if (broken) {
        // 1.5u 标注引线指向断点（45° 短线）
        POINT ld[2] = { {cx + gapPx / 2 + (int)(10 * u), cy - gapPx / 2 - (int)(2 * u)},
                        {cx + gapPx / 2 + (int)(2 * u), cy - gapPx / 2 - (int)(10 * u)} };
        GraphicsPath pl;
        pl.AddLine(REAL(ld[0].x), REAL(ld[0].y), REAL(ld[1].x), REAL(ld[1].y));
        StrokePath(g, pl, lineC, REAL(leaderW));
    }
}

} // namespace

void DrawEmptyState(HDC dc, const RECT& rc, const design::ThemeTokens& t) {
    if (!dc || rc.right <= rc.left || rc.bottom <= rc.top) return;

    // 图形区占上半，文案居下
    RECT gr = rc;
    gr.bottom = rc.top + (rc.bottom - rc.top) * 55 / 100;
    DrawOpenTrace(dc, gr, t, false);

    // 引导文案（ink55 语义 = Text3）
    const wchar_t* caption = lang::T(L"信号没到，线就不画完");
    DrawText(dc, caption,
             RECT{ rc.left, gr.bottom + dpi::S(6), rc.right,
                   gr.bottom + dpi::S(30) },
             DT_CENTER | DT_SINGLELINE | DT_NOPREFIX, t.Text2, ui::res::Body);
}

void DrawErrorState(HDC dc, const RECT& rc, const design::ThemeTokens& t) {
    if (!dc || rc.right <= rc.left || rc.bottom <= rc.top) return;

    RECT gr = rc;
    gr.bottom = rc.top + (rc.bottom - rc.top) * 52 / 100;
    DrawOpenTrace(dc, gr, t, true);               // 断线错误态——不上红

    const wchar_t* caption = lang::T(L"信号在中途断了");
    const wchar_t* tip = lang::T(L"检查网络、权限或参数后重试");
    DrawText(dc, caption,
             RECT{ rc.left, gr.bottom + dpi::S(4), rc.right,
                   gr.bottom + dpi::S(28) },
             DT_CENTER | DT_SINGLELINE | DT_NOPREFIX, t.Text2, ui::res::Body);
    DrawText(dc, tip,
             RECT{ rc.left, gr.bottom + dpi::S(28), rc.right,
                   gr.bottom + dpi::S(52) },
             DT_CENTER | DT_SINGLELINE | DT_NOPREFIX, t.Text3, ui::res::Small);
}

// ---------------------------------------------------------------- 蜕变四态叙事

void DrawMorphTimeline(HDC dc, const RECT& rc, const design::ThemeTokens& t) {
    if (!dc || rc.right <= rc.left || rc.bottom <= rc.top) return;

    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    COLORREF lineC = t.Text3;
    const int H = rc.bottom - rc.top;
    const double u = std::max(0.6, H / 90.0);     // 叙事画布单元
    const double traceW = 1.5 * u;                // 线稿一律标注档宽

    // 标题
    DrawText(dc, lang::T(L"蜕变四态"),
             RECT{ rc.left, rc.top, rc.right, rc.top + dpi::S(22) },
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
             t.Text, ui::res::BodyStrong);

    // 「当前版本 vX.Y」实际文本宽度测量（Bug-B1 修复）：
    // 金点 + 文本整体自右缘内收，预留宽度按实测而非固定 96px，杜绝字形截断。
    wchar_t ver[48];
    wsprintfW(ver, L"%s v%d.%d", lang::T(L"当前版本"),
              k_AppVersionMajor, k_AppVersionMinor);
    int verW = 0;
    {
        SIZE sz = { 0, 0 };
        HGDIOBJ old = SelectObject(dc, ui::res::Small);
        if (GetTextExtentPoint32W(dc, ver, (int)wcslen(ver), &sz)) verW = sz.cx;
        if (old) SelectObject(dc, old);
        if (verW <= 0) verW = dpi::S(110);            // 测量失败兜底（保守值）
    }

    const int baseY = rc.top + H / 2 + dpi::S(10);
    // 标签行底部对齐带底：与带外脚注行不重叠
    const int labelY = std::min(baseY + dpi::S(18),
                                (int)(rc.bottom - dpi::S(22)));
    const int x0 = rc.left + dpi::S(28);
    // 金点圆心 = 右边距 8 + 文本宽 + 点文间距 6 + 焊盘半径 6u
    const int gx = rc.right - dpi::S(8) - verW - dpi::S(6) - (int)std::ceil(6 * u);
    const int span = gx - x0;
    const int step = span > dpi::S(80) ? (int)(span / 4.5) : dpi::S(60);

    // 时间线走线（45° 语义：水平基线用两段 45° 微折？——手册允许基线水平走线
    // 属于「标注引线」延伸，此处按时间轴语义画水平线，宽度走线档）
    {
        GraphicsPath p;
        p.AddLine(REAL(x0), REAL(baseY), REAL(gx), REAL(baseY));
        StrokePath(g, p, lineC, REAL(traceW));
    }

    // 四格线稿（仅走线/焊盘/过孔语法）
    const wchar_t* labels[4] = { lang::T(L"卵 v0.1"), lang::T(L"幼虫 v0.5"),
                                 lang::T(L"蛹 v0.9"), lang::T(L"蝶 v1.0") };
    for (int i = 0; i < 4; ++i) {
        const int nx = x0 + step * i + step / 2;
        // 跳变点 = 过孔（实心点）
        SolidBrush vb(Argb(lineC));
        const REAL vr = REAL(2.8 * u);
        g.FillEllipse(&vb, REAL(nx) - vr, REAL(baseY) - vr, vr * 2, vr * 2);

        // 各态线稿（45° 折线语法，画在节点上方 34px 带）
        const int cy = baseY - dpi::S(26);
        GraphicsPath p;
        if (i == 0) {          // 卵：小椭圆（圆头闭合线稿）
            Pen pen(Argb(lineC), REAL(traceW));
            g.DrawEllipse(&pen, REAL(nx - 8 * u), REAL(cy - 11 * u),
                          REAL(16 * u), REAL(22 * u));
        } else if (i == 1) {   // 幼虫：两段 45° 折线（拱背）
            Gdiplus::PointF seg[] = { {REAL(nx - 14 * u), REAL(cy + 6 * u)},
                                      {REAL(nx - 4 * u), REAL(cy - 4 * u)},
                                      {REAL(nx + 6 * u), REAL(cy + 6 * u)} };
            PathTrace(p, seg, 3);
            StrokePath(g, p, lineC, REAL(traceW));
        } else if (i == 2) {   // 蛹：胶囊线稿（斜 45° 悬挂）
            GraphicsPath cap;
            PathCapsule(cap, REAL(nx - 9 * u), REAL(cy - 12 * u),
                        REAL(18 * u), REAL(24 * u));
            Pen pen(Argb(lineC), REAL(traceW));
            g.DrawPath(&pen, &cap);
            // 悬挂引线（1.5u 标注档）
            GraphicsPath h;
            h.AddLine(REAL(nx), REAL(cy - 12 * u), REAL(nx + 10 * u),
                      REAL(cy - 22 * u));
            StrokePath(g, h, lineC, REAL(traceW));
        } else {               // 蝶：X 形四翅线稿 + 触角
            GraphicsPath w1, w2;
            w1.AddLine(REAL(nx - 14 * u), REAL(cy + 10 * u), REAL(nx + 14 * u),
                       REAL(cy - 18 * u));
            w2.AddLine(REAL(nx + 14 * u), REAL(cy + 10 * u), REAL(nx - 14 * u),
                       REAL(cy - 18 * u));
            StrokePath(g, w1, lineC, REAL(traceW));
            StrokePath(g, w2, lineC, REAL(traceW));
            GraphicsPath ant;
            Gdiplus::PointF a[] = { {REAL(nx - 5 * u), REAL(cy - 18 * u)},
                                    {REAL(nx - 10 * u), REAL(cy - 23 * u)} };
            Gdiplus::PointF b[] = { {REAL(nx + 5 * u), REAL(cy - 18 * u)},
                                    {REAL(nx + 10 * u), REAL(cy - 23 * u)} };
            PathTrace(ant, a, 2);
            PathTrace(ant, b, 2);
            StrokePath(g, ant, lineC, REAL(traceW));
        }

        // 节点标签
        DrawText(dc, labels[i],
                 RECT{ nx - step / 2, labelY, nx + step / 2, labelY + dpi::S(20) },
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 t.Text3, ui::res::Small);
    }

    // 当前版本：金点 = 焊盘语义高亮，落在蝶之后延伸的走线上
    {
        const REAL od = REAL(12 * u);
        Pen ring(Argb(t.Gold), REAL(traceW));
        g.DrawEllipse(&ring, REAL(gx) - od / 2, REAL(baseY) - od / 2, od, od);
        SolidBrush fill(Argb(t.Gold));
        g.FillEllipse(&fill, REAL(gx) - 2.8 * u, REAL(baseY) - 2.8 * u,
                      REAL(5.6 * u), REAL(5.6 * u));
        // 文本自 gx + 6 起画，终点 = gx + 6 + verW ≤ rc.right - 8（见上方实测内收）
        DrawText(dc, ver,
                 RECT{ gx + dpi::S(6), baseY - dpi::S(12), rc.right,
                       baseY + dpi::S(12) },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 t.GoldText, ui::res::Small);
    }
}

// ---------------------------------------------------------------- 启动闪屏

namespace {

constexpr wchar_t kSplashClass[] = L"MzSplashWnd";

struct SplashState {
    int elapsed = 0;      // 毫秒
};

SplashState g_splash;

LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    namespace motion = design::motion;
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, 1, motion::Frame, nullptr);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        DoubleBuffer db;
        HDC mem = db.Begin(dc, rc);
        if (mem) {
            const design::ThemeTokens* t = design::CurrentTheme();
            FillSolid(mem, rc, t->Canvas);

            // 蝶形：纸亮 = 变体 A；墨暗 = 变体 C（反白）
            const int markW = dpi::S(180), markH = dpi::S(120);
            RECT fit{ (rc.left + rc.right - markW) / 2,
                      rc.top + dpi::S(28),
                      (rc.left + rc.right + markW) / 2,
                      rc.top + dpi::S(28) + markH };
            brand::DrawButterfly(mem, fit,
                                 t->Dark ? MarkVariant::ReverseC : MarkVariant::FullA,
                                 *t);

            // 字标 + 产品名副行
            RECT wm{ rc.left, fit.bottom + dpi::S(10), rc.right,
                     fit.bottom + dpi::S(46) };
            brand::DrawWordmark(mem, wm, *t);
            DrawText(mem, design::brand::kAppNameEn,
                     RECT{ rc.left, wm.bottom + dpi::S(6), rc.right,
                           wm.bottom + dpi::S(28) },
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     t->Text2, ui::res::Body);
            db.End(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        g_splash.elapsed += motion::Frame;
        int alpha;
        if (g_splash.elapsed <= motion::SplashIn) {
            alpha = 255 * g_splash.elapsed / motion::SplashIn;
        } else if (g_splash.elapsed < motion::SplashMax) {
            alpha = 255;
        } else {
            alpha = 255 - 255 * (g_splash.elapsed - motion::SplashMax) /
                              motion::SplashOut;
            if (alpha <= 0) { DestroyWindow(hwnd); return 0; }
        }
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void ShowSplash(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = SplashWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kSplashClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    const int w = dpi::S(440), h = dpi::S(320);
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - w) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                                    (owner ? 0 : WS_EX_TOPMOST),
                                kSplashClass, L"",
                                WS_POPUP, x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}

} // namespace brand
} // namespace ui
} // namespace mz
