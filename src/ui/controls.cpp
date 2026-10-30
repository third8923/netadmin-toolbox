#include "controls.h"
#include "dpi.h"
#include "lang.h"

#include <commctrl.h>
#include <algorithm>
#include <cwchar>

namespace mz {
namespace ui {

namespace res {
HFONT Body = nullptr;
HFONT BodyStrong = nullptr;
HFONT Small = nullptr;
HFONT SmallStrong = nullptr;
HFONT Title = nullptr;
HFONT Lead = nullptr;
HFONT PageTitle = nullptr;
HFONT Wordmark = nullptr;
HFONT Mono = nullptr;
HFONT MonoStrong = nullptr;
HFONT Icon = nullptr;
HFONT IconSm = nullptr;
HFONT IconLg = nullptr;
} // namespace res

namespace {

constexpr wchar_t kPropHover[] = L"MzBtnHover";

LRESULT CALLBACK ButtonHoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!GetPropW(hwnd, kPropHover)) {
            SetPropW(hwnd, kPropHover, (HANDLE)1);
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, kPropHover);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ENABLE:
    case WM_STYLECHANGED:
        RemovePropW(hwnd, kPropHover);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------- 资源

void InitResources(UINT dpi) {
    namespace font = design::font;
    FreeResources();
    EnsureGdiplus();   // GDI+ 生命周期：首次资源创建前预热（r>2 圆角 AA 用）

    const wchar_t* kFace = L"Microsoft YaHei UI";
    // Display 层：Baloo 2 800（内嵌子集；加载失败回落雅黑，品牌降级不破版）
    const wchar_t* kDisplayFace = design::fontsys::HasDisplayFont()
                                      ? L"Baloo 2" : kFace;
    // Data 层：JetBrains Mono → Cascadia Mono → Consolas 回落链
    const wchar_t* kMonoFace = L"Consolas";
    if (design::fontsys::HasMonoFont())      kMonoFace = L"JetBrains Mono";
    else if (design::fontsys::FamilyAvailable(L"Cascadia Mono"))
        kMonoFace = L"Cascadia Mono";

    // ---- 七级字阶（Body 16 / Label 14.5 走 S(double)）----
    res::Body      = CreateFontW(dpi::S(font::Body), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::BodyStrong = CreateFontW(dpi::S(font::Body), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::Small     = CreateFontW(dpi::S(font::Label), 0, 0, 0, 500, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::SmallStrong = CreateFontW(dpi::S(font::Label), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::Title     = CreateFontW(dpi::S(font::Title), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::Lead      = CreateFontW(dpi::S(font::Lead), 0, 0, 0, 500, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFace);
    res::PageTitle = CreateFontW(dpi::S(font::Display), 0, 0, 0, 800, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 kDisplayFace);
    res::Wordmark  = CreateFontW(dpi::S(font::Wordmark), 0, 0, 0, 800, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 kDisplayFace);
    // Data 层等宽（数据列/日志/时间戳；中文由 GDI 字体链接回落雅黑）
    res::Mono      = design::fontsys::DataFont(dpi::S(font::Data), false);
    res::MonoStrong = design::fontsys::DataFont(dpi::S(font::Data), true);
    res::Icon      = CreateFontW(dpi::S(19), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe MDL2 Assets");
    res::IconSm    = CreateFontW(dpi::S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe MDL2 Assets");
    res::IconLg    = CreateFontW(dpi::S(28), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe MDL2 Assets");
}

void FreeResources() {
    for (HFONT* f : { &res::Body, &res::BodyStrong, &res::Small, &res::SmallStrong,
                      &res::Title, &res::Lead, &res::PageTitle, &res::Wordmark,
                      &res::Mono, &res::MonoStrong, &res::Icon, &res::IconSm,
                      &res::IconLg })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    ShutdownGdiplus();
}

// ---------------------------------------------------------------- 按钮

void InstallButtonHover(HWND btn) {
    SetWindowSubclass(btn, ButtonHoverProc, 1, 0);
}

// owner-draw 按钮：VI-D2 胶囊语法（radius::Pill=999，FillRoundRect 内部钳制
// 到高度一半）。样式 → 角色令牌映射（§1.7 表）：
//   Primary   主按钮/危险确认主键 = BtnFill 墨底反白（危险键同色，不用红）
//   Secondary 次级 = Hover 面 + Hairline 描边圆头
//   Ghost     透明融入底色（侧栏辅助钮）
//   Danger    与 Primary 同底（VI 纪律：危险确认用墨底反白，红只归数据层）
void DrawFluentButton(const DRAWITEMSTRUCT* d, ButtonStyle style, COLORREF accent,
                      wchar_t glyph, COLORREF bg) {
    namespace c = design::color;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (d->itemState & ODS_SELECTED) != 0;
    const bool focus    = (d->itemState & ODS_FOCUS) != 0;
    const bool hover    = GetPropW(d->hwndItem, kPropHover) != nullptr;

    wchar_t text[160] = { 0 };
    GetWindowTextW(d->hwndItem, text, 160);

    RECT rc = d->rcItem;
    COLORREF fill = c::Canvas;
    COLORREF line = c::Hairline;
    bool stroked = false;
    COLORREF fg = c::Text;

    switch (style) {
    case ButtonStyle::Primary:
    case ButtonStyle::Danger:
        // 主键 = 墨底反白（暗主题 = 纸底反白）；危险确认键同色（红只归数据层）
        // 悬停轻微提亮（向底色混 13%），按下回实底——文字对比度全程 ≥9:1
        if (disabled) { fill = BlendColor(c::BtnFill, c::Canvas, 600);
                        fg = BlendColor(c::BtnText, c::Canvas, 600); }
        else if (hover && !pressed) { fill = BlendColor(c::BtnFill, c::Canvas, 870);
                                      fg = c::BtnText; }
        else              { fill = c::BtnFill; fg = c::BtnText; }
        break;
    case ButtonStyle::Ghost:
        if (disabled) { fill = bg; fg = c::Text3; }
        else {
            fill = pressed ? c::AccentSoft : (hover ? c::Hover : bg);
            fg = disabled ? c::Text3 : (hover || pressed ? c::Text : c::Text2);
        }
        break;
    case ButtonStyle::Secondary:
    default:
        if (disabled) { fill = c::Canvas; fg = c::Text3; }
        else {
            fill = pressed ? c::AccentSoft
                           : (hover ? c::Hover : c::Canvas);
            fg = disabled ? c::Text3 : c::Text;
        }
        line = c::Hairline;   // 次级按钮 = 描边圆头，无硬投影
        stroked = true;
        break;
    }

    FillSolid(d->hDC, rc, bg);   // 圆角外的四角：不铺底色会露系统灰
    FillRoundRect(d->hDC, rc, dpi::S(design::radius::Pill), fill);
    if (stroked)
        StrokeRoundRect(d->hDC, rc, dpi::S(design::radius::Pill), line, 1);

    if (glyph) {
        // 图标 + 文字作为一组整体居中：比「图标贴左、文字居中」更稳，
        // 不同字数下视觉重心不会偏移。
        SIZE ts{};
        {
            Selected sel(d->hDC, res::Body);
            GetTextExtentPoint32W(d->hDC, text, (int)wcslen(text), &ts);
        }
        const int box = dpi::S(18);
        const int gap = dpi::S(8);
        int total = box + gap + ts.cx;
        int start = rc.left + ((rc.right - rc.left) - total) / 2;
        if (start < rc.left + dpi::S(6)) start = rc.left + dpi::S(6);
        RECT ir = { start, rc.top, start + box, rc.bottom };
        DrawGlyphCentered(d->hDC, glyph, ir, fg, res::Icon);
        RECT tr = { start + box + gap, rc.top, rc.right - dpi::S(6), rc.bottom };
        DrawText(d->hDC, text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 fg, res::Body);
    } else {
        RECT tr = rc;
        DrawText(d->hDC, text, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 fg, res::Body);
    }

    if (focus && !disabled && style != ButtonStyle::Primary &&
        style != ButtonStyle::Danger) {
        // 焦点环：Accent 两档线宽中的强调档（非主键才有描边焦点环）
        RECT fr = rc;
        InflateRect(&fr, -dpi::S(3), -dpi::S(3));
        StrokeRoundRect(d->hDC, fr, dpi::S(design::radius::Pill), c::Accent, 2);
    }
}

void DrawIconButton(const DRAWITEMSTRUCT* d, wchar_t glyph,
                    COLORREF bg, COLORREF accent) {
    namespace c = design::color;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (d->itemState & ODS_SELECTED) != 0;
    const bool hover    = GetPropW(d->hwndItem, kPropHover) != nullptr;

    RECT rc = d->rcItem;
    // 必须整块重绘：owner-draw 按钮的默认底色是系统按钮灰，留空会露出灰方块
    FillSolid(d->hDC, rc, bg);
    if (hover || pressed) {
        RECT t = rc;
        InflateRect(&t, -dpi::S(1), -dpi::S(1));
        FillRoundRect(d->hDC, t, dpi::S(design::radius::Pill),
                      pressed ? c::AccentSoft : c::Hover);
    }
    DrawGlyphCentered(d->hDC, glyph, rc,
                      disabled ? c::Text3
                               : (hover || pressed ? c::Text : accent),
                      res::IconLg);
}

// ---------------------------------------------------------------- 搜索框

namespace {
constexpr wchar_t kPropClear[] = L"MzSearchClear";
constexpr wchar_t kPropCue[] = L"MzFieldCue";

// EDIT 子类：默认绘制完成后，在空控件上补画占位文字。
// 只画在背景上（空文本时背景是纯色），不会与正文重叠。
LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR) {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_PAINT && GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
        auto* cue = static_cast<const wchar_t*>(GetPropW(hwnd, kPropCue));
        if (cue && *cue) {
            HDC dc = GetDC(hwnd);
            RECT rc; GetClientRect(hwnd, &rc);
            RECT tr = { rc.left + dpi::S(1), rc.top, rc.right - dpi::S(2), rc.bottom };
            DrawText(dc, cue, tr,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                     design::color::Text3, res::Body);
            ReleaseDC(hwnd, dc);
        }
    }
    return r;
}
} // namespace

void SetFieldPlaceholder(HWND edit, const wchar_t* text) {
    if (!edit) return;
    SetPropW(edit, kPropCue, (HANDLE)text);
    SetWindowSubclass(edit, SearchEditProc, 1, 0);
    InvalidateRect(edit, nullptr, FALSE);
}

HWND CreateSearchBox(HWND parent, int editId, int clearId, HINSTANCE hInst) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 0, 0, parent, (HMENU)(INT_PTR)editId, hInst, nullptr);
    // 去掉 3D 边框，由父窗口统一绘制圆角框
    LONG st = GetWindowLongW(edit, GWL_STYLE);
    SetWindowLongW(edit, GWL_STYLE, st & ~WS_BORDER);
    LONG ex = GetWindowLongW(edit, GWL_EXSTYLE);
    SetWindowLongW(edit, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE);
    SetFieldPlaceholder(edit, mz::lang::T(L"搜索功能…  (Ctrl+F)"));

    HWND clear = CreateWindowExW(0, L"BUTTON", L"\xE894",
                                 WS_CHILD | BS_OWNERDRAW,
                                 0, 0, 0, 0, parent, (HMENU)(INT_PTR)clearId, hInst, nullptr);
    InstallButtonHover(clear);

    SetPropW(edit, kPropClear, (HANDLE)clear);
    return edit;
}

void LayoutSearchBox(HWND edit, HWND clearBtn, const RECT& rc) {
    if (!edit) return;
    int clearW = 22;
    MoveWindow(edit, rc.left, rc.top, (rc.right - rc.left) - clearW, rc.bottom - rc.top, TRUE);
    if (clearBtn)
        MoveWindow(clearBtn, rc.right - clearW, rc.top, clearW, rc.bottom - rc.top, TRUE);
    RefreshSearchBox(edit, clearBtn);
}

void RefreshSearchBox(HWND edit, HWND clearBtn) {
    if (!edit || !clearBtn) return;
    bool has = GetWindowTextLengthW(edit) > 0;
    ShowWindow(clearBtn, has ? SW_SHOW : SW_HIDE);
}

void DrawSearchClearButton(const DRAWITEMSTRUCT* d) {
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    const bool hover = GetPropW(d->hwndItem, kPropHover) != nullptr;
    RECT rc = d->rcItem;

    if (hover || pressed) {
        int r = (rc.bottom - rc.top) / 2;
        int cx = (rc.left + rc.right) / 2;
        int cy = (rc.top + rc.bottom) / 2;
        RECT bg = { cx - r, cy - r, cx + r, cy + r };
        FillRoundRect(d->hDC, bg, r, pressed ? design::color::Hairline
                                             : design::color::Hover);
    }
    DrawGlyphCentered(d->hDC, 0xE894, rc, design::color::Text2, res::Icon);
}

// ---------------------------------------------------------------- Toast

namespace {

constexpr wchar_t kToastClass[] = L"MzToastWnd";
constexpr int kToastPadX = 14;
constexpr int kToastPadY = 12;

enum class ToastPhase { In, Hold, Out, Done };

struct ToastState {
    HWND parent = nullptr;
    std::wstring text;
    COLORREF accent = design::color::Accent;
    wchar_t glyph = 0xE73E;
    int alpha = 0;
    int tick = 0;
    ToastPhase phase = ToastPhase::In;
    bool layered = false;
};

ToastState g_toast;

LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        DoubleBuffer db;
        HDC mem = db.Begin(dc, rc);
        if (mem) {
            FillRoundRect(mem, rc, dpi::S(design::radius::Card), design::color::Canvas);
            StrokeRoundRect(mem, rc, dpi::S(design::radius::Card),
                            design::color::Hairline, 1);
            // 图标底：AccentSoft 柔光圆底
            int pad = dpi::S(kToastPadX);
            int box = dpi::S(30);
            RECT ir = { rc.left + pad, (rc.top + rc.bottom - box) / 2,
                        rc.left + pad + box, (rc.top + rc.bottom + box) / 2 };
            FillRoundRect(mem, ir, box / 2, design::color::AccentSoft);
            DrawGlyphCentered(mem, g_toast.glyph, ir, g_toast.accent, res::Icon);
            // 文本（最多两行）
            RECT tr = { ir.right + dpi::S(10), rc.top + dpi::S(kToastPadY),
                        rc.right - pad, rc.bottom - dpi::S(kToastPadY) };
            DrawText(mem, g_toast.text.c_str(), tr,
                     DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX,
                     design::color::Text, res::Body);
            db.End(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        namespace motion = design::motion;
        if (g_toast.phase == ToastPhase::In) {
            g_toast.alpha += 255 * motion::Frame / motion::ToastIn;
            if (g_toast.alpha >= 255) {
                g_toast.alpha = 255;
                g_toast.phase = ToastPhase::Hold;
                g_toast.tick = 0;
            }
        } else if (g_toast.phase == ToastPhase::Hold) {
            g_toast.tick += motion::Frame;
            if (g_toast.tick >= motion::ToastHold) g_toast.phase = ToastPhase::Out;
        } else if (g_toast.phase == ToastPhase::Out) {
            g_toast.alpha -= 255 * motion::Frame / motion::ToastOut;
            if (g_toast.alpha <= 0) { DestroyWindow(hwnd); return 0; }
        }
        if (g_toast.layered)
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_toast.alpha, LWA_ALPHA);
        return 0;
    }
    case WM_LBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SizeToastToText(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    RECT rc = { 0, 0, dpi::S(design::size::ToastW) - dpi::S(kToastPadX) * 2
                     - dpi::S(30) - dpi::S(10), 0 };
    Selected sf(dc, res::Body);
    int hgt = DrawTextW(dc, g_toast.text.c_str(), -1, &rc,
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    ReleaseDC(hwnd, dc);
    int w = dpi::S(design::size::ToastW);
    int h = hgt + dpi::S(kToastPadY) * 2;
    if (h < dpi::S(52)) h = dpi::S(52);
    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

} // namespace

void ShowToast(HWND parent, const std::wstring& text, COLORREF accent, wchar_t glyph) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ToastWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kToastClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    DismissToast();

    g_toast.parent = parent;
    g_toast.text = text;
    g_toast.accent = accent;
    g_toast.glyph = glyph;
    g_toast.alpha = 0;
    g_toast.tick = 0;
    g_toast.phase = ToastPhase::In;

    int w = dpi::S(design::size::ToastW);
    int h = dpi::S(48);
    RECT pr; GetWindowRect(parent, &pr);
    int x = pr.right - w - dpi::S(design::space::XL);
    int y = pr.bottom - h - dpi::S(design::space::XXL + 30);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kToastClass, L"",
                                WS_POPUP, x, y, w, h, parent, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    // 圆角（不支持分层的老系统也能有形状）
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                  design::radius::Card, design::radius::Card);
    SetWindowRgn(hwnd, rgn, TRUE);

    SizeToastToText(hwnd);
    GetWindowRect(parent, &pr);
    // 文本可能换行把气泡撑大，定位要用调整后的真实尺寸，否则底边会压到状态栏
    RECT tr = { 0, 0, w, h };
    GetWindowRect(hwnd, &tr);
    const int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    SetWindowPos(hwnd, nullptr, pr.right - tw - dpi::S(design::space::XL),
                 pr.bottom - th - dpi::S(design::space::XXL + 30),
                 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);

    if (SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA))
        g_toast.layered = true;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, design::motion::Frame, nullptr);
}

void DismissToast() {
    HWND existing = FindWindowW(kToastClass, nullptr);
    if (existing && GetParent(existing) == g_toast.parent) DestroyWindow(existing);
}

// ---------------------------------------------------------------- 日志控制台
//  自绘行缓冲视图。核心决策见 controls.h 注释：EDIT 滚动降级是系统行为，
//  无法从应用层关闭，因此整体自绘，保证任何滚动操作下文字始终清晰。

namespace {

constexpr wchar_t kLogViewClass[] = L"MzLogView";
constexpr int kMaxLogLines = 4000;   // 行数上限，超出裁掉最旧的

struct LogViewData {
    std::vector<std::wstring> lines;
    int scrollPos = 0;     // 首个可见行索引
    int lineH = 0;         // 行高（缓存，随字体/DPI 变化重算）
    HFONT font = nullptr;  // 上次计算行高所用字体（变化即重算）
    bool follow = true;    // 跟随底部：追加时不抢用户正在看的历史
    bool dragging = false; // 正在拖动自绘滚动条
    int dragY = 0;
    int dragStart = 0;
};

// 行高 / 可见行数缓存：res::Mono 被重建（DPI 切换）时自动重算
void SyncMetrics(LogViewData* d, HWND hwnd) {
    if (d->font == res::Mono && d->lineH > 0) return;
    d->font = res::Mono;
    HDC dc = GetDC(hwnd);
    HFONT f = d->font ? d->font : (HFONT)GetStockObject(SYSTEM_FONT);
    Selected sel(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    ReleaseDC(hwnd, dc);
    d->lineH = tm.tmHeight + dpi::S(9);
}

int VisibleLines(const LogViewData* d, HWND hwnd) {
    if (d->lineH <= 0) return 0;
    RECT rc; GetClientRect(hwnd, &rc);
    int h = rc.bottom - rc.top;
    return h > 0 ? h / d->lineH : 0;
}

int MaxScrollPos(const LogViewData* d, HWND hwnd) {
    int total = (int)d->lines.size();
    int vis = VisibleLines(d, hwnd);
    return total > vis ? total - vis : 0;
}

void UpdateScrollBar(LogViewData* d, HWND hwnd) {
    int vis = VisibleLines(d, hwnd);
    int total = (int)d->lines.size();
    int maxPos = total > vis ? total - vis : 0;
    if (d->scrollPos > maxPos) d->scrollPos = maxPos;
    if (d->scrollPos < 0) d->scrollPos = 0;
}

// 自绘滚动条几何：轨道在右侧，滑块最小 28px
RECT LogThumbRect(const LogViewData* d, HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int vis = VisibleLines(d, hwnd);
    int total = (int)d->lines.size();
    RECT track = { rc.right - dpi::S(9), rc.top + dpi::S(6),
                   rc.right - dpi::S(4), rc.bottom - dpi::S(6) };
    int trackH = track.bottom - track.top;
    if (total <= vis || trackH <= 0) return RECT{ 0, 0, 0, 0 };
    int thumbH = std::max(dpi::S(28), trackH * vis / total);
    if (thumbH > trackH) thumbH = trackH;
    int maxPos = total - vis;
    int y = track.top + (maxPos > 0 ? (trackH - thumbH) * d->scrollPos / maxPos : 0);
    return RECT{ track.left, y, track.right, y + thumbH };
}

// 滚动后立即整窗重绘（RDW_UPDATENOW 同步走完 WM_PAINT）——
// 每一帧都是完整双缓冲绘制，从机制上消除滚动拖影
void ScrollAndRepaint(LogViewData* d, HWND hwnd, int newPos) {
    int maxPos = MaxScrollPos(d, hwnd);
    if (newPos > maxPos) newPos = maxPos;
    if (newPos < 0) newPos = 0;
    if (newPos == d->scrollPos) return;
    d->scrollPos = newPos;
    d->follow = (newPos >= maxPos);   // 回到底部恢复跟随，离开暂停
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void PaintLogView(LogViewData* d, HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);

    ui::DoubleBuffer db;
    HDC mem = db.Begin(dc, rc);
    if (mem) {
        SyncMetrics(d, hwnd);
        FillSolid(mem, rc, design::color::Canvas);
        // 内描边极淡：日志与卡片同底，只留一圈呼吸边界
        StrokeRoundRect(mem, rc, 0, design::color::Hairline);

        if (d->lines.empty()) {
            ui::DrawText(mem, lang::T(L"暂无日志"),
                         RECT{ dpi::S(14), dpi::S(10), rc.right, rc.bottom },
                         DT_LEFT | DT_TOP | DT_NOPREFIX,
                         design::color::Text3, res::Mono);
        } else {
            int vis = VisibleLines(d, hwnd);
            if (d->scrollPos > (int)d->lines.size() - vis)
                d->scrollPos = (int)d->lines.size() - vis;
            if (d->scrollPos < 0) d->scrollPos = 0;
            int y = rc.top + dpi::S(6);
            int end = (int)d->lines.size();
            for (int i = d->scrollPos; i < end; ++i, y += d->lineH) {
                if (y >= rc.bottom) break;
                const wchar_t* s = d->lines[i].c_str();
                // 通道色语义（仅日志层，禁做按钮/标题）：
                //   ✓ → ChIn（进/成功）  ✗ /「错误」 → ChErr（异常）
                //   「提示」 → GoldText（信号小字）   ▸ → Text2（动作行）
                COLORREF c = design::color::Text;
                if (s[0] == L'\u2713') c = design::color::ChIn;
                else if (s[0] == L'\u2717' || std::wcsstr(s, L"错误")) c = design::color::ChErr;
                else if (std::wcsstr(s, L"提示")) c = design::color::GoldText;
                else if (s[0] == L'\u25B8') c = design::color::Text2;
                ui::DrawText(mem, s,
                             RECT{ dpi::S(14), y, rc.right - dpi::S(14), y + d->lineH },
                             DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX, c, res::Mono);
            }
        }

        // 自绘窄滚动条：细轨 Hairline，拖动态提亮
        RECT th = LogThumbRect(d, hwnd);
        if (th.right > th.left) {
            int r = (th.right - th.left) / 2;
            ui::FillRoundRect(mem, th, r,
                              d->dragging ? design::color::Text3
                                          : design::color::Hairline);
        }
        db.End(dc);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK LogViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<LogViewData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new LogViewData()));
        return 0;
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_PAINT:
        if (d) PaintLogView(d, hwnd);
        else DefWindowProcW(hwnd, msg, wp, lp);
        return 0;
    case WM_ERASEBKGND:
        return 1;   // 双缓冲兜底，禁系统擦背景
    case WM_SIZE:
        if (d) { SyncMetrics(d, hwnd); UpdateScrollBar(d, hwnd);
                 InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_VSCROLL: {
        if (!d || !d->lineH) return 0;
        int vis = VisibleLines(d, hwnd);
        int pos = d->scrollPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:    pos -= 1; break;
        case SB_LINEDOWN:  pos += 1; break;
        case SB_PAGEUP:    pos -= vis; break;
        case SB_PAGEDOWN:  pos += vis; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            // 位置取自消息 wParam 的 HIWORD：真实鼠标拖动与程序化发送都可靠。
            // （GetScrollInfo 的 nTrackPos 只在真实拖动时由系统更新，不可依赖）
            pos = (int)(short)HIWORD(wp);
            break;
        }
        case SB_TOP:    pos = 0; break;
        case SB_BOTTOM: pos = MaxScrollPos(d, hwnd); break;
        default: return 0;
        }
        ScrollAndRepaint(d, hwnd, pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!d || !d->lineH) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        ScrollAndRepaint(d, hwnd, d->scrollPos + (delta > 0 ? -3 : 3));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!d) return 0;
        int vis = VisibleLines(d, hwnd);
        int total = (int)d->lines.size();
        if (total <= vis) return 0;
        POINT p = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        RECT th = LogThumbRect(d, hwnd);
        RECT hit = { th.left - dpi::S(4), th.top, th.right + dpi::S(4), th.bottom };
        if (PtInRectStrict(hit, p.x, p.y)) {
            d->dragging = true;
            d->dragY = p.y;
            d->dragStart = d->scrollPos;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!d || !d->dragging) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        int vis = VisibleLines(d, hwnd);
        int total = (int)d->lines.size();
        int maxPos = total - vis;
        RECT th = LogThumbRect(d, hwnd);
        int trackH = (rc.bottom - dpi::S(6)) - (rc.top + dpi::S(6));
        int travel = trackH - (th.bottom - th.top);
        if (travel < 1) travel = 1;
        int y = (int)(short)HIWORD(lp);
        ScrollAndRepaint(d, hwnd, d->dragStart + (y - d->dragY) * maxPos / travel);
        return 0;
    }
    case WM_LBUTTONUP:
        if (d && d->dragging) {
            d->dragging = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateLogView(HWND parent, int id, HINSTANCE hInst) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = LogViewProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kLogViewClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // 全部自绘
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kLogViewClass, L"",
                           WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

void LogViewAppendBatch(HWND h, const std::vector<std::wstring>& lines) {
    if (!h) return;
    auto* d = reinterpret_cast<LogViewData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!d || lines.empty()) return;
    bool wasFollow = d->follow;
    d->lines.insert(d->lines.end(), lines.begin(), lines.end());
    if ((int)d->lines.size() > kMaxLogLines)
        d->lines.erase(d->lines.begin(), d->lines.end() - kMaxLogLines);
    SyncMetrics(d, h);
    if (wasFollow) d->scrollPos = MaxScrollPos(d, h);
    UpdateScrollBar(d, h);
    InvalidateRect(h, nullptr, FALSE);   // 合并为一次 WM_PAINT，旧内容不动
}

void LogViewClear(HWND h) {
    if (!h) return;
    auto* d = reinterpret_cast<LogViewData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!d) return;
    d->lines.clear();
    d->scrollPos = 0;
    d->follow = true;
    UpdateScrollBar(d, h);
    InvalidateRect(h, nullptr, FALSE);
}

// ---------------------------------------------------------------- 其它

void FillSolid(HDC dc, const RECT& rc, COLORREF c) {
    FillRect(dc, &rc, Brush(c));
}

bool PtInRectStrict(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

} // namespace ui
} // namespace mz
