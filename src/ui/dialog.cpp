// ============================================================================
//  模态对话框框架 v2.17（自绘，VI-D2 令牌体系）
//  ---------------------------------------------------------------------------
//    * ModalHost        —— 模态宿主：圆角卡片 + 键盘可达（Enter/Esc/Tab）
//    * ConfirmPage      —— 确认框（危险键 = 墨底反白，不用红）
//    * AboutPage        —— 关于页：蝶形 + Baloo 2 字标（金点=焊盘）+
//                          蜕变四态叙事 + OFL 脚注
//    * ThemePickerPage  —— 主题选择（纸亮 / 墨暗 2 卡，迷你预览）
// ============================================================================
#include "ui/dialog.h"
#include "ui/design.h"
#include "ui/dpi.h"
#include "ui/gdi.h"
#include "ui/brand.h"
#include "lang.h"

#include <commctrl.h>
#include <shellapi.h>

using namespace mz;
namespace d = mz::ui::design;
namespace color = mz::ui::design::color;

namespace mz {
namespace ui {

using mz::lang::T;
using mz::lang::Tx;

namespace {

constexpr wchar_t kModalClass[] = L"MzModalHost";

struct ModalHost {
    HWND hwnd = nullptr;
    HWND btnOk = nullptr, btnCancel = nullptr, btnExtra = nullptr, btnClose = nullptr;
    ModalPage* page = nullptr;
    ModalOptions opt;
    HBRUSH bSurface = nullptr;
    RECT contentRc = {};
    RECT rcClose = {};
};

// owner-draw 按钮的键盘支持：Esc=取消、Enter=点击当前、Tab=切换焦点
LRESULT CALLBACK ModalBtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        HWND parent = GetParent(hwnd);
        if (wp == VK_ESCAPE) {
            SendMessageW(parent, WM_COMMAND, (WPARAM)kModalCancel, 0);
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_SPACE) {
            SendMessageW(parent, WM_COMMAND, (WPARAM)GetDlgCtrlID(hwnd), 0);
            return 0;
        }
        if (wp == VK_TAB) {
            bool prev = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            HWND next = GetNextDlgTabItem(parent, hwnd, prev);
            if (next) SetFocus(next);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ModalWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ModalHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<ModalHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->hwnd = hwnd;
        st->bSurface = CreateSolidBrush(color::Canvas);

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        auto mkBtn = [&](const wchar_t* t, int id, int w) {
            HWND b = CreateWindowExW(0, L"BUTTON", t,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, w, dpi::S(36), hwnd, (HMENU)(INT_PTR)id,
                                     hInst, nullptr);
            ui::InstallButtonHover(b);
            SetWindowSubclass(b, ModalBtnProc, 1, 0);
            SendMessageW(b, WM_SETFONT, (WPARAM)ui::res::Body, TRUE);
            return b;
        };
        st->btnOk = mkBtn(st->opt.okText.c_str(), kModalOk, dpi::S(124));
        st->btnCancel = mkBtn(st->opt.cancelText.c_str(), kModalCancel, dpi::S(124));
        st->btnExtra = mkBtn(L"访问 biohacker.fun", kModalExtra, dpi::S(180));
        st->btnClose = mkBtn(L"\xE894", kModalClose, dpi::S(40));
        SetWindowLongW(st->btnClose, GWL_STYLE,
                       GetWindowLongW(st->btnClose, GWL_STYLE) & ~WS_TABSTOP);
        ShowWindow(st->btnExtra, st->opt.showExtra ? SW_SHOW : SW_HIDE);
        ShowWindow(st->btnCancel, st->opt.showCancel ? SW_SHOW : SW_HIDE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        ui::DoubleBuffer db;
        HDC mem = db.Begin(dc, rc);
        if (mem && st) {
            FillSolid(mem, rc, color::Canvas);
            StrokeRoundRect(mem, rc, dpi::S(d::radius::Card), color::Hairline, 1);
            // 标题栏：同底 + 细线，标题 22/700
            RECT titleRc = { 0, 0, rc.right, dpi::S(60) };
            FillSolid(mem, titleRc, color::Canvas);
            RECT tl = { dpi::S(24), titleRc.bottom - 1, rc.right - dpi::S(24),
                        titleRc.bottom };
            FillSolid(mem, tl, color::Hairline);
            wchar_t title[128] = { 0 };
            GetWindowTextW(hwnd, title, 128);
            ui::DrawText(mem, title,
                         RECT{ dpi::S(24), 0, rc.right - dpi::S(64), dpi::S(60) },
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                         color::Text, ui::res::Title);
            // 底栏分隔线
            RECT bl = { dpi::S(24), rc.bottom - dpi::S(78), rc.right - dpi::S(24),
                        rc.bottom - dpi::S(78) };
            FillSolid(mem, bl, color::Hairline);
            st->page->Draw(mem, st->contentRc);
            db.End(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE: {
        if (!st) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        int bw = dpi::S(116), gap = dpi::S(10), pad = dpi::S(24);
        int by = rc.bottom - dpi::S(78) + dpi::S(17);
        int x = rc.right - pad - bw;
        MoveWindow(st->btnOk, x, by, bw, dpi::S(44), TRUE);
        x -= bw + gap;
        // 用 WS_VISIBLE 样式位判断，不能用 IsWindowVisible：
        // 后者在父窗口尚未显示时返回 FALSE，会让按钮永远停在 (0,0)
        if (GetWindowLongW(st->btnCancel, GWL_STYLE) & WS_VISIBLE)
            MoveWindow(st->btnCancel, x, by, bw, dpi::S(44), TRUE);
        if (GetWindowLongW(st->btnExtra, GWL_STYLE) & WS_VISIBLE)
            MoveWindow(st->btnExtra, pad, by, dpi::S(190), dpi::S(44), TRUE);
        // 标题栏关闭按钮（右上角）
        int cb = dpi::S(34);
        SetRect(&st->rcClose, rc.right - pad - cb + dpi::S(6), dpi::S(13),
                rc.right - pad + dpi::S(6), dpi::S(13) + cb);
        MoveWindow(st->btnClose, st->rcClose.left, st->rcClose.top,
                   cb, cb, TRUE);
        st->contentRc = { pad, dpi::S(70), rc.right - pad, rc.bottom - dpi::S(94) };
        st->page->Layout(st->contentRc);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!st) break;
        if (d->CtlID == kModalExtra) {
            ui::DrawFluentButton(d, ui::ButtonStyle::Ghost, color::Accent, 0xE774,
                                 color::Canvas);
        } else if (d->CtlID == kModalClose) {
            // 关闭按钮：透明底 + ✕ 字形，悬停出圆底
            RECT rc = d->rcItem;
            bool pressed = (d->itemState & ODS_SELECTED) != 0;
            bool hover = GetPropW(d->hwndItem, L"MzBtnHover") != nullptr;
            FillSolid(d->hDC, rc, color::Canvas);
            if (hover || pressed) {
                int r = (rc.bottom - rc.top) / 2;
                int cx = (rc.left + rc.right) / 2;
                int cy = (rc.top + rc.bottom) / 2;
                RECT bg = { cx - r, cy - r, cx + r, cy + r };
                ui::FillRoundRect(d->hDC, bg, r,
                                  pressed ? color::Hairline : color::Hover);
            }
            ui::DrawGlyphCentered(d->hDC, 0xE894, rc, color::Text2, ui::res::Icon);
            return TRUE;
        } else if (d->CtlID == kModalOk && st->opt.dangerOk) {
            // 危险确认主键 = 墨底反白（VI 纪律：红色只归数据层徽标）
            ui::DrawFluentButton(d, ui::ButtonStyle::Primary, color::Accent, 0,
                                 color::Canvas);
        } else {
            // 对话框底是 Canvas，不传就会在圆角外露出一圈系统灰
            ui::DrawFluentButton(d, d->CtlID == kModalOk ? ui::ButtonStyle::Primary
                                                         : ui::ButtonStyle::Secondary,
                                 color::Accent, 0, color::Canvas);
        }
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        if (st && st->page) {
            if (st->page->OnMouseMove((int)(short)LOWORD(lp), (int)(short)HIWORD(lp)))
                InvalidateRect(hwnd, &st->contentRc, FALSE);
            if (!GetPropW(hwnd, L"MzModalHover")) {
                SetPropW(hwnd, L"MzModalHover", (HANDLE)1);
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"MzModalHover");
        if (st && st->page && st->page->OnMouseLeave())
            InvalidateRect(hwnd, &st->contentRc, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        if (st && st->page) {
            int mx = (int)(short)LOWORD(lp);
            int my = (int)(short)HIWORD(lp);
            st->page->OnClick(mx, my);
            // 页面在 OnClick 里设置了 result（如选中卡片）则按命令关闭
            if (st->page->result == kModalOk &&
                st->page->OnCommand(kModalOk)) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (st && st->page && (id == kModalOk || id == kModalCancel ||
                               id == kModalExtra || id == kModalClose)) {
            // 关闭按钮等同取消；按钮 ID 已由框架写入 page->result
            st->page->result = (id == kModalClose) ? kModalCancel : id;
            if (st->page->OnCommand(st->page->result)) DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (st && st->page) {
            if (wp == VK_RETURN) {
                st->page->result = kModalOk;
                if (st->page->OnCommand(kModalOk)) DestroyWindow(hwnd);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                st->page->result = kModalCancel;
                if (st->page->OnCommand(kModalCancel)) DestroyWindow(hwnd);
                return 0;
            }
            st->page->OnKey(wp);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st && st->bSurface) DeleteObject(st->bSurface);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int RunModal(HWND host, const std::wstring& title, int width, int height,
             ModalPage* page, const ModalOptions& opt) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ModalWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kModalClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    ModalHost st;
    st.page = page;
    st.opt = opt;

    RECT hr; GetWindowRect(host, &hr);
    int w = dpi::S(width), h = dpi::S(height);
    int x = hr.left + ((hr.right - hr.left) - w) / 2;
    int y = hr.top + ((hr.bottom - hr.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kModalClass, title.c_str(),
                                WS_POPUP, x, y, w, h,
                                host, nullptr, GetModuleHandleW(nullptr), &st);
    if (!hwnd) return 0;

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, dpi::S(d::radius::Card),
                                  dpi::S(d::radius::Card));
    SetWindowRgn(hwnd, rgn, TRUE);

    EnableWindow(host, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(host, TRUE);
    SetForegroundWindow(host);
    return st.page->result;
}

// ---------------------------------------------------------------- 确认页
namespace {

class ConfirmPage : public ModalPage {
public:
    ConfirmPage(std::wstring body, bool danger)
        : body_(std::move(body)), danger_(danger) {}

    void Draw(HDC dc, const RECT& rc) override {
        // 图标：危险 = 金警示（信号语义，不上红）；普通 = 藤紫信息
        const int box = dpi::S(44);
        RECT cir = { rc.left, rc.top + dpi::S(2), rc.left + box, rc.top + dpi::S(2) + box };
        ui::FillRoundRect(dc, cir, box / 2, color::AccentSoft);
        ui::DrawGlyphCentered(dc, danger_ ? 0xE7BA : 0xE946, cir,
                              danger_ ? color::GoldText : color::Accent,
                              ui::res::IconLg);
        RECT tr = { rc.left + box + dpi::S(16), rc.top - dpi::S(2),
                    rc.right, rc.bottom };
        ui::DrawText(dc, body_.c_str(), tr,
                     DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX,
                     color::Text2, ui::res::Body);
    }

    // 确认/取消/关闭一律关闭对话框（result 已由框架写入）
    bool OnCommand(int /*id*/) override { return true; }

private:
    std::wstring body_;
    bool danger_;
};

} // namespace

bool ConfirmDialog(HWND host, const std::wstring& title, const std::wstring& body,
                   const std::wstring& okText, const std::wstring& cancelText,
                   bool danger) {
    using mz::lang::T;
    ConfirmPage page(body, danger);
    ModalOptions opt;
    opt.okText = okText.empty() ? T(L"确定") : okText;
    opt.cancelText = cancelText.empty() ? T(L"取消") : cancelText;
    opt.showCancel = true;
    opt.dangerOk = danger;
    return RunModal(host, title, 620, 280, &page, opt) == kModalOk;
}

// ---------------------------------------------------------------- 关于页
namespace {

class AboutPage : public ModalPage {
public:
    explicit AboutPage(int moduleCount) : modules_(moduleCount) {}

    void Layout(const RECT& rc) override { rc_ = rc; }

    void Draw(HDC dc, const RECT& rc) override {
        int y = rc.top;

        // ---- 蝶形标志（变体 A / 墨暗 C）+ 字标（金点=焊盘，不可拆）----
        const design::ThemeTokens* t = d::CurrentTheme();
        RECT mark = { rc.left, y, rc.left + dpi::S(96), y + dpi::S(64) };
        brand::DrawButterfly(dc, mark,
                             t->Dark ? brand::MarkVariant::ReverseC
                                     : brand::MarkVariant::FullA,
                             *t);

        RECT wm = { rc.left + dpi::S(108), y - dpi::S(4), rc.right, y + dpi::S(26) };
        brand::DrawWordmark(dc, wm, *t);
        ui::DrawText(dc, d::brand::kAppNameEn,
                     RECT{ rc.left + dpi::S(110), y + dpi::S(28), rc.right,
                           y + dpi::S(48) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text2, ui::res::Body);
        ui::DrawText(dc, d::brand::kPublisherCn,
                     RECT{ rc.left + dpi::S(110), y + dpi::S(48), rc.right,
                           y + dpi::S(66) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text3, ui::res::Small);

        wchar_t ver[32];
        wsprintfW(ver, L"v%d.%d", k_AppVersionMajor, k_AppVersionMinor);
        ui::DrawText(dc, ver,
                     RECT{ rc.right - dpi::S(90), y, rc.right, y + dpi::S(24) },
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::GoldText, ui::res::Title);

        y += dpi::S(76);
        ui::FillSolid(dc, RECT{ rc.left, y, rc.right, y + 1 }, color::Hairline);

        y += dpi::S(14);
        ui::DrawText(dc, d::brand::kTaglineCn,
                     RECT{ rc.left, y, rc.right, y + dpi::S(22) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text2, ui::res::Small);

        y += dpi::S(30);
        wstring m1 = Tx(L"%d 个功能模块，覆盖清理 / 网络 / 系统信息");
        wstring m1s;
        {
            // %d 替换后绘制（Tx 的英文表项同样带 %d）
            wstring n = std::to_wstring(modules_);
            size_t pct = m1.find(L"%d");
            m1s = pct == wstring::npos ? m1
                : m1.substr(0, pct) + n + m1.substr(pct + 2);
        }
        const wchar_t* facts[] = {
            m1s.c_str(),
            T(L"单文件免安装，无服务、无驱动、不写系统目录"),
            T(L"全部本地执行，不上传任何数据"),
        };
        for (const wchar_t* f : facts) {
            RECT dot = { rc.left, y + dpi::S(1), rc.left + dpi::S(18), y + dpi::S(19) };
            ui::FillRoundRect(dc, dot, dpi::S(9), color::AccentSoft);
            ui::DrawGlyphCentered(dc, 0xE73E, dot, color::ChIn, ui::res::IconSm);
            ui::DrawText(dc, f,
                         RECT{ rc.left + dpi::S(28), y, rc.right, y + dpi::S(20) },
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                         color::Text2, ui::res::Small);
            y += dpi::S(28);
        }

        // ---- 蜕变四态叙事（卵 v0.1 → 幼虫 v0.5 → 蛹 v0.9 → 蝶 v1.0）----
        y += dpi::S(6);
        // Bug-B1：带底与脚注行（fy = rc.bottom - S(20)）保留 ≥ S(12) 间距
        const int tlBottom = (y + dpi::S(78) < rc.bottom - dpi::S(32))
                                 ? y + dpi::S(78)
                                 : rc.bottom - dpi::S(32);
        RECT tl = { rc.left, y, rc.right, tlBottom };
        brand::DrawMorphTimeline(dc, tl, *t);

        // ---- 脚注：MIT / OFL / © ----
        int fy = rc.bottom - dpi::S(20);
        ui::DrawText(dc, L"MIT License",
                     RECT{ rc.left, fy, rc.left + dpi::S(220), rc.bottom },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text3, ui::res::Small);
        ui::DrawText(dc, T(L"Baloo 2 / JetBrains Mono 以 SIL Open Font License 1.1 子集形式内嵌"),
                     RECT{ rc.left + dpi::S(220), fy, rc.right - dpi::S(180), rc.bottom },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                         DT_END_ELLIPSIS,
                     color::Text3, ui::res::Small);
        ui::DrawText(dc, L"© 2026 biohacker.fun",
                     RECT{ rc.right - dpi::S(180), fy, rc.right, rc.bottom },
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text3, ui::res::Small);
    }

    bool OnCommand(int id) override {
        if (id == kModalExtra) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"open";
            sei.lpFile = d::brand::kSiteUrl;
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            return false;
        }
        return true;
    }

private:
    int modules_;
    RECT rc_ = {};
};

} // namespace

void AboutDialog(HWND host, int moduleCount) {
    AboutPage page(moduleCount);
    ModalOptions opt;
    opt.showCancel = false;
    opt.showExtra = true;
    RunModal(host, T(L"关于"), 640, 464, &page, opt);   // 464：时间线带底与脚注留足间距
}

// ---------------------------------------------------------------- 主题选择页
namespace {

// 单张主题卡片：迷你应用预览（侧栏 + 内容卡 + 主键）+ 名称 + 描述。
// 卡片内的色彩一律使用「该主题自己的」角色令牌，暗色卡片上才看得清。
void DrawThemeCard(HDC dc, const RECT& card, const design::ThemeTokens& t,
                   bool current, bool hovered) {
    const int pad = dpi::S(12);

    ui::FillRoundRect(dc, card, dpi::S(d::radius::Card), t.Canvas);
    ui::StrokeRoundRect(dc, card, dpi::S(d::radius::Card),
                        current ? t.Accent : (hovered ? t.Text3 : t.Hairline),
                        current ? 2 : 1);

    // ---- 迷你界面预览 ----
    const int pvH = dpi::S(96);
    RECT pv = { card.left + pad, card.top + pad, card.right - pad,
                card.top + pad + pvH };
    ui::FillRoundRect(dc, pv, dpi::S(8), t.Canvas);
    ui::StrokeRoundRect(dc, pv, dpi::S(8), t.Hairline, 1);

    int sw = (pv.right - pv.left) * 3 / 10;
    RECT sb = { pv.left + 1, pv.top + 1, pv.left + sw, pv.bottom - 1 };
    ui::FillRoundRect(dc, sb, dpi::S(7), t.Side);
    ui::FillSolid(dc, RECT{ sb.right, sb.top, sb.right + 1, sb.bottom }, t.Hairline);

    // 侧栏：品牌蝶形角标 + 模块行（第一行选中 = Accent 胶囊）
    {
        RECT bf = { sb.left + dpi::S(6), sb.top + dpi::S(5),
                    sb.left + dpi::S(24), sb.top + dpi::S(19) };
        brand::DrawButterfly(dc, bf, t.Dark ? brand::MarkVariant::ReverseC
                                            : brand::MarkVariant::FullA, t);
    }
    for (int i = 0; i < 4; ++i) {
        RECT ni = { sb.left + dpi::S(6), sb.top + dpi::S(26) + i * dpi::S(12),
                    sb.right - dpi::S(6), sb.top + dpi::S(36) + i * dpi::S(12) };
        ui::FillRoundRect(dc, ni, dpi::S(6),
                          i == 0 ? t.Accent : (i == 1 ? t.Hover : t.Side));
        // 选中行补一枚 OnAccent 短条模拟文字
        if (i == 0)
            ui::FillRoundRect(dc,
                              RECT{ ni.left + dpi::S(8), ni.top + dpi::S(3),
                                    ni.left + dpi::S(26), ni.top + dpi::S(7) },
                              dpi::S(2), t.OnAccent);
    }

    RECT mr = { pv.left + sw + 1, pv.top + 1, pv.right - 1, pv.bottom - 1 };
    ui::FillSolid(dc, mr, t.Canvas);
    ui::FillRoundRect(dc, RECT{ mr.left + dpi::S(8), mr.top + dpi::S(8),
                                mr.left + dpi::S(58), mr.top + dpi::S(13) },
                      dpi::S(2), t.Text);
    RECT mc = { mr.left + dpi::S(8), mr.top + dpi::S(20),
                mr.right - dpi::S(8), mr.bottom - dpi::S(8) };
    ui::FillRoundRect(dc, mc, dpi::S(5), t.Canvas);
    ui::StrokeRoundRect(dc, mc, dpi::S(5), t.Hairline, 1);
    for (int i = 0; i < 2; ++i) {
        ui::FillRoundRect(dc, RECT{ mc.left + dpi::S(6), mc.top + dpi::S(6) + i * dpi::S(8),
                                    mc.right - dpi::S(6) - i * dpi::S(18),
                                    mc.top + dpi::S(10) + i * dpi::S(8) },
                          dpi::S(2), t.Hairline);
    }
    // 主按钮（墨底反白胶囊）+ 金色进度线（信号语义）
    ui::FillRoundRect(dc, RECT{ mc.left + dpi::S(6), mc.bottom - dpi::S(14),
                                mc.left + dpi::S(40), mc.bottom - dpi::S(6) },
                      dpi::S(4), t.BtnFill);
    ui::FillRoundRect(dc, RECT{ mc.right - dpi::S(34), mc.bottom - dpi::S(11),
                                mc.right - dpi::S(6), mc.bottom - dpi::S(9) },
                      dpi::S(1), t.Gold);

    // ---- 名称 / 当前标记 / 描述 ----
    RECT nr = { card.left + pad, pv.bottom + dpi::S(8), card.right - pad - dpi::S(54),
                pv.bottom + dpi::S(32) };
    ui::DrawText(dc, T(t.name), nr,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 t.Text, ui::res::Title);

    if (current) {
        RECT bg = { card.right - pad - dpi::S(50), pv.bottom + dpi::S(9),
                    card.right - pad, pv.bottom + dpi::S(31) };
        ui::FillRoundRect(dc, bg, dpi::S(11), t.AccentSoft);
        ui::DrawText(dc, T(L"当前"), bg,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     t.Text, ui::res::Small);
    }

    ui::DrawText(dc, T(t.desc),
                 RECT{ card.left + pad, pv.bottom + dpi::S(32),
                       card.right - pad, card.bottom - dpi::S(8) },
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS,
                 t.Text2, ui::res::Small);
}

class ThemePickerPage : public ModalPage {
public:
    void Layout(const RECT& rc) override {
        constexpr int kCount = (int)(sizeof(design::kAll) /
                                     sizeof(design::kAll[0]));
        const int gap = dpi::S(16);
        const int top = rc.top + dpi::S(34);
        const int cw = (rc.right - rc.left - gap) / 2;
        const int ch = rc.bottom - top;   // 单行两卡
        cards_.clear();
        for (int i = 0; i < kCount; ++i) {
            RECT c = { rc.left + i * (cw + gap), top,
                       rc.left + i * (cw + gap) + cw, top + ch };
            cards_.push_back(c);
        }
    }

    void Draw(HDC dc, const RECT& rc) override {
        ui::DrawText(dc, T(L"选择界面主题   ·   Ctrl+T 快速循环切换，选择自动保存"),
                     RECT{ rc.left, rc.top, rc.right, rc.top + dpi::S(26) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text2, ui::res::Small);
        for (size_t i = 0; i < cards_.size(); ++i) {
            const design::ThemeTokens* t = design::kAll[i];
            bool cur = (wcscmp(t->id, design::CurrentTheme()->id) == 0);
            DrawThemeCard(dc, cards_[i], *t, cur, (int)i == hover_);
        }
    }

    void OnClick(int x, int y) override {
        for (size_t i = 0; i < cards_.size(); ++i) {
            const RECT& c = cards_[i];
            if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) {
                if (design::kAll[i] != design::CurrentTheme()) {
                    design::ApplyTheme(design::kAll[i]);
                }
                result = kModalOk;   // 点击即应用并关闭
                return;
            }
        }
    }

    bool OnMouseMove(int x, int y) override {
        int hit = -1;
        for (size_t i = 0; i < cards_.size(); ++i) {
            const RECT& c = cards_[i];
            if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) { hit = (int)i; break; }
        }
        if (hit == hover_) return false;
        hover_ = hit;
        return true;
    }

    bool OnMouseLeave() override {
        if (hover_ == -1) return false;
        hover_ = -1;
        return true;
    }

    // 确定/取消/✕ 都关闭（卡片点击是主要路径）
    bool OnCommand(int /*id*/) override { return true; }

private:
    vector<RECT> cards_;
    int hover_ = -1;
};

} // namespace

void ThemePickerDialog(HWND host) {
    ThemePickerPage page;
    ModalOptions opt;
    opt.okText = T(L"完成");
    opt.cancelText = T(L"取消");
    opt.showCancel = false;
    RunModal(host, T(L"界面主题"), 720, 480, &page, opt);
}

} // namespace ui
} // namespace mz
