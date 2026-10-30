#include "ui/appwindow.h"
#include "ui/brand.h"
#include "ui/controls.h"
#include "ui/design.h"
#include "ui/dialog.h"
#include "ui/dpi.h"
#include "ui/gdi.h"
#include "ui/icons.h"
#include "cleaner.h"
#include "common.h"
#include "lang.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <set>

using namespace mz;
using mz::lang::T;
using mz::lang::Tx;
namespace d = mz::ui::design;
namespace dpi = mz::ui::dpi;
namespace color = mz::ui::design::color;
namespace size = mz::ui::design::size;
namespace space = mz::ui::design::space;
namespace brand = mz::ui::brand;
using mz::ui::FillSolid;
using mz::ui::FillRoundRect;
using mz::ui::BlendColor;

namespace mz {
namespace {

// ---------------------------------------------------------------- 控件 ID
enum {
    ID_NAV = 1001,
    ID_LIST = 1002,
    ID_LOG = 1003,
    ID_BTN_RUN = 1010,
    ID_BTN_CLEAN = 1011,
    ID_BTN_UNDO = 1012,
    ID_BTN_STOP = 1013,
    ID_BTN_ADMIN = 1014,
    ID_BTN_ABOUT = 1015,
    ID_BTN_ALL = 1016,
    ID_BTN_NONE = 1017,
    ID_BTN_THEME = 1018,
    ID_BTN_MORE = 1019,     // 扩展动作「更多 ▾」（v2.8）
    ID_PATH = 1020,
    ID_TARGET = 1021,
    ID_SEARCH = 1022,
    ID_SEARCH_CLEAR = 1023,
    ID_NAV_EXPAND = 1024,   // 侧栏「全部展开」（v2.10）
    ID_NAV_COLLAPSE = 1025, // 侧栏「全部收起」（v2.10）
    ID_BTN_LANG = 1026,     // 语言切换（v2.12 R4）
    ID_FIELD0 = 1032,       // 扩展字段输入行（v2.12 R3，最多 4 个）
    ID_FIELD1 = 1033,
    ID_FIELD2 = 1034,
    ID_FIELD3 = 1035,
    ID_PROGRESS = 1030,
    ID_NOMATCH = 1031,
};

constexpr int kMaxFields = 4;   // 扩展字段上限（与 module.h Fields() 契约一致）

// 「更多 ▾」弹出菜单项 ID 基址（ID = kActMenuIdBase + 动作绝对下标）
constexpr int kActMenuIdBase = 3000;

// 「更多 ▾」弹出菜单项数据（owner-draw 绘制，danger 项红色）
struct ActionMenuItem {
    wstring label;
    bool danger = false;
};

constexpr UINT kMsgDone = WM_APP + 1;   // 后台任务完成
constexpr UINT kMsgTick = WM_APP + 2;   // 进度/日志刷新
constexpr UINT_PTR kTimerSelAnim = 1;

// ---------------------------------------------------------------- 侧栏数据
struct NavEntry {
    bool isGroup = false;
    wstring text;
    wstring group;          // 所属组名（v2.9 T01：组头用它切换展开/收起）
    int moduleIndex = -1;
};

// 侧栏一级分组固定展示顺序（v2.9 T01）。
// 根因：v2.8 分组顺序 = 翻译单元链接顺序（module.cpp Grouped() 按注册先后建组），
// 「磁盘清理」被挤到第 9 位。这里按「高频清理在前、网络运维相邻、低频优化垫底」
// 的排序原则固定展示顺序；仅侧栏展示层排序，Group() 字符串与注册顺序一律不动，
// CLI --list 与 v2.8 文档一致性不受影响。未登记的分组按注册顺序垫底。
const wchar_t* const kGroupOrder[] = {
    L"磁盘清理", L"空间分析", L"网络管理", L"远程终端", L"网络诊断", L"系统管理",
    L"系统信息", L"常用管理", L"软件管家", L"安全体检", L"净化中心",
    L"密码工具", L"高级工具", L"系统优化", L"系统维护",
};

wstring ToLower(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c += 32;
    return s;
}

bool ContainsCI(const wstring& haystack, const wstring& needle) {
    if (needle.empty()) return true;
    return ToLower(haystack).find(ToLower(needle)) != wstring::npos;
}

// 模块 → 侧栏图标：v2.18 起改为自研 24×24 矢量线性图标（ui/icons）。
// 旧 Segoe MDL2 字形方案在部分系统字体版本下缺字（渲染成感叹号/豆腐块），
// 自绘矢量彻底摆脱系统图标字体依赖，且与 VI-D2 手册的线稿风格一脉相承。

// 读取控件文本（短文本安全）
wstring GetWndText(HWND h) {
    if (!h) return L"";
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    vector<wchar_t> b(n + 1);
    GetWindowTextW(h, b.data(), n + 1);
    return b.data();
}

// 列表当前选中行索引（升序）
vector<int> SelectedRowsOf(HWND list) {
    vector<int> out;
    int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    while (i >= 0) { out.push_back(i); i = ListView_GetNextItem(list, i, LVNI_SELECTED); }
    return out;
}

// 侧栏 listbox 子类：把悬停项通知给父窗口绘制（定义在 AppWindow 之后）
LRESULT CALLBACK NavHoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR ref);

// 表头子类：整体自绘（暗色 + 排序箭头），不依赖 NM_CUSTOMDRAW 转发
LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR, DWORD_PTR ref);

// ---------------------------------------------------------------- 主窗口类
class AppWindow {
public:
    int Run(HINSTANCE hInst, int nCmdShow);

private:
    // ---- 生命周期 ----
    void Create(HINSTANCE hInst);
    void OnCreate();
    void OnDestroy();
    void ApplyWin11Style();
    void ApplyChromeTheme();      // 标题栏/滚动条/进度条随主题
    void RebuildBrushes();        // 重建缓存画刷（主题/DPI 变化）
    static void OnThemeChangedStatic(void* ctx);
    void OnThemeChanged();
    void ChangeScale(int dir);    // 界面缩放：+1 放大 / -1 缩小 / 0 复位
    void ApplyFontsToChildren();  // 字体重建后重新下发（DPI/缩放变化）

    // ---- 布局 ----
    void Layout(int w, int h);

    // ---- 侧栏 ----
    void BuildNav();
    void ExpandAllNav();      // 全部展开（v2.10）
    void CollapseAllNav();    // 全部收起（v2.10）
    void SaveNavOpen();       // 展开集合 → ini 持久化（v2.10 抽取复用）
    IModule* CurModule() const;
    void ApplyModuleToUi();

    // ---- 结果 ----
    void SetupColumns();
    void RenderResult();
    static const wstring& ColText(const ModuleRow& r, int col);

    // ---- 任务 ----
    void StartWork(bool apply, bool undo = false, int actionIndex = -1);
    void StopWork();
    void RunModuleOnThread(bool apply, bool undo, int actionIndex, wstring path,
                           wstring target, vector<int> selection,
                           vector<wstring> fieldVals);
    bool ConfirmApply(IModule* m, const vector<int>& selection,
                      int actionIndex = -1) const;
    void OnWorkDone();
    void ShowActionsMenu();   // 「更多 ▾」弹出扩展动作菜单（动作 2..N）

    // ---- 输入持久化 / 扩展字段（v2.12 R1/R3）----
    void SaveCurrentInputs();             // 当前模块的输入快照 → ini
    void RefreshStaticTexts();            // 语言切换后刷新固定文案按钮
    wstring FieldValue(int i) const;      // 第 i 个扩展字段当前文本

    // ---- 日志 ----
    void PushLog(const wstring& s);
    void FlushLog();
    void LogLine(const wstring& s) { PushLog(s); FlushLog(); }

    // ---- 绘制 ----
    void Paint();
    void DrawSidebar(HDC dc, const RECT& rc);
    void DrawHeader(HDC dc, const RECT& rc);
    void DrawStatusBar(HDC dc, const RECT& rc);
    void DrawDataCard(HDC dc);          // 结果 + 日志的卡片容器
    void DrawNavItem(const DRAWITEMSTRUCT* d);
    void DrawButton(const DRAWITEMSTRUCT* d);
    LRESULT ListCustomDraw(LPARAM lp);
    LRESULT HeaderCustomDraw(LPARAM lp);

    // ---- 消息 ----
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnCommand(WORD id, WORD code);

    HWND hwnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    HWND nav_ = nullptr, list_ = nullptr, logView_ = nullptr;
    HWND btnRun_ = nullptr, btnClean_ = nullptr, btnUndo_ = nullptr, btnStop_ = nullptr;
    HWND btnMore_ = nullptr;   // 扩展动作「更多 ▾」（v2.8）
    HWND btnAdmin_ = nullptr, btnAbout_ = nullptr, btnAll_ = nullptr, btnNone_ = nullptr;
    HWND btnTheme_ = nullptr;
    HWND pathEdit_ = nullptr, targetEdit_ = nullptr;
    HWND fieldEdits_[kMaxFields] = {};              // 扩展字段输入行（v2.12 R3）
    vector<ModuleField> curFields_;                 // 当前模块声明的字段
    HWND searchEdit_ = nullptr, clearBtn_ = nullptr, noMatch_ = nullptr;
    HWND btnLang_ = nullptr;                        // 语言切换（v2.12 R4）
    HWND btnNavExpand_ = nullptr, btnNavCollapse_ = nullptr;   // 侧栏展开/收起全部（v2.10）
    HWND progress_ = nullptr, header_ = nullptr;
    HIMAGELIST rowH_ = nullptr;   // 仅用于把列表行撑到 38px
    HFONT hdrFont_ = nullptr;     // 仅用于把表头撑到 38px（标签走自绘）

    HBRUSH bBg_ = nullptr, bSide_ = nullptr, bSurface_ = nullptr, bConsole_ = nullptr;
    HBRUSH bField_ = nullptr;     // 搜索框填充（比白面略深）

    vector<NavEntry> navs_;
    vector<IModule*> mods_;
    int cur_ = 0;
    wstring filter_;
    std::set<wstring> navOpen_;       // 展开的组名集合（v2.9 T01，缺省全展开）
    bool navOpenLoaded_ = false;      // ini 展开状态只装载一次

    ModuleResult result_;

public:
    // 供侧栏/表头子类化回调访问
    int hoverNav_ = -1;
    int sortCol_ = -1;
    bool sortAsc_ = true;
    int HoverNavIndex(int x, int y) const;
    void SortByColumn(int col);
    // v2.9 T01：侧栏两级导航（子类化回调需要命中/切换组头）
    bool IsNavGroupRow(int idx) const;          // idx 是否为组头行
    int NavGroupRowAt(int x, int y) const;      // 命中的组头行（-1 = 非组头）
    bool IsNavGroupOpen(const wstring& group) const;  // 组当前是否展开
    void ToggleNavGroupAt(int row);             // 切换第 row 行组头的展开/收起

private:
    std::thread worker_;
    std::atomic<bool> busy_{ false };
    std::atomic<bool> cancel_{ false };
    std::mutex logMutex_;
    vector<wstring> pending_;
    bool lastUndo_ = false;
    bool lastWasApply_ = false;   // 上次任务为 Tweaks 的应用/还原，完成后需刷新状态
    int lastActionIndex_ = -1;    // 上次执行的扩展动作下标（-1 = 非扩展动作）
    vector<ActionMenuItem> menuItems_;   // 弹出菜单期间有效（WM_DRAWITEM 用）

    RECT rcPath_, rcTarget_;
    RECT rcCard_ = {};        // 数据面板卡片外框
    RECT rcCardList_ = {};    // 「结果」段标题条
    RECT rcCardLog_ = {};     // 「运行日志」段标题条
    RECT rcFieldPath_ = {};   // 输入框外框（自绘圆角容器）
    RECT rcFieldTarget_ = {};
    RECT rcFieldExtra_[kMaxFields] = {};   // 扩展字段输入框外框（v2.12 R3）
    RECT rcNav_ = {};         // 侧栏导航可视区
    RECT rcElevChip_ = {};    // 权限状态胶囊
    int listH_ = 0;           // 列表实际高度（绘制与布局共用）
    int selAnim_ = 1000;   // 选中高亮动画进度 0..1000
};

// 侧栏 listbox 子类：把悬停项通知给父窗口绘制
LRESULT CALLBACK NavHoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<AppWindow*>(ref);
    if (msg == WM_MOUSEMOVE) {
        if (self) {
            int x = LOWORD(lp), y = HIWORD(lp);
            int h = self->HoverNavIndex(x, y);
            if (h != self->hoverNav_) {
                self->hoverNav_ = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        if (!GetPropW(hwnd, L"MzNavTrack")) {
            SetPropW(hwnd, L"MzNavTrack", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"MzNavTrack");
        if (self && self->hoverNav_ != -1) {
            self->hoverNav_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        // v2.9 T01：点击组头 = 切换展开/收起。在 ListBox 改变选中之前拦截
        // （吞掉消息，选中态不跳变）；模块行走默认路径 → LBN_SELCHANGE 切页。
        if (self) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            int row = self->NavGroupRowAt(x, y);
            if (row >= 0) {
                if (msg == WM_LBUTTONDOWN) self->ToggleNavGroupAt(row);
                return 0;   // 双击组头不做任何事（防止连点导致两次切换）
            }
        }
    } else if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_SPACE)) {
        // 键盘可达性：方向键把高亮移到组头行后，回车/空格同样切换展开
        if (self) {
            int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
            if (self->IsNavGroupRow(sel)) { self->ToggleNavGroupAt(sel); return 0; }
        }
    } else if (msg == WM_MOUSEWHEEL) {
        int idx = (int)SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0);
        int cnt = (int)SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (cnt > 0) {
            idx += (delta > 0 ? -2 : 2);
            if (idx < 0) idx = 0;
            if (idx > cnt - 1) idx = cnt - 1;
            SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)idx, 0);
            InvalidateRect(GetParent(hwnd), nullptr, FALSE);
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// 表头子类：整体自绘（暗色 + 排序箭头）
LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<AppWindow*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        // 表头撑高后，下半部分不在系统列命中区内，点它不会触发排序——手动补上
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT cr; GetClientRect(hwnd, &cr);
        for (int i = 0; i < 4; ++i) {
            RECT ir;
            if (!Header_GetItemRect(hwnd, i, &ir)) continue;
            RECT full = { ir.left, ir.top, ir.right, cr.bottom };
            if (PtInRect(&full, pt) && pt.y > ir.bottom) {
                if (self) self->SortByColumn(i);
                return 0;
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        ui::DoubleBuffer db;
        HDC mem = db.Begin(dc, rc);
        if (mem) {
            // 表头 = Panel2（Hover），底边 Hairline
            FillSolid(mem, rc, color::Hover);
            RECT bl = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
            FillSolid(mem, bl, color::Hairline);
            static const wchar_t* kLabels[] = { T(L"名称"), T(L"数值"), T(L"分类"), T(L"详情") };
            for (int i = 0; i < 4; ++i) {
                RECT ir;
                Header_GetItemRect(hwnd, i, &ir);
                // 垂直用整个表头高度，让标题在 38px 带内居中
                RECT tr = { ir.left + dpi::S(14), rc.top,
                            ir.right - dpi::S(14), rc.bottom };
                UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
                if (i == 1) fmt |= DT_RIGHT;
                bool sorted = self && self->sortCol_ == i;
                ui::DrawText(mem, T(kLabels[i]), tr, fmt,
                             sorted ? color::Accent : color::Text2,
                             ui::res::SmallStrong);
                if (sorted) {
                    wchar_t arrow = self->sortAsc_ ? 0xE70E : 0xE70D;
                    RECT ar = { ir.right - dpi::S(34), rc.top,
                                ir.right - dpi::S(14), rc.bottom };
                    ui::DrawGlyphCentered(mem, arrow, ar, color::Accent, ui::res::Icon);
                }
            }
            db.End(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 实现

int AppWindow::Run(HINSTANCE hInst, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                                 ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    dpi::EnableAwareness();
    ui::dpi::SetCurrent(ui::dpi::ForWindow(nullptr));
    dpi::SetUserScale(d::LoadUserScale());   // 恢复上次的界面缩放（先于字体创建）
    // v2.17：内嵌字体子集必须先于 InitResources 注册——InitResources 按
    // 字族名 CreateFontW(L"Baloo 2")，内存字体必须已就位。失败静默降级。
    d::fontsys::LoadEmbeddedFonts();
    ui::InitResources(ui::dpi::ForWindow(nullptr));
    d::LoadThemeOnStartup();   // 恢复上次选择的主题（先于任何窗口创建）
    lang::LoadOnStartup();     // 恢复上次选择的语言（v2.12 R4，先于任何文案创建）

    // 必须在创建任何窗口之前调用：强制浅色控件（系统深色模式下 EDIT 的
    // 提示文字会被染成近白色，落在我们的浅色输入框上就看不见了）。
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using FnMode = int (WINAPI*)(int);
        if (auto p = reinterpret_cast<FnMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
            p(3);   // SetPreferredAppMode: ForceLight
    }

    // v2.17 闪屏（仅 GUI 分支）：纸底/墨底 + 蝶形 + 字标；300ms 淡入、
    // 最长 1.5s 自动淡出，非阻塞（内部计时自毁）。
    brand::ShowSplash(nullptr);

    Create(hInst);
    if (!hwnd_) { ui::FreeResources(); return 1; }

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ui::FreeResources();
    return (int)msg.wParam;
}

void AppWindow::Create(HINSTANCE hInst) {
    hInst_ = hInst;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            self = static_cast<AppWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) { self->hwnd_ = h; return self->WndProc(h, m, w, l); }
            return -1;
        }
        if (self) return self->WndProc(h, m, w, l);
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NetAdminToolboxWnd";
    wc.hIcon = LoadIconW(hInst, L"APPICON");
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    // 以「工作区」为界（自动排除任务栏），避免窗口底部被任务栏遮住
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int availW = wa.right - wa.left;
    const int availH = wa.bottom - wa.top;
    int w = dpi::S(1260), h = dpi::S(880);
    if (w > availW - dpi::S(24)) w = availW - dpi::S(24);
    if (h > availH - dpi::S(24)) h = availH - dpi::S(24);
    if (w < 900 && availW > 900) w = 900;
    if (h < 620 && availH > 620) h = 620;
    int x = wa.left + (availW - w) / 2;
    int y = wa.top + (availH - h) / 2;

    CreateWindowExW(0, wc.lpszClassName,
                    Tx(d::brand::kAppNameCn).append(L" · ").append(d::brand::kAppNameEn)
                        .append(L"  —  ").append(d::brand::kPublisher).c_str(),
                    WS_OVERLAPPEDWINDOW, x, y, w, h,
                    nullptr, nullptr, hInst, this);
}

void AppWindow::OnCreate() {
    RebuildBrushes();

    // 暗色滚动条由 ApplyChromeTheme 按主题逐个控件设置
    nav_ = CreateWindowExW(0, WC_LISTBOXW, L"",
                           WS_CHILD | WS_VISIBLE | LBS_NOTIFY |
                               LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                           0, 0, 0, 0, hwnd_, (HMENU)ID_NAV, hInst_, nullptr);

    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, hwnd_, (HMENU)ID_LIST, hInst_, nullptr);
    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    header_ = ListView_GetHeader(list_);
    if (header_) {
        SetWindowSubclass(header_, HeaderSubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
    }

    logView_ = ui::CreateLogView(hwnd_, ID_LOG, hInst_);

    pathEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                hwnd_, (HMENU)ID_PATH, hInst_, nullptr);
    ui::SetFieldPlaceholder(pathEdit_, T(L"路径（留空 = 默认）"));
    targetEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                  hwnd_, (HMENU)ID_TARGET, hInst_, nullptr);
    ui::SetFieldPlaceholder(targetEdit_, T(L"目标 / 参数"));

    searchEdit_ = ui::CreateSearchBox(hwnd_, ID_SEARCH, ID_SEARCH_CLEAR, hInst_);
    clearBtn_ = (HWND)GetPropW(searchEdit_, L"MzSearchClear");

    auto mkBtn = [&](const wchar_t* t, int id) {
        HWND b = CreateWindowExW(0, L"BUTTON", T(t),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)id, hInst_, nullptr);
        ui::InstallButtonHover(b);
        return b;
    };
    btnRun_ = mkBtn(T(L"开始扫描"), ID_BTN_RUN);
    btnClean_ = mkBtn(T(L"执行清理"), ID_BTN_CLEAN);
    btnUndo_ = mkBtn(T(L"还原选中"), ID_BTN_UNDO);
    btnStop_ = mkBtn(T(L"停止"), ID_BTN_STOP);
    btnAll_ = mkBtn(T(L"全选"), ID_BTN_ALL);
    btnNone_ = mkBtn(T(L"取消全选"), ID_BTN_NONE);
    btnMore_ = mkBtn(T(L"更多 ▾"), ID_BTN_MORE);
    btnAdmin_ = mkBtn(T(L"以管理员重启"), ID_BTN_ADMIN);
    btnAbout_ = mkBtn(T(L"关于"), ID_BTN_ABOUT);
    btnTheme_ = mkBtn(T(L"主题"), ID_BTN_THEME);
    btnLang_ = mkBtn(T(L"语言"), ID_BTN_LANG);

    // 扩展字段输入行（v2.12 R3）：按模块 Fields() 动态显隐，密码位在
    // ApplyModuleToUi 里按字段声明切换（EM_SETPASSWORDCHAR）
    for (int i = 0; i < kMaxFields; ++i) {
        fieldEdits_[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                         hwnd_, (HMENU)(INT_PTR)(ID_FIELD0 + i),
                                         hInst_, nullptr);
    }

    progress_ = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                WS_CHILD | PBS_SMOOTH,
                                0, 0, 0, 0, hwnd_, (HMENU)ID_PROGRESS, hInst_, nullptr);

    noMatch_ = CreateWindowExW(0, L"STATIC", T(L"没有匹配的功能"),
                               WS_CHILD | SS_CENTER, 0, 0, 0, 0,
                               hwnd_, (HMENU)ID_NOMATCH, hInst_, nullptr);

    // 侧栏「全部展开 / 全部收起」（v2.10）：owner-draw 小按钮，
    // 必须带 WS_VISIBLE（WM_CREATE 期间创建的子窗口漏它永不显示）
    btnNavExpand_ = mkBtn(T(L"全部展开"), ID_NAV_EXPAND);
    btnNavCollapse_ = mkBtn(T(L"全部收起"), ID_NAV_COLLAPSE);

    ApplyFontsToChildren();
    SetWindowSubclass(nav_, NavHoverProc, 1, reinterpret_cast<DWORD_PTR>(this));

    ApplyWin11Style();
    ApplyChromeTheme();          // 滚动条/进度条按当前主题
    d::SetThemeChangedCallback(OnThemeChangedStatic, this);  // 切换主题时重建资源
    BuildNav();
    SetupColumns();
    ApplyModuleToUi();

    LogLine(T(L"网管工具箱 NetAdmin Toolbox v") + std::to_wstring(k_AppVersionMajor) + L"." +
            std::to_wstring(k_AppVersionMinor) + T(L"  ·  biohacker.fun 旗舰产品"));
    LogLine(Tx(L"已加载 ") + std::to_wstring(ModuleRegistry::Instance().All().size()) +
            Tx(L" 个功能模块，GUI/CLI 均由注册表自动生成。"));
    if (IsElevated()) LogLine(Tx(L"✓ 管理员模式：全部功能可用。"));
    else LogLine(Tx(L"提示：当前为标准用户模式，部分网络/系统功能可能读取不全。"));
    LogLine(Tx(L"提示：Ctrl+F 搜索功能，F5 重新扫描，Esc 停止，Ctrl+T 切换主题。"));
    LogLine(Tx(L"当前主题：") + Tx(d::CurrentTheme()->name) + Tx(L"（") +
            Tx(d::CurrentTheme()->desc) + Tx(L"）"));
}

void AppWindow::OnDestroy() {
    SaveCurrentInputs();   // 退出前把当前模块输入快照落盘（v2.12 R1）
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    ui::DismissToast();
    if (rowH_) { ImageList_Destroy(rowH_); rowH_ = nullptr; }
    if (hdrFont_) { DeleteObject(hdrFont_); hdrFont_ = nullptr; }
    for (HBRUSH b : { bBg_, bSide_, bSurface_, bConsole_, bField_ })
        if (b) DeleteObject(b);
    bBg_ = bSide_ = bSurface_ = bConsole_ = bField_ = nullptr;
}

void AppWindow::RebuildBrushes() {
    for (HBRUSH b : { bBg_, bSide_, bSurface_, bConsole_, bField_ })
        if (b) DeleteObject(b);
    bBg_ = CreateSolidBrush(color::Canvas);
    bSide_ = CreateSolidBrush(color::Side);
    bSurface_ = CreateSolidBrush(color::Canvas);
    bConsole_ = CreateSolidBrush(color::Canvas);
    bField_ = CreateSolidBrush(color::Hover);
}

void AppWindow::OnThemeChangedStatic(void* ctx) {
    if (auto* self = static_cast<AppWindow*>(ctx)) self->OnThemeChanged();
}

void AppWindow::OnThemeChanged() {
    if (!hwnd_) return;
    RebuildBrushes();
    ApplyWin11Style();     // 标题栏暗/亮跟随主题
    ApplyChromeTheme();    // 滚动条/进度条颜色
    // 全量子窗重绘（WM_CTLCOLOR 会重新取色，owner-draw 全部即时读取令牌）
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// 字体重建后重新下发到所有子窗口（DPI 变化 / 用户缩放都会走这里）
void AppWindow::ApplyFontsToChildren() {
    for (HWND c : { nav_, list_, btnRun_, btnClean_, btnUndo_, btnStop_, btnAll_,
                    btnNone_, btnMore_, btnAdmin_, btnAbout_, btnTheme_, btnLang_,
                    btnNavExpand_, btnNavCollapse_,
                    pathEdit_, targetEdit_, fieldEdits_[0], fieldEdits_[1],
                    fieldEdits_[2], fieldEdits_[3], searchEdit_, noMatch_ })
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)ui::res::Body, TRUE);
    if (logView_) SendMessageW(logView_, WM_SETFONT, (WPARAM)ui::res::Mono, TRUE);
    if (header_) {
        // 表头高度 = 字体行高 + 固定内边距，系统不允许随意改高度（每次 WM_SIZE
        // 都会重算）。用一只"高行高字体"把行高顶上去，标签实际由自绘负责。
        if (hdrFont_) DeleteObject(hdrFont_);
        hdrFont_ = CreateFontW(dpi::S(35), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
        SendMessageW(header_, WM_SETFONT, (WPARAM)hdrFont_, TRUE);
    }
    if (nav_) {
        for (int i = 0; i < (int)navs_.size(); ++i)
            SendMessageW(nav_, LB_SETITEMHEIGHT, (WPARAM)i,
                         dpi::S(navs_[i].isGroup ? size::NavGroup : size::NavItem));
    }

    // 用 1px 宽、38px 高的图像列表把报表行撑开：系统行高只按字体算，
    // 17px 文字会显得挤。图像列表只贡献高度，不占可见宽度。
    if (list_) {
        if (rowH_) {
            ListView_SetImageList(list_, nullptr, LVSIL_SMALL);
            ImageList_Destroy(rowH_);
        }
        rowH_ = ImageList_Create(1, dpi::S(38), ILC_COLOR32, 1, 1);
        ListView_SetImageList(list_, rowH_, LVSIL_SMALL);
    }
}

// 界面缩放：用户按 Ctrl+= / Ctrl+- 即可把整个界面（含字体）等比放大，
// 不需要重新编译，也不需要改系统 DPI。选择写入 ini，下次启动自动恢复。
void AppWindow::ChangeScale(int dir) {
    const double before = dpi::UserScale();
    if (dir == 0) dpi::ResetUserScale();
    else          dpi::StepUserScale(dir);
    const double after = dpi::UserScale();

    if (after == before && dir != 0) {
        ui::ShowToast(hwnd_, T(L"已到缩放上下限（85% ~ 180%）"),
                      color::GoldText, 0xE7BA /* Warning */);
        return;
    }
    if (after == before) return;

    ui::DismissToast();
    ui::InitResources(dpi::ForWindow(hwnd_));   // 按新倍率重建字体
    ApplyFontsToChildren();
    d::SaveUserScale(after);

    // 窗口尺寸同比增长：放大后仍是同样的信息密度，而不是把内容挤成一团。
    // 受当前显示器工作区钳制，超出屏幕时只放大内容（此时按钮行会自动压缩）。
    RECT wr; GetWindowRect(hwnd_, &wr);
    const double ratio = after / before;
    int nw = (int)((wr.right - wr.left) * ratio + 0.5);
    int nh = (int)((wr.bottom - wr.top) * ratio + 0.5);
    int maxW = 0, maxH = 0;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        maxW = mi.rcWork.right - mi.rcWork.left - dpi::S(16);
        maxH = mi.rcWork.bottom - mi.rcWork.top - dpi::S(32);
        if (nw > maxW) nw = maxW;
        if (nh > maxH) nh = maxH;
    }
    if (nw < 900 && (maxW == 0 || maxW > 900)) nw = 900;
    if (nh < 620 && (maxH == 0 || maxH > 620)) nh = 620;
    SetWindowPos(hwnd_, nullptr, 0, 0, nw, nh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT rc; GetClientRect(hwnd_, &rc);
    Layout(rc.right, rc.bottom);
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_);

    wchar_t msg[80];
    wsprintfW(msg, T(L"界面缩放 %d%%"), (int)(after * 100.0 + 0.5));
    ui::ShowToast(hwnd_, msg, color::Accent, 0xE8A3 /* Zoom */);
}

void AppWindow::ApplyWin11Style() {
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");   // 只加载一次
    if (!dwm) return;
    using FnAttr = HRESULT (WINAPI*)(HWND, int, LPCVOID, UINT);
    auto set = reinterpret_cast<FnAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!set) return;

    const bool dark = d::CurrentTheme()->Dark;
    BOOL darkFlag = dark ? TRUE : FALSE;
    set(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkFlag, sizeof(darkFlag));
    set(hwnd_, 19 /* 旧版同一属性 */, &darkFlag, sizeof(darkFlag));

    // 明确指定标题栏配色：系统处于深色模式时也不会得到一条黑色标题栏
    // （v2.17：Cap/CapText/Hairline 角色令牌，随主题即时刷新）
    const d::ThemeTokens* t = d::CurrentTheme();
    COLORREF cap = t->Cap;
    COLORREF txt = t->CapText;
    COLORREF brd = t->Hairline;
    set(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    set(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
    set(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &brd, sizeof(brd));

    int corner = 2;  // DWMWCP_ROUND
    set(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
}

// 滚动条 / 进度条颜色随主题（chrome 类资源不跟随令牌系统）
void AppWindow::ApplyChromeTheme() {
    bool dark = d::CurrentTheme()->Dark;
    const wchar_t* sb = dark ? L"DarkMode_Explorer" : L"Explorer";
    for (HWND c : { nav_, list_, header_, logView_, pathEdit_, targetEdit_,
                    fieldEdits_[0], fieldEdits_[1], fieldEdits_[2], fieldEdits_[3],
                    searchEdit_ })
        if (c) SetWindowTheme(c, sb, nullptr);
    if (list_) {
        ListView_SetBkColor(list_, color::Canvas);
        ListView_SetTextBkColor(list_, color::Canvas);
        ListView_SetTextColor(list_, color::Text);
    }
    if (progress_) {
        // 进度条 = 金信号（墨底/纸底唯一允许的大面积彩色），底轨 Panel2
        SendMessageW(progress_, PBM_SETBARCOLOR, 0, (LPARAM)color::Gold);
        SendMessageW(progress_, PBM_SETBKCOLOR, 0, (LPARAM)color::Hover);
        ShowWindow(progress_, busy_ ? SW_SHOW : SW_HIDE);
    }
}

// ---------------------------------------------------------------- 侧栏

void AppWindow::BuildNav() {
    auto groups = ModuleRegistry::Instance().Grouped();

    // 展开状态初始化（只装载一次）：ini 键 ui/navExpanded。
    // 空值/缺省 = 全展开；占位符 "-" = 全收起；否则为分号分隔的展开组名列表。
    if (!navOpenLoaded_) {
        navOpenLoaded_ = true;
        const wstring raw = d::LoadNavExpanded();
        if (raw.empty()) {
            for (auto& g : groups) navOpen_.insert(g.group);
        } else if (raw != L"-") {
            wstring cur;
            for (wchar_t c : raw) {
                if (c == L';' || c == L',') {
                    if (!cur.empty()) navOpen_.insert(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) navOpen_.insert(cur);
        }
    }

    // v2.10 修复：搜索结果里点选模块后清空搜索，页面跳回第一个模块。
    // 根因是 cur_ 存的是过滤后 mods_ 的下标，重过滤后索引错位；
    // 现按模块指针在新 mods_ 中重新对齐 cur_，保证切页状态不丢。
    // （keep 必须在清空前捕获）
    IModule* keep = (cur_ >= 0 && cur_ < (int)mods_.size()) ? mods_[cur_] : nullptr;

    mods_.clear();
    navs_.clear();

    // 1) 分组按固定顺序表排序（未登记组按注册顺序垫底）
    vector<ModuleRegistry::GroupEntry> ordered;
    vector<bool> used(groups.size(), false);
    for (const wchar_t* name : kGroupOrder) {
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            if (!used[gi] && groups[gi].group == name) {
                ordered.push_back(groups[gi]);
                used[gi] = true;
            }
        }
    }
    for (size_t gi = 0; gi < groups.size(); ++gi)
        if (!used[gi]) ordered.push_back(groups[gi]);

    // 2) 过滤 + 组装：mods_ 始终保存全部命中模块（cur_ 索引不受展开影响），
    //    展开状态只决定组内模块行是否进入 navs_ 展示行。
    const bool forceOpen = !filter_.empty();   // 搜索时强制全展开（忽略折叠态）
    for (auto& g : ordered) {
        const size_t first = mods_.size();
        for (auto* m : g.mods) {
            // 过滤同时匹配简体原文与译文（繁/英界面下也能用原文或译文搜索）
            const wchar_t* t = m->Title() ? m->Title() : L"";
            bool hit = filter_.empty() ||
                       ContainsCI(t, filter_) ||
                       ContainsCI(m->Description() ? m->Description() : L"", filter_) ||
                       ContainsCI(m->Id() ? m->Id() : L"", filter_) ||
                       (!filter_.empty() && ContainsCI(Tx(t), filter_));
            if (!hit) continue;
            mods_.push_back(m);
        }
        if (mods_.size() == first) continue;

        NavEntry he;
        he.isGroup = true;
        he.group = g.group;
        he.text = Tx(g.group);   // 展示层翻译（Group() 原文不动，CLI --list 不受影响）
        navs_.push_back(he);

        if (!forceOpen && !navOpen_.count(g.group)) continue;   // 收起：只留组头
        for (size_t k = first; k < mods_.size(); ++k) {
            NavEntry e;
            e.isGroup = false;
            e.group = g.group;
            e.text = Tx(mods_[k]->Title() ? mods_[k]->Title() : L"");
            e.moduleIndex = (int)k;
            navs_.push_back(e);
        }
    }

    // 2.5) cur_ 按模块指针对齐到新 mods_（模块被过滤掉时保持原值，
    //      走下方回落逻辑选第一个可见模块）
    if (keep) {
        for (size_t k = 0; k < mods_.size(); ++k)
            if (mods_[k] == keep) { cur_ = (int)k; break; }
    }

    SendMessageW(nav_, LB_RESETCONTENT, 0, 0);
    for (auto& e : navs_)
        SendMessageW(nav_, LB_ADDSTRING, 0, (LPARAM)e.text.c_str());

    // 组头 44 / 模块行 42（v2.9 T01 行高令牌，全部走 dpi::S 缩放）
    for (int i = 0; i < (int)navs_.size(); ++i)
        SendMessageW(nav_, LB_SETITEMHEIGHT, (WPARAM)i,
                     dpi::S(navs_[i].isGroup ? size::NavGroup : size::NavItem));

    // 恢复选中：优先当前模块行；模块行被收起时选其组头（cur_ 保持不变，
    // 页面仍显示该模块）；当前模块被搜索过滤掉时回落到第一个可见模块。
    // v2.10 全程按模块指针语义比较：keep 为空（首次构建）或 cur_ 未对齐到
    // keep 时一律走回落，杜绝「cur_ 残留下标撞值 → 页面与选中脱钩」。
    const bool kept = (keep && cur_ >= 0 && cur_ < (int)mods_.size() &&
                       mods_[cur_] == keep);
    int want = -1;
    if (kept) {
        for (size_t i = 0; i < navs_.size(); ++i)
            if (!navs_[i].isGroup && navs_[i].moduleIndex == cur_) { want = (int)i; break; }
        if (want < 0) {
            const wchar_t* cg = mods_[cur_]->Group();
            for (size_t i = 0; i < navs_.size(); ++i)
                if (navs_[i].isGroup && navs_[i].group == (cg ? cg : L"")) {
                    want = (int)i;
                    break;
                }
        }
    }
    if (want < 0) {
        for (size_t i = 0; i < navs_.size(); ++i)
            if (!navs_[i].isGroup) { want = (int)i; break; }
        if (want >= 0 && mods_[navs_[want].moduleIndex] != keep) {
            cur_ = navs_[want].moduleIndex;
            ApplyModuleToUi();
        }
    }
    if (want >= 0)
        SendMessageW(nav_, LB_SETCURSEL, (WPARAM)want, 0);
    SendMessageW(nav_, LB_SETTOPINDEX, 0, 0);
    ShowWindow(noMatch_, navs_.empty() ? SW_SHOW : SW_HIDE);
}

bool AppWindow::IsNavGroupRow(int idx) const {
    return idx >= 0 && idx < (int)navs_.size() && navs_[idx].isGroup;
}

// 展开集合 → ini 持久化（分号分隔组名；全收起由 design 层写占位符 "-"，
// 空值/缺省语义 = 全展开，不能占用）
void AppWindow::SaveNavOpen() {
    wstring csv;
    for (const auto& name : navOpen_) {
        if (!csv.empty()) csv += L';';
        csv += name;
    }
    d::SaveNavExpanded(csv.c_str());
}

// 全部展开 / 全部收起（v2.10）。搜索过滤期间 forceOpen 恒展开，
// 这两个操作改的是记忆状态（清空搜索后生效）。
void AppWindow::ExpandAllNav() {
    navOpen_.clear();
    for (auto& g : ModuleRegistry::Instance().Grouped())
        navOpen_.insert(g.group);
    SaveNavOpen();
    BuildNav();   // BuildNav 按模块指针保持 cur_ 与页面
}

void AppWindow::CollapseAllNav() {
    navOpen_.clear();
    SaveNavOpen();
    BuildNav();   // 收起后 BuildNav 把选中回落到当前模块所在组头，页面保持
}

int AppWindow::NavGroupRowAt(int x, int y) const {
    if (!nav_) return -1;
    // LB_ITEMFROMPOINT：低 WORD 为命中项，高 WORD 为客户端外标志
    LRESULT r = SendMessageW(nav_, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    if (HIWORD(r) != 0) return -1;
    int idx = (int)(short)LOWORD(r);
    return IsNavGroupRow(idx) ? idx : -1;
}

bool AppWindow::IsNavGroupOpen(const wstring& group) const {
    if (!filter_.empty()) return true;   // 搜索期间恒为展开
    return navOpen_.count(group) != 0;
}

// 切换分组展开/收起并重建展示行。搜索过滤期间组头恒为展开态，切换忽略。
// v2.10 手风琴语义：展开某组时其余组全部收起（点开一个、关掉其它）。
void AppWindow::ToggleNavGroupAt(int row) {
    if (!IsNavGroupRow(row) || !filter_.empty()) return;
    const wstring g = navs_[row].group;
    if (navOpen_.count(g)) {
        navOpen_.erase(g);            // 收起当前组
    } else {
        navOpen_.clear();             // 手风琴：一次只展开一个组
        navOpen_.insert(g);
    }
    SaveNavOpen();

    const int keepCur = cur_;   // 展开切换不改变当前模块
    BuildNav();

    // 高亮回到触发分组头并保证可见；组头不可用时（异常）回落到原模块行
    int want = -1;
    for (int i = 0; i < (int)navs_.size(); ++i)
        if (navs_[i].isGroup && navs_[i].group == g) { want = i; break; }
    if (want < 0 && keepCur >= 0 && keepCur < (int)mods_.size()) {
        for (int i = 0; i < (int)navs_.size(); ++i)
            if (!navs_[i].isGroup && navs_[i].moduleIndex == keepCur) { want = i; break; }
    }
    if (want >= 0) {
        SendMessageW(nav_, LB_SETCURSEL, (WPARAM)want, 0);
        SendMessageW(nav_, LB_SETTOPINDEX, (WPARAM)want, 0);
    }
    InvalidateRect(nav_, nullptr, FALSE);
}

IModule* AppWindow::CurModule() const {
    if (cur_ < 0 || cur_ >= (int)mods_.size()) return nullptr;
    return mods_[cur_];
}

int AppWindow::HoverNavIndex(int x, int y) const {
    if (!nav_) return -1;
    // LB_ITEMFROMPOINT：低 WORD 为命中项，高 WORD 为客户端外标志
    LRESULT r = SendMessageW(nav_, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    int idx = (int)(short)LOWORD(r);
    if (HIWORD(r) != 0) return -1;
    // v2.9 T01：组头也是可点击行（展开/收起），悬停命中不再跳过组头
    if (idx < 0 || idx >= (int)navs_.size()) return -1;
    return idx;
}

void AppWindow::ApplyModuleToUi() {
    IModule* m = CurModule();
    if (!m) return;

    bool hasPath = m->NeedsPath();
    bool hasTarget = m->NeedsTarget();
    bool tweak = (m->Kind() == ModuleKind::Tweaks);
    bool cleaner = (m->Kind() == ModuleKind::Cleaner);
    bool action = (m->Kind() == ModuleKind::Action || m->Kind() == ModuleKind::Console);

    ShowWindow(pathEdit_, hasPath ? SW_SHOW : SW_HIDE);
    ShowWindow(targetEdit_, hasTarget ? SW_SHOW : SW_HIDE);
    if (hasTarget) {
        const wchar_t* hint = m->TargetHint();
        ui::SetFieldPlaceholder(targetEdit_, T(hint ? hint : T(L"目标 / 参数")));
    }
    if (hasPath) {
        const wchar_t* hint = m->PathHint();
        ui::SetFieldPlaceholder(pathEdit_, T(hint ? hint : T(L"路径（留空 = 默认）")));
    }

    // ---- 扩展字段（v2.12 R3）：按 Fields() 显隐/占位/密码位 ----
    curFields_ = m->Fields();
    if ((int)curFields_.size() > kMaxFields) curFields_.resize(kMaxFields);
    for (int i = 0; i < kMaxFields; ++i) {
        if (i < (int)curFields_.size()) {
            ShowWindow(fieldEdits_[i], SW_SHOW);
            ui::SetFieldPlaceholder(fieldEdits_[i],
                                    curFields_[i].hint ? T(curFields_[i].hint) : L"");
            // 密码位切换（EM_SETPASSWORDCHAR；置 0 恢复普通显示）
            SendMessageW(fieldEdits_[i], EM_SETPASSWORDCHAR,
                         curFields_[i].secret ? (WPARAM)L'*' : 0, 0);
            InvalidateRect(fieldEdits_[i], nullptr, TRUE);
        } else {
            ShowWindow(fieldEdits_[i], SW_HIDE);
        }
    }

    // ---- 输入持久化回填（v2.12 R1）：填过一次，之后每次打开都是默认值 ----
    {
        d::ModuleInputSnapshot snap;
        if (d::LoadModuleInput(m->Id(), snap)) {
            if (hasPath) SetWindowTextW(pathEdit_, snap.path.c_str());
            if (hasTarget) SetWindowTextW(targetEdit_, snap.target.c_str());
            for (int i = 0; i < kMaxFields && i < (int)curFields_.size(); ++i)
                if (i < (int)snap.fields.size())
                    SetWindowTextW(fieldEdits_[i], snap.fields[i].c_str());
        }
    }

    // 主按钮文案：模块可自定义（如「开始体检」「开始查杀」）；Action 型默认「执行」
    // （v2.13：RunLabel 判断前移——Tweaks + 扩展动作的模块（如 purg.scan）同样适用）
    {
        const wchar_t* rl = m->RunLabel();
        SetWindowTextW(btnRun_, rl ? T(rl) : (action ? T(L"执行") : T(L"开始扫描")));
    }

    // ---- 扩展动作框架（v2.8）：Actions() 非空的模块复用按钮位 ----
    // 动作 0 → 「执行清理」位（btnClean_）、动作 1 → 「还原选中」位（btnUndo_）、
    // 动作 2+ → 「更多 ▾」弹出菜单（btnMore_）。确认框与执行统一走
    // StartWork(apply, undo, actionIndex) → RunAction。
    vector<ModuleAction> acts = m->Actions();
    if (!acts.empty()) {
        const ModuleAction& a0 = acts[0];
        SetWindowTextW(btnClean_, a0.label ? T(a0.label) : T(L"执行"));
        ShowWindow(btnClean_, SW_SHOW);
        if (acts.size() > 1) {
            const ModuleAction& a1 = acts[1];
            SetWindowTextW(btnUndo_, a1.label ? T(a1.label) : T(L"执行"));
            ShowWindow(btnUndo_, SW_SHOW);
        } else {
            ShowWindow(btnUndo_, SW_HIDE);
        }
        // 动作 2+ 收进「更多 ▾」弹出菜单
        ShowWindow(btnMore_, acts.size() > 2 ? SW_SHOW : SW_HIDE);
    } else {
        ShowWindow(btnMore_, SW_HIDE);
        ShowWindow(btnClean_, (cleaner || tweak) ? SW_SHOW : SW_HIDE);
        // 主按钮文案：模块可自定义（如「安装选中」「卸载选中」），为空回落默认
        if (const wchar_t* cleanText = m->ApplyLabel())
            SetWindowTextW(btnClean_, T(cleanText));
        else
            SetWindowTextW(btnClean_, T(tweak ? T(L"应用选中") : T(L"执行清理")));
        ShowWindow(btnUndo_, (tweak && m->SupportsUndo()) ? SW_SHOW : SW_HIDE);
        // 还原按钮文案：模块可自定义（如「卸载选中」），为空回落默认
        if (const wchar_t* undoText = m->UndoLabel())
            SetWindowTextW(btnUndo_, T(undoText));
        else
            SetWindowTextW(btnUndo_, T(L"还原选中"));
    }
    ShowWindow(btnAll_, tweak ? SW_SHOW : SW_HIDE);
    ShowWindow(btnNone_, tweak ? SW_SHOW : SW_HIDE);
    ShowWindow(progress_, SW_HIDE);

    EnableWindow(btnRun_, !busy_);
    EnableWindow(btnClean_, !busy_ && result_.canClean);
    // 扩展动作模块（如 sec.repair）：主按钮=动作 0，不经扫描就有意义，
    // 不受 canClean 约束（canClean 只对「先扫描后清理」的模块有意义）
    if (!acts.empty()) EnableWindow(btnClean_, !busy_);
    EnableWindow(btnUndo_, !busy_);
    EnableWindow(btnMore_, !busy_);
    EnableWindow(btnStop_, FALSE);

    result_.rows.clear();
    result_.summary.clear();
    result_.hint.clear();
    result_.canClean = false;
    ListView_DeleteAllItems(list_);
    sortCol_ = -1;

    if (m->NeedsElevation() && !IsElevated())
        PushLog(Tx(L"提示：「") + Tx(m->Title() ? m->Title() : L"") +
                Tx(L"」建议以管理员身份运行。"));

    selAnim_ = 0;
    SetTimer(hwnd_, kTimerSelAnim, d::motion::Frame, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
    Layout(0, 0);
}

// ---------------------------------------------------------------- 结果列表

void AppWindow::SetupColumns() {
    while (ListView_DeleteColumn(list_, 0)) {}
    struct { const wchar_t* t; int w; int fmt; } cols[] = {
        { T(L"名称"), 280, LVCFMT_LEFT },
        { T(L"数值"), 130, LVCFMT_RIGHT },
        { T(L"分类"), 140, LVCFMT_LEFT },
        { T(L"详情"), 420, LVCFMT_LEFT },
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = dpi::S(cols[i].w);
        c.iSubItem = i;
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(list_, i, &c);
    }
}

const wstring& AppWindow::ColText(const ModuleRow& r, int col) {
    switch (col) {
    case 0: return r.c1;
    case 2: return r.c3;
    case 3: return r.c4;
    default: return r.c2;
    }
}

void AppWindow::RenderResult() {
    ListView_DeleteAllItems(list_);
    auto& rows = result_.rows;
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    for (size_t i = 0; i < rows.size(); ++i) {
        it.iItem = (int)i;
        it.pszText = (LPWSTR)rows[i].c1.c_str();
        ListView_InsertItem(list_, &it);
        ListView_SetItemText(list_, (int)i, 1, (LPWSTR)rows[i].c2.c_str());
        ListView_SetItemText(list_, (int)i, 2, (LPWSTR)rows[i].c3.c_str());
        ListView_SetItemText(list_, (int)i, 3, (LPWSTR)rows[i].c4.c_str());
    }
    // 行数变化后重排：列表高度按内容自适应，多出的空间归日志
    Layout(0, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::SortByColumn(int col) {
    // 注：Tweaks(Windows 优化) 模块过去因行序与内部特性下标一一对应而禁止排序。
    // 现已通过 ModuleRow::tag 记录特性下标，排序后选择仍可正确映射，故放开。
    if (sortCol_ == col) sortAsc_ = !sortAsc_;
    else { sortCol_ = col; sortAsc_ = true; }

    std::stable_sort(result_.rows.begin(), result_.rows.end(),
                     [this, col](const ModuleRow& a, const ModuleRow& b) {
        int r;
        if (col == 1) {
            r = (a.sortKey < b.sortKey) ? -1 : (a.sortKey > b.sortKey ? 1 : 0);
        } else {
            const wstring& sa = ColText(a, col);
            const wstring& sb = ColText(b, col);
            int cr = CompareStringOrdinal(sa.c_str(), (int)sa.size(),
                                          sb.c_str(), (int)sb.size(), TRUE);
            r = (cr == CSTR_LESS_THAN) ? -1 : (cr == CSTR_GREATER_THAN ? 1 : 0);
        }
        return sortAsc_ ? r < 0 : r > 0;
    });
    RenderResult();
}

// ---------------------------------------------------------------- 任务

bool AppWindow::ConfirmApply(IModule* m, const vector<int>& selection,
                             int actionIndex) const {
    wstring title = m->Title();
    wstring body;
    bool danger = (m->Kind() == ModuleKind::Cleaner) || m->ApplyIsDanger();

    // 扩展动作（v2.8）：确认框优先用模块动态生成的详细警告（ActionWarning，
    // 如列出将结束的进程名），回落 ModuleAction.warning，再回落通用文案
    vector<ModuleAction> acts = m->Actions();
    if (actionIndex >= 0 && actionIndex < (int)acts.size()) {
        const ModuleAction& a = acts[actionIndex];
        danger = a.danger;
        // 选中行 → tag（排序安全），供模块生成动态警告文本
        vector<int> tags;
        for (int idx : selection)
            tags.push_back((idx >= 0 && idx < (int)result_.rows.size())
                               ? result_.rows[idx].tag : idx);
        // Tx 包裹：静态确认框文案命中 kEn 表（动态拼接回退中文，已知边界）
        body = Tx(m->ActionWarning(actionIndex, tags));
        if (body.empty()) {
            if (a.warning) {
                body = a.warning;
                if (!selection.empty())
                    body = Tx(L"已勾选 ") + std::to_wstring(selection.size()) +
                           Tx(L" 项。\n\n") + body;
            } else {
                wstring scope = selection.empty()
                                    ? T(L"全部")
                                    : (std::to_wstring(selection.size()) + Tx(L" 项"));
                body = Tx(L"将对选中的 ") + scope + Tx(L" 执行「") +
                       (a.label ? T(a.label) : T(L"操作")) + Tx(L"」。\n\n是否继续？");
            }
        }
        return mz::ui::ConfirmDialog(hwnd_, danger ? T(L"确认执行操作") : T(L"确认执行"),
                                     body, T(L"确认执行"), T(L"取消"), danger);
    }

    if (const wchar_t* warn = m->ApplyWarning()) {
        // 模块自定义确认框正文（自行含「是否继续」问句），前面拼接勾选数量
        body = Tx(warn);
        if (!selection.empty())
            body = Tx(L"已勾选 ") + std::to_wstring(selection.size()) +
                   Tx(L" 项。\n\n") + body;
    } else if (m->Kind() == ModuleKind::Tweaks) {
        int n = (int)selection.size();
        if (n == 0)
            body = T(L"未选中任何优化项，将对「") + title + wstring(T(L"」的全部设置执行应用（写入注册表）。\n\n"))
                   + T(L"建议先创建系统还原点。是否继续？");
        else
            body = T(L"将「") + title + T(L"」应用到选中的 ") + std::to_wstring(n) +
                   T(L" 项设置（写入注册表）。\n\n建议先创建系统还原点。是否继续？");
    } else {
        body = T(L"将执行「") + title + T(L"」的清理操作，扫描到的文件可能被永久删除且不可恢复。\n\n") +
               (result_.summary.empty() ? wstring(T(L"请确认已看清扫描列表。")) : result_.summary) +
               T(L"\n\n建议先创建系统还原点。是否继续？");
    }
    return mz::ui::ConfirmDialog(hwnd_, danger ? T(L"确认执行清理") : T(L"确认执行"), body,
                                 T(L"确认执行"), T(L"取消"), danger);
}

void AppWindow::StartWork(bool apply, bool undo, int actionIndex) {
    if (busy_) return;
    IModule* m = CurModule();
    if (!m) return;

    // 输入与勾选状态只能在 UI 线程读取，先快照再交给工作线程
    wstring path = GetWndText(pathEdit_);
    wstring target = GetWndText(targetEdit_);
    vector<wstring> fieldVals;
    for (int i = 0; i < (int)curFields_.size() && i < kMaxFields; ++i)
        fieldVals.push_back(GetWndText(fieldEdits_[i]));
    vector<int> selection = SelectedRowsOf(list_);

    // v2.12 R1：执行一次就把输入快照落盘（含 DPAPI 加密的密码字段）
    SaveCurrentInputs();

    // v2.9 T02：确认框前先注入一次输入——ActionWarning 动态警告（如列出目标
    // 用户名/新计算机名）需要在确认时读到当前输入框值；工作线程随后会再注入
    // 一次（同值幂等，对存量模块无影响）
    m->SetInput(path, target);
    if (!fieldVals.empty()) m->SetFields(fieldVals);

    if (apply && !ConfirmApply(m, selection, actionIndex)) return;

    busy_ = true;
    cancel_ = false;
    lastUndo_ = undo;
    lastWasApply_ = apply;
    lastActionIndex_ = actionIndex;
    EnableWindow(btnRun_, FALSE);
    EnableWindow(btnClean_, FALSE);
    EnableWindow(btnUndo_, FALSE);
    EnableWindow(btnMore_, FALSE);
    EnableWindow(btnStop_, TRUE);
    SendMessageW(progress_, PBM_SETPOS, 0, 0);
    ShowWindow(progress_, SW_SHOW);
    ui::DismissToast();
    InvalidateRect(hwnd_, nullptr, FALSE);

    wstring what = (actionIndex >= 0) ? T(L"动作：")
                                      : (undo ? T(L"还原：") : (apply ? T(L"执行：") : T(L"扫描：")));
    PushLog(L"");
    PushLog(L"▸ " + what + Tx(m->Title() ? m->Title() : L""));
    FlushLog();

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, apply, undo, actionIndex, path, target,
                           fieldVals, selection]() {
        RunModuleOnThread(apply, undo, actionIndex, std::move(path), std::move(target),
                          std::move(selection), std::move(fieldVals));
    });
}

void AppWindow::StopWork() {
    cancel_ = true;
    LogLine(Tx(L"正在停止…"));
}

// ---- 输入持久化 / 扩展字段（v2.12 R1/R3）--------------------------

wstring AppWindow::FieldValue(int i) const {
    if (i < 0 || i >= kMaxFields || !fieldEdits_[i]) return wstring();
    return GetWndText(fieldEdits_[i]);
}

// 当前模块的输入快照 → ini（切模块 / 执行 / 退出时调用）。
// secret 字段在 design::SaveModuleInput 内部走 DPAPI 加密，不落明文。
void AppWindow::SaveCurrentInputs() {
    IModule* m = CurModule();
    if (!m) return;
    d::ModuleInputSnapshot s;
    s.path = GetWndText(pathEdit_);
    s.target = GetWndText(targetEdit_);
    for (int i = 0; i < (int)curFields_.size() && i < kMaxFields; ++i)
        s.fields.push_back(FieldValue(i));
    for (const auto& f : curFields_) s.secret.push_back(f.secret);
    d::SaveModuleInput(m->Id(), s);
}

// 语言切换后刷新固定文案（动态文案由 ApplyModuleToUi / BuildNav / 重画承担）
void AppWindow::RefreshStaticTexts() {
    SetWindowTextW(btnRun_, T(L"开始扫描"));
    SetWindowTextW(btnStop_, T(L"停止"));
    SetWindowTextW(btnAll_, T(L"全选"));
    SetWindowTextW(btnNone_, T(L"取消全选"));
    SetWindowTextW(btnMore_, T(L"更多 ▾"));
    SetWindowTextW(btnAdmin_, T(L"以管理员重启"));
    SetWindowTextW(btnAbout_, T(L"关于"));
    SetWindowTextW(btnTheme_, T(L"主题"));
    SetWindowTextW(btnNavExpand_, T(L"全部展开"));
    SetWindowTextW(btnNavCollapse_, T(L"全部收起"));
    SetWindowTextW(noMatch_, T(L"没有匹配的功能"));
    ui::SetFieldPlaceholder(pathEdit_, T(L"路径（留空 = 默认）"));
    ui::SetFieldPlaceholder(targetEdit_, T(L"目标 / 参数"));
    ui::SetFieldPlaceholder(searchEdit_, T(L"搜索功能…  (Ctrl+F)"));
}

void AppWindow::RunModuleOnThread(bool apply, bool undo, int actionIndex,
                                  wstring path, wstring target,
                                  vector<int> selection,
                                  vector<wstring> fieldVals) {
    IModule* m = CurModule();
    if (!m) { PostMessageW(hwnd_, kMsgDone, 0, 0); return; }

    ModuleContext ctx;
    ctx.hwnd = hwnd_;
    ctx.log.onLine = [this](const wstring& s) {
        PushLog(s);
        PostMessageW(hwnd_, kMsgTick, (WPARAM)-1, 0);
    };
    ctx.progress = [this](int p, const wstring& s) {
        if (!s.empty()) PushLog(s);
        PostMessageW(hwnd_, kMsgTick, (WPARAM)p, 0);
    };
    ctx.cancel = [this]() { return cancel_.load(); };

    m->SetInput(path, target);
    if (!fieldVals.empty()) m->SetFields(fieldVals);
    if (m->Kind() == ModuleKind::Tweaks) {
        // 列表可能被排序过，显示行下标已与 feats_ 错位。
        // 通过 row.tag（特性下标）把「显示行」映射回模块内部索引。
        vector<int> mapped;
        for (int idx : selection) {
            if (idx >= 0 && idx < (int)result_.rows.size())
                mapped.push_back(result_.rows[idx].tag);
            else
                mapped.push_back(idx);
        }
        m->SetSelection(mapped);
    }

    ModuleResult out;
    if (actionIndex >= 0) {
        m->RunAction(ctx, actionIndex, out);   // 扩展动作（v2.8 第四态）
    } else if (apply) {
        if (undo) m->Undo(ctx, out);
        else m->Apply(ctx, out);
    } else {
        m->Run(ctx, out);
    }
    result_ = std::move(out);
    PostMessageW(hwnd_, kMsgDone, 0, 0);
}

void AppWindow::OnWorkDone() {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    EnableWindow(btnRun_, TRUE);
    EnableWindow(btnClean_, result_.canClean);
    EnableWindow(btnUndo_, TRUE);
    EnableWindow(btnMore_, TRUE);
    EnableWindow(btnStop_, FALSE);
    SendMessageW(progress_, PBM_SETPOS, 0, 0);
    ShowWindow(progress_, SW_HIDE);
    RenderResult();
    FlushLog();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
        wstring t = result_.summary;
        if (t.size() > 56) t = t.substr(0, 56) + L"…";
        if (!t.empty())
            // Toast 语义映射：还原/回退 → GoldText（信号），完成 → ChIn（进）
            ui::ShowToast(hwnd_, t, lastUndo_ ? color::GoldText : color::ChIn,
                          lastUndo_ ? 0xE72C : 0xE73E);
    }

    // Tweaks 应用/还原后（或模块声明 RefreshAfterAction 的扩展动作完成后），
    // 重新扫描以刷新列表状态，否则界面停留在旧状态，会被误判为「不成功」。
    bool needRefresh = false;
    if (lastWasApply_ && CurModule()) {
        IModule* m = CurModule();
        if (m->Kind() == ModuleKind::Tweaks) needRefresh = true;
        if (lastActionIndex_ >= 0 && m->RefreshAfterAction()) needRefresh = true;
    }
    if (needRefresh) {
        lastWasApply_ = false;
        lastActionIndex_ = -1;
        StartWork(false);
    }
}

// 「更多 ▾」弹出菜单：扩展动作 2..N（owner-draw 绘制，danger 项红色）。
// 选择后走 StartWork(actionIndex)（带确认框），与按钮位动作同一条链路。
void AppWindow::ShowActionsMenu() {
    IModule* m = CurModule();
    if (!m || busy_) return;
    vector<ModuleAction> acts = m->Actions();
    if ((int)acts.size() <= 2) return;

    menuItems_.clear();
    HMENU menu = CreatePopupMenu();
    for (int i = 2; i < (int)acts.size(); ++i) {
        ActionMenuItem it;
        it.label = acts[i].label ? T(acts[i].label) : T(L"动作");
        it.danger = acts[i].danger;
        menuItems_.push_back(std::move(it));

        MENUITEMINFOW mi = { sizeof(mi) };
        mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_DATA;
        mi.wID = (UINT)(kActMenuIdBase + i);
        mi.fType = MFT_OWNERDRAW;
        mi.dwItemData = (ULONG_PTR)(kActMenuIdBase + i);   // 动作绝对下标 + kActMenuIdBase（Bug-1：与 WM_MEASUREITEM/WM_DRAWITEM 读取端的 >= kActMenuIdBase 判定配对；wID 路由与此解耦）
        InsertMenuItemW(menu, (UINT)(i - 2), TRUE, &mi);
    }

    RECT rc;
    GetWindowRect(btnMore_, &rc);
    SetForegroundWindow(hwnd_);   // 规避菜单弹出后不消失的焦点问题
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                              rc.left, rc.bottom, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    menuItems_.clear();

    if (cmd >= kActMenuIdBase && cmd < (UINT)(kActMenuIdBase + 1000))
        StartWork(true, false, (int)(cmd - kActMenuIdBase));
}

// ---------------------------------------------------------------- 日志

void AppWindow::PushLog(const wstring& s) {
    if (!logView_) return;
    std::lock_guard<std::mutex> lk(logMutex_);
    pending_.push_back(s);
}

void AppWindow::FlushLog() {
    if (!logView_) return;
    vector<wstring> lines;
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        if (pending_.empty()) return;
        lines.swap(pending_);
    }
    // LogView 自行管理行数上限与自动滚动，无需 EDIT 时代的字符数裁剪
    ui::LogViewAppendBatch(logView_, lines);
}

// ---------------------------------------------------------------- 布局

void AppWindow::Layout(int w, int h) {
    if (!nav_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    w = rc.right; h = rc.bottom;

    const int SB     = dpi::S(size::SidebarWidth);
    const int ST     = dpi::S(size::StatusH);
    const int PAD    = dpi::S(space::XXL);        // 内容区左右留白
    const int GAP    = dpi::S(space::S);
    const int EDH    = dpi::S(size::EditH);
    const int BTN_H  = dpi::S(size::ButtonH);
    const int SHD    = dpi::S(size::SidebarHeader);
    const int CPAD   = dpi::S(size::CardPad);
    const int CTH    = dpi::S(size::CardTitleH);
    const int MIN_LOG = dpi::S(size::LogMinH);

    // ---- 侧栏：品牌 + 搜索 + 导航 ----
    const int sx = dpi::S(16);
    const int sw = SB - sx * 2;
    const int searchTop = SHD + dpi::S(2);
    {
        RECT field = { sx, searchTop, sx + sw, searchTop + dpi::S(38) };
        ui::LayoutSearchBox(searchEdit_, clearBtn_,
                            RECT{ field.left + dpi::S(32), field.top + dpi::S(3),
                                  field.right - dpi::S(10), field.bottom - dpi::S(3) });
    }
    // v2.10：搜索框下方「全部展开 / 全部收起」按钮条，再往下才是导航列表
    const int stripTop = searchTop + dpi::S(38) + dpi::S(8);
    const int stripH = dpi::S(28);
    {
        const int halfW = (sw - dpi::S(8)) / 2;
        MoveWindow(btnNavExpand_, sx, stripTop, halfW, stripH, TRUE);
        MoveWindow(btnNavCollapse_, sx + halfW + dpi::S(8), stripTop,
                   sw - halfW - dpi::S(8), stripH, TRUE);
    }
    const int navTop = stripTop + stripH + dpi::S(10);
    SetRect(&rcNav_, 0, navTop, SB, h - ST);
    // 右侧留 10px 窄条给自绘滚动指示（系统滚动条太粗且不可主题化）
    MoveWindow(nav_, rcNav_.left, rcNav_.top, SB - dpi::S(10),
               rcNav_.bottom - rcNav_.top, TRUE);
    MoveWindow(noMatch_, sx + dpi::S(4), navTop + dpi::S(40),
               sw - dpi::S(8), dpi::S(24), TRUE);

    // ---- 内容区 ----
    const int cx = SB + PAD;
    const int cw = w - SB - PAD * 2;
    int y = dpi::S(size::HeaderH);

    // 输入区（仅需要路径/目标的模块出现；扩展字段独占整行，标签在框外左侧）
    {
        const bool hasPath = IsWindowVisible(pathEdit_) != FALSE;
        const bool hasTarget = IsWindowVisible(targetEdit_) != FALSE;
        if (hasPath || hasTarget) {
            const int half = (cw - dpi::S(12)) / 2;
            SetRect(&rcFieldPath_, 0, 0, 0, 0);
            SetRect(&rcFieldTarget_, 0, 0, 0, 0);
            if (hasPath && hasTarget) {
                SetRect(&rcFieldPath_, cx, y, cx + half, y + EDH);
                SetRect(&rcFieldTarget_, cx + half + dpi::S(12), y, cx + cw, y + EDH);
            } else if (hasPath) {
                SetRect(&rcFieldPath_, cx, y, cx + cw, y + EDH);
            } else {
                SetRect(&rcFieldTarget_, cx, y, cx + cw, y + EDH);
            }
            auto place = [&](HWND ed, const RECT& f) {
                if (!ed || f.right <= f.left) return;
                MoveWindow(ed, f.left + dpi::S(14), f.top + dpi::S(5),
                           (f.right - f.left) - dpi::S(28),
                           (f.bottom - f.top) - dpi::S(10), TRUE);
            };
            place(pathEdit_, rcFieldPath_);
            place(targetEdit_, rcFieldTarget_);
            y += EDH + dpi::S(14);
        } else {
            SetRect(&rcFieldPath_, 0, 0, 0, 0);
            SetRect(&rcFieldTarget_, 0, 0, 0, 0);
        }

        // 扩展字段行（v2.12 R3）：[标签 150px][输入框占满余宽]
        const int LABELW = dpi::S(150);
        for (int i = 0; i < kMaxFields; ++i) {
            SetRect(&rcFieldExtra_[i], 0, 0, 0, 0);
            if (i >= (int)curFields_.size()) continue;
            if (!(GetWindowLongW(fieldEdits_[i], GWL_STYLE) & WS_VISIBLE)) continue;
            SetRect(&rcFieldExtra_[i], cx + LABELW, y, cx + cw, y + EDH);
            MoveWindow(fieldEdits_[i],
                       rcFieldExtra_[i].left + dpi::S(10), rcFieldExtra_[i].top + dpi::S(5),
                       (rcFieldExtra_[i].right - rcFieldExtra_[i].left) - dpi::S(20),
                       (rcFieldExtra_[i].bottom - rcFieldExtra_[i].top) - dpi::S(10), TRUE);
            y += EDH + dpi::S(10);
        }
    }

    // 按钮行：空间不足时等比压缩，绝不溢出卡片右边界
    {
        HWND btns[7] = { btnRun_, btnClean_, btnUndo_, btnStop_, btnAll_,
                         btnNone_, btnMore_ };
        const int baseW[7] = { 134, 134, 134, 108, 104, 124, 96 };
        int visCount = 0, sumW = 0;
        for (int i = 0; i < 7; ++i)
            if (IsWindowVisible(btns[i])) { ++visCount; sumW += dpi::S(baseW[i]); }
        const int gaps = GAP * (visCount > 1 ? visCount - 1 : 0);
        double shrink = 1.0;
        if (visCount > 0 && sumW + gaps > cw)
            shrink = double(cw - gaps) / double(sumW);
        if (shrink < 0.45) shrink = 0.45;

        int bx = cx;
        for (int i = 0; i < 7; ++i) {
            if (!IsWindowVisible(btns[i])) continue;
            int bw = (int)(dpi::S(baseW[i]) * shrink);
            MoveWindow(btns[i], bx, y, bw, BTN_H, TRUE);
            bx += bw + GAP;
        }
        y += BTN_H;
    }

    // 进度线：占固定高度（运行中才显示），避免开始扫描时卡片整体跳动
    MoveWindow(progress_, cx, y + dpi::S(12), cw, dpi::S(4), TRUE);
    y += dpi::S(4) + dpi::S(24);

    // ---- 数据卡：结果 + 日志 ----
    SetRect(&rcCard_, cx, y, cx + cw, h - ST - dpi::S(16));
    if (rcCard_.bottom < rcCard_.top + dpi::S(200))
        rcCard_.bottom = rcCard_.top + dpi::S(200);

    const int titleTop = rcCard_.top + dpi::S(10);
    SetRect(&rcCardList_, rcCard_.left + CPAD, titleTop,
            rcCard_.right - CPAD, titleTop + CTH);
    const int listTop = rcCardList_.bottom + dpi::S(2);

    // 列表高度按内容自适应：数据少时把余量让给日志
    const int rowCnt = ListView_GetItemCount(list_);
    int rowH = dpi::S(38);
    if (rowCnt > 0) {
        RECT ir = { LVIR_BOUNDS, 0, 0, 0 };   // 取系统实际行高，不靠猜
        if (ListView_GetItemRect(list_, 0, &ir, LVIR_BOUNDS))
            rowH = ir.bottom - ir.top;
        if (rowH < dpi::S(20)) rowH = dpi::S(38);
    }
    const int innerW = cw - CPAD * 2;

    // 列宽：先算好再定高度——横向滚动条会吃掉行高，必须保证列宽一定放得下
    {
        const int vres = GetSystemMetrics(SM_CXVSCROLL) + dpi::S(4);
        const int availW = innerW - vres;
        int w1 = dpi::S(240), w2 = dpi::S(160), w3 = dpi::S(120);
        const int w4min = dpi::S(140);
        if (availW - w1 - w2 - w3 < w4min) {
            double k = double(availW - w4min) / double(w1 + w2 + w3);
            if (k < 0.5) k = 0.5;
            w1 = (int)(w1 * k); w2 = (int)(w2 * k); w3 = (int)(w3 * k);
        }
        int w4 = availW - w1 - w2 - w3;
        if (w4 < dpi::S(80)) w4 = dpi::S(80);
        ListView_SetColumnWidth(list_, 0, w1);
        ListView_SetColumnWidth(list_, 1, w2);
        ListView_SetColumnWidth(list_, 2, w3);
        ListView_SetColumnWidth(list_, 3, w4);
    }

    // 表头高度由控件字体行高决定：给表头一个高行高字体专门用来撑高，
    // 标签本身走自绘、不受该字体影响。实测高度参与下面的整行取整。
    int hdrH = dpi::S(38);
    if (header_) {
        RECT hr = {};
        if (GetWindowRect(header_, &hr) && hr.bottom > hr.top)
            hdrH = hr.bottom - hr.top;
    }

    const int avail = rcCard_.bottom - listTop - CTH - CPAD - dpi::S(24);
    int listH = (rowCnt <= 0) ? dpi::S(176)
                              : hdrH + rowCnt * rowH + dpi::S(2);
    if (listH > avail - MIN_LOG) listH = avail - MIN_LOG;
    if (listH < dpi::S(120)) listH = dpi::S(120);
    // 表头 + 整数行：避免容器底部把最后一行拦腰截断
    if (rowCnt > 0) {
        int whole = (listH - hdrH) / rowH;
        if (whole < 1) whole = 1;
        listH = hdrH + whole * rowH + 1;
    }
    listH_ = listH;

    MoveWindow(list_, rcCard_.left + CPAD, listTop, innerW, listH, TRUE);
    const int logTitleTop = listTop + listH + dpi::S(18);
    SetRect(&rcCardLog_, rcCard_.left + CPAD, logTitleTop,
            rcCard_.right - CPAD, logTitleTop + CTH);
    int logTop = rcCardLog_.bottom + dpi::S(2);
    int logH = rcCard_.bottom - CPAD - logTop;
    if (logH < dpi::S(60)) logH = dpi::S(60);
    MoveWindow(logView_, rcCard_.left + CPAD, logTop, innerW, logH, TRUE);

    // ---- 页头右上：语言 / 主题 / 关于（图标按钮）+ 权限状态 ----
    {
        const int IB = dpi::S(34);
        const int by = dpi::S(22);
        const int aboutX = w - PAD - IB;
        const int themeX = aboutX - IB - dpi::S(4);
        const int langX = themeX - IB - dpi::S(4);
        MoveWindow(btnAbout_, aboutX, by, IB, IB, TRUE);
        MoveWindow(btnTheme_, themeX, by, IB, IB, TRUE);
        MoveWindow(btnLang_, langX, by, IB, IB, TRUE);

        const bool elev = IsElevated();
        const int adminW = dpi::S(122);
        const int adminX = langX - dpi::S(12) - adminW;
        MoveWindow(btnAdmin_, adminX, by + dpi::S(1), adminW, dpi::S(32), TRUE);
        ShowWindow(btnAdmin_, elev ? SW_HIDE : SW_SHOW);

        const int chipRight = elev ? (langX - dpi::S(12)) : (adminX - dpi::S(10));
        SetRect(&rcElevChip_, chipRight - dpi::S(96), by + dpi::S(1),
                chipRight, by + dpi::S(33));
    }
}

// ---------------------------------------------------------------- 绘制

void AppWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc; GetClientRect(hwnd_, &rc);

    ui::DoubleBuffer db;
    HDC mem = db.Begin(dc, rc);
    if (mem) {
        FillSolid(mem, rc, color::Canvas);
        DrawSidebar(mem, RECT{ 0, 0, dpi::S(size::SidebarWidth),
                               rc.bottom - dpi::S(size::StatusH) });
        DrawHeader(mem, RECT{ dpi::S(size::SidebarWidth), 0, rc.right,
                              dpi::S(size::HeaderH) });

        // 输入框外框：系统 EDIT 只画文字，圆角容器由这里补上。
        // 焦点环 = Accent 两档线宽中的强调档（手册：选中态/焦点环）。
        HWND focused = GetFocus();
        for (const RECT& f : { rcFieldPath_, rcFieldTarget_ }) {
            if (f.right <= f.left) continue;
            ui::FillRoundRect(mem, f, dpi::S(d::radius::Card), color::Canvas);
            bool isFocus = focused &&
                (PtInRect(&f, POINT{ (f.left + f.right) / 2,
                                     (f.top + f.bottom) / 2 }));
            if (isFocus) {
                RECT fi = f;
                InflateRect(&fi, -dpi::S(2), -dpi::S(2));
                ui::StrokeRoundRect(mem, fi, dpi::S(d::radius::Card), color::Accent, 2);
            } else {
                ui::StrokeRoundRect(mem, f, dpi::S(d::radius::Card), color::Hairline, 1);
            }
        }
        // 扩展字段：左侧标签 + 同款圆角输入容器
        const int LABELW = dpi::S(150);
        for (int i = 0; i < kMaxFields; ++i) {
            const RECT& f = rcFieldExtra_[i];
            if (f.right <= f.left) continue;
            ui::DrawText(mem, i < (int)curFields_.size() && curFields_[i].label
                                  ? T(curFields_[i].label) : L"",
                         RECT{ f.left - LABELW + dpi::S(4), f.top, f.left - dpi::S(6), f.bottom },
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                             DT_END_ELLIPSIS,
                         color::Text2, ui::res::Body);
            ui::FillRoundRect(mem, f, dpi::S(d::radius::Card), color::Canvas);
            ui::StrokeRoundRect(mem, f, dpi::S(d::radius::Card), color::Hairline, 1);
        }

        DrawDataCard(mem);
        DrawStatusBar(mem, RECT{ 0, rc.bottom - dpi::S(size::StatusH),
                                 rc.right, rc.bottom });
        db.End(dc);
    }
    EndPaint(hwnd_, &ps);
}

// 结果 + 日志的共用卡片容器：一张圆角白卡把数据区整体托起来，
// 内部用段标题与细分隔线分层；标题行右侧放当前状态，避免信息散落在页脚。
void AppWindow::DrawDataCard(HDC dc) {
    if (rcCard_.right <= rcCard_.left || rcCard_.bottom <= rcCard_.top) return;

    // 卡片：Canvas 底 + Hairline 描边 + 圆角 12（VI 卡片规格）
    ui::FillRoundRect(dc, rcCard_, dpi::S(d::radius::Card), color::Canvas);
    ui::StrokeRoundRect(dc, rcCard_, dpi::S(d::radius::Card), color::Hairline);

    auto section = [&](const RECT& r, const wchar_t* title, const wchar_t* right,
                       COLORREF rightColor) {
        RECT tr = { r.left, r.top, r.right - dpi::S(220), r.bottom };
        ui::DrawText(dc, title, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     color::Text, ui::res::BodyStrong);
        if (right && *right) {
            RECT rr = { r.right - dpi::S(240), r.top, r.right, r.bottom };
            ui::DrawText(dc, right, rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE |
                         DT_NOPREFIX | DT_END_ELLIPSIS, rightColor, ui::res::Small);
        }
    };

    wchar_t count[64] = { 0 };
    int rows = ListView_GetItemCount(list_);
    if (rows > 0) {
        wstring cf = Tx(L"%d 条记录");
        wsprintfW(count, cf.c_str(), rows);
    } else {
        wsprintfW(count, L"%s", T(L"暂无数据"));
    }

    section(rcCardList_, T(L"扫描结果"), count, color::Text3);

    const wchar_t* logRight = busy_ ? T(L"运行中 · Esc 可停止") : T(L"就绪");
    section(rcCardLog_, T(L"运行日志"), logRight,
            busy_ ? color::Accent : color::Text3);

    // 日志段上方的细分隔线
    int sepY = rcCardLog_.top - dpi::S(9);
    RECT sep = { rcCard_.left + dpi::S(space::L), sepY,
                 rcCard_.right - dpi::S(space::L), sepY + 1 };
    FillSolid(dc, sep, color::Hairline);
}

void AppWindow::DrawSidebar(HDC dc, const RECT& rc) {
    FillSolid(dc, rc, color::Side);
    RECT line = { rc.right - 1, rc.top, rc.right, rc.bottom };
    FillSolid(dc, line, color::Hairline);

    const int SB = rc.right;
    const int PADX = dpi::S(18);

    // ---- 品牌区：蝶形角标（变体 A / 墨暗 C）+ 产品名 + 英文标识 ----
    {
        RECT logo = { PADX, dpi::S(16), PADX + dpi::S(54), dpi::S(52) };
        brand::DrawButterfly(dc, logo,
                             d::CurrentTheme()->Dark ? brand::MarkVariant::ReverseC
                                                     : brand::MarkVariant::FullA,
                             *d::CurrentTheme());
    }

    ui::DrawText(dc, T(d::brand::kAppNameCn),
                 RECT{ PADX + dpi::S(62), dpi::S(15), SB - dpi::S(10), dpi::S(39) },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 color::Text, ui::res::Title);
    ui::DrawText(dc, d::brand::kAppNameEn,
                 RECT{ PADX + dpi::S(62), dpi::S(39), SB - dpi::S(10), dpi::S(57) },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 color::Text2, ui::res::Small);

    // ---- 搜索框（自绘圆角容器 Panel2 底，EDIT 在容器内部透明贴合）----
    RECT field = { dpi::S(16), dpi::S(size::SidebarHeader) + dpi::S(2),
                   SB - dpi::S(16), dpi::S(size::SidebarHeader) + dpi::S(40) };
    ui::FillRoundRect(dc, field, dpi::S(d::radius::Card), color::Hover);
    RECT gi = { field.left + dpi::S(10), field.top, field.left + dpi::S(30), field.bottom };
    ui::DrawGlyphCentered(dc, 0xE721, gi, color::Text2, ui::res::Icon);

    // ---- 导航滚动指示（系统滚动条太粗，这里用 3px 细条 Hairline）----
    int totalH = 0;
    for (const auto& e : navs_)
        totalH += dpi::S(e.isGroup ? size::NavGroup : size::NavItem);
    const int viewH = rcNav_.bottom - rcNav_.top;
    if (totalH > viewH && viewH > 0 && !navs_.empty()) {
        int top = (int)SendMessageW(nav_, LB_GETTOPINDEX, 0, 0);
        if (top < 0) top = 0;
        int offset = 0;
        for (int i = 0; i < top && i < (int)navs_.size(); ++i)
            offset += dpi::S(navs_[i].isGroup ? size::NavGroup : size::NavItem);
        int thumbH = std::max(dpi::S(30), viewH * viewH / totalH);
        int span = viewH - thumbH;
        int maxOffset = totalH - viewH;
        int ty = rcNav_.top + (maxOffset > 0 ? span * std::min(offset, maxOffset) / maxOffset : 0);
        RECT t = { SB - dpi::S(7), ty, SB - dpi::S(3), ty + thumbH };
        ui::FillRoundRect(dc, t, dpi::S(2), color::Hairline);
    }
}

void AppWindow::DrawHeader(HDC dc, const RECT& rc) {
    FillSolid(dc, rc, color::Canvas);
    IModule* m = CurModule();
    const int PAD = dpi::S(space::XXL);
    const int left = rc.left + PAD;
    const int rightEdge = rcElevChip_.right > 0 ? rcElevChip_.left - dpi::S(16)
                                                : rc.right - dpi::S(220);

    if (m) {
        const wstring title = Tx(m->Title() ? m->Title() : L"");
        // 模块页题 = 标题档 22/700（40px Display 只给关于页/闪屏）
        ui::DrawText(dc, title.c_str(),
                     RECT{ left, dpi::S(14), rightEdge, dpi::S(50) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                     color::Text, ui::res::Title);

        // 标题右侧的分组标签：AccentSoft 胶囊给页头一点色彩锚点
        const wstring group = Tx(m->Group() ? m->Group() : L"");
        if (!group.empty()) {
            SIZE ts{};
            { ui::Selected sel(dc, ui::res::Title);
              GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &ts); }
            SIZE gs{};
            { ui::Selected sel(dc, ui::res::Small);
              GetTextExtentPoint32W(dc, group.c_str(), (int)group.size(), &gs); }
            int chipX = left + std::min<int>(ts.cx, rightEdge - left - dpi::S(40)) + dpi::S(12);
            int chipW = gs.cx + dpi::S(20);
            if (chipX + chipW < rightEdge) {
                RECT chip = { chipX, dpi::S(24), chipX + chipW, dpi::S(46) };
                ui::FillRoundRect(dc, chip, dpi::S(d::radius::Pill), color::AccentSoft);
                ui::DrawText(dc, group.c_str(), chip,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                             color::Text2, ui::res::Small);
            }
        }

        // 引言 = 18/500（Lead 档）
        ui::DrawText(dc, Tx(m->Description() ? m->Description() : L"").c_str(),
                     RECT{ left, dpi::S(54), rightEdge, dpi::S(82) },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                     color::Text2, ui::res::Lead);
    }

    // ---- 权限状态胶囊（通道色语义：管理员 = ChIn 进，标准 = GoldText 信号）----
    const bool elev = IsElevated();
    RECT chip = rcElevChip_;
    if (chip.right > chip.left) {
        COLORREF dotC = elev ? color::ChIn : color::GoldText;
        ui::FillRoundRect(dc, chip, dpi::S(d::radius::Pill),
                          BlendColor(dotC, color::Canvas, 880));
        int cy = (chip.top + chip.bottom) / 2;
        int r = dpi::S(3);
        RECT dot = { chip.left + dpi::S(12), cy - r, chip.left + dpi::S(12) + r * 2, cy + r };
        ui::FillRoundRect(dc, dot, r, dotC);
        ui::DrawText(dc, elev ? T(L"管理员模式") : T(L"标准用户"),
                     RECT{ chip.left + dpi::S(24), chip.top,
                           chip.right - dpi::S(10), chip.bottom },
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                     dotC, ui::res::Small);
    }
}

void AppWindow::DrawStatusBar(HDC dc, const RECT& rc) {
    FillSolid(dc, rc, color::Canvas);
    RECT line = { rc.left, rc.top, rc.right, rc.top + 1 };
    FillSolid(dc, line, color::Hairline);

    const int PAD = dpi::S(space::XXL);
    // 左：版本与出品（与侧栏同宽，视觉上延续成一列）
    ui::DrawText(dc, (L"v" + std::to_wstring(k_AppVersionMajor) + L"." +
                      std::to_wstring(k_AppVersionMinor) + L"   ·   " +
                      d::brand::kPublisher).c_str(),
                 RECT{ dpi::S(18), rc.top, dpi::S(size::SidebarWidth) - dpi::S(10), rc.bottom },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                 color::Text3, ui::res::Small);

    wstring sum = result_.summary;
    if (!result_.hint.empty()) {
        if (!sum.empty()) sum += L"    ·  ";
        sum += result_.hint;
    }
    if (sum.empty()) sum = T(L"就绪");
    const int sb = dpi::S(size::SidebarWidth);
    ui::DrawText(dc, sum.c_str(),
                 RECT{ sb + PAD, rc.top, rc.right - dpi::S(360), rc.bottom },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                 color::Text2, ui::res::Small);

    wchar_t right[192];
    int pct = (int)(dpi::UserScale() * 100.0 + 0.5);
    wstring rf = Tx(L"%d 个模块    界面 %d%%");
    wsprintfW(right, rf.c_str(),
              (int)ModuleRegistry::Instance().All().size(), pct);
    ui::DrawText(dc, right,
                 RECT{ rc.right - dpi::S(360), rc.top, rc.right - PAD, rc.bottom },
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                 color::Text3, ui::res::Small);
}

void AppWindow::DrawNavItem(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1 || d->itemID >= navs_.size()) return;
    const NavEntry& e = navs_[d->itemID];
    bool sel = (d->itemID == (UINT)SendMessageW(nav_, LB_GETCURSEL, 0, 0));
    bool hover = ((int)d->itemID == hoverNav_);   // v2.9 T01：组头也响应悬停

    FillSolid(d->hDC, d->rcItem, color::Side);

    if (e.isGroup) {
        // 组头 = 导航骨架：正文半粗 + chevron 指示展开方向，整行可点击
        if (hover || sel) {
            RECT pillRc = { dpi::S(10), d->rcItem.top + dpi::S(2),
                            d->rcItem.right - dpi::S(4), d->rcItem.bottom - dpi::S(2) };
            ui::FillRoundRect(d->hDC, pillRc, dpi::S(d::radius::Pill),
                              hover ? color::Hover : color::AccentSoft);
        }
        RECT gr = { d->rcItem.left + dpi::S(22), d->rcItem.top,
                    d->rcItem.right - dpi::S(32), d->rcItem.bottom };
        ui::DrawText(d->hDC, e.text.c_str(), gr,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                     color::Text, ui::res::BodyStrong);
        // chevron：展开 ↓ 0xE70D / 收起 → 0xE76C（Segoe MDL2 同族字形，与排序箭头一致）
        wchar_t ch = IsNavGroupOpen(e.group) ? 0xE70D : 0xE76C;
        RECT cr = { d->rcItem.right - dpi::S(28), d->rcItem.top,
                    d->rcItem.right - dpi::S(10), d->rcItem.bottom };
        ui::DrawGlyphCentered(d->hDC, ch, cr, color::Text2, ui::res::IconSm);
        return;
    }

    // 选中 = Accent 胶囊（wing / 墨底 wing_on_ink），文字 OnAccent；
    // 悬停 = Hover 面轻染。
    if (sel || hover) {
        int t = sel ? (selAnim_ * 3 / 4 + 250) : 300;
        if (t > 1000) t = 1000;
        COLORREF pill = sel ? BlendColor(color::Side, color::Accent, t)
                            : BlendColor(color::Side, color::Hover, t);
        RECT pillRc = { dpi::S(10), d->rcItem.top + dpi::S(2),
                        d->rcItem.right - dpi::S(4), d->rcItem.bottom - dpi::S(2) };
        ui::FillRoundRect(d->hDC, pillRc, dpi::S(d::radius::Pill), pill);
    }

    int ic = mz::ui::icon::For(mods_[e.moduleIndex]->Id());
    RECT ir = { d->rcItem.left + dpi::S(21), d->rcItem.top,
                d->rcItem.left + dpi::S(43), d->rcItem.bottom };
    mz::ui::icon::Draw(d->hDC, ic, ir,
                       sel ? color::OnAccent : (hover ? color::Text : color::Text2));

    RECT tr = { d->rcItem.left + dpi::S(50), d->rcItem.top,
                d->rcItem.right - dpi::S(14), d->rcItem.bottom };
    ui::DrawText(d->hDC, e.text.c_str(), tr,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                 sel ? color::OnAccent : color::Text2,
                 sel ? ui::res::BodyStrong : ui::res::Body);
}

void AppWindow::DrawButton(const DRAWITEMSTRUCT* d) {
    // VI-D2 样式 → 令牌映射在 controls::DrawFluentButton 内固定：
    // Primary/Danger = BtnFill 墨底反白；Secondary = Hover 面 + Hairline 描边。
    // MDL2 字形：Search / Delete / Undo / Cancel / Check / Clear / Color / Info
    switch (d->CtlID) {
    case ID_BTN_RUN:   ui::DrawFluentButton(d, ui::ButtonStyle::Primary,
                                            color::Accent, 0xE721, color::Canvas); break;
    case ID_BTN_CLEAN: {
        // 模块可声明危险态（如「卸载选中」）；VI 纪律下危险确认仍 = 墨底反白
        IModule* cm = CurModule();
        if (cm && cm->Kind() == ModuleKind::Tweaks)
            ui::DrawFluentButton(d, ui::ButtonStyle::Primary,
                                 color::Accent, 0xE73E, color::Canvas);
        else
            ui::DrawFluentButton(d, ui::ButtonStyle::Danger,
                                 color::Accent, 0xE74D, color::Canvas);
        break;
    }
    case ID_BTN_UNDO:  ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::ChIn, 0xE72C, color::Canvas); break;
    case ID_BTN_MORE:  ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::Accent, 0xE70D /* ChevronDown */,
                                            color::Canvas); break;
    case ID_BTN_STOP:  ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::ChErr, 0xE8BB, color::Canvas); break;
    case ID_BTN_ALL:   ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::Accent, 0xE73E, color::Canvas); break;
    case ID_BTN_NONE:  ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::Accent, 0xE894, color::Canvas); break;
    case ID_BTN_ADMIN: ui::DrawFluentButton(d, ui::ButtonStyle::Secondary,
                                            color::GoldText, 0xE7EF, color::Canvas); break;
    case ID_BTN_THEME: ui::DrawIconButton(d, 0xE790, color::Canvas, color::Text2); break;
    case ID_BTN_LANG:  ui::DrawIconButton(d, 0xE8C1 /* World */, color::Canvas,
                                          color::Text2); break;
    case ID_BTN_ABOUT: ui::DrawIconButton(d, 0xE946, color::Canvas, color::Text2); break;
    case ID_SEARCH_CLEAR: ui::DrawSearchClearButton(d); break;
    case ID_NAV_EXPAND:   // 侧栏辅助小按钮：Ghost 样式融入侧栏底色
        ui::DrawFluentButton(d, ui::ButtonStyle::Ghost, color::Accent, 0xE70D,
                             color::Side);
        break;
    case ID_NAV_COLLAPSE:
        ui::DrawFluentButton(d, ui::ButtonStyle::Ghost, color::Accent, 0xE70E,
                             color::Side);
        break;
    default:           ui::DrawFluentButton(d, ui::ButtonStyle::Ghost); break;
    }
}

LRESULT AppWindow::ListCustomDraw(LPARAM lp) {
    auto* cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lp);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: {
        RECT rc; GetClientRect(list_, &rc);
        FillSolid(cd->nmcd.hdc, rc, color::Canvas);
        if (header_) {   // 空状态要避开表头，否则图形会被表头压掉一截
            RECT hr = {};
            if (GetWindowRect(header_, &hr)) rc.top += hr.bottom - hr.top;
        }
        if (ListView_GetItemCount(list_) == 0) {
            // v2.17 品牌化空/错误态：线稿走线 + 未画完焊盘（错误态断线，
            // 不上红）；模块差异只保留一句操作提示。
            IModule* m = CurModule();
            const bool action = m && (m->Kind() == ModuleKind::Action ||
                                      m->Kind() == ModuleKind::Console);
            const wchar_t* tip = action ? T(L"点击上方「执行」运行该功能")
                                        : T(L"点击上方「开始扫描」获取当前模块的数据");
            // 错误路径（无 winget / 非管理员 / 超时等）：hint/summary 带失败
            // 语义时切错误态线稿（信号在中途断了）
            const wstring failBlob = result_.hint + L" " + result_.summary;
            const bool failed =
                // 双语检测：模块 hint 已 T() 化（en 为英文），summary 仍是中文原文
                failBlob.find(L"错误") != wstring::npos || failBlob.find(T(L"错误")) != wstring::npos ||
                failBlob.find(L"失败") != wstring::npos || failBlob.find(T(L"失败")) != wstring::npos ||
                failBlob.find(L"无法") != wstring::npos || failBlob.find(T(L"无法")) != wstring::npos ||
                failBlob.find(L"超时") != wstring::npos || failBlob.find(T(L"超时")) != wstring::npos ||
                failBlob.find(L"未找到") != wstring::npos || failBlob.find(T(L"未找到")) != wstring::npos ||
                failBlob.find(L"不可用") != wstring::npos || failBlob.find(T(L"不可用")) != wstring::npos;
            if (failed)
                brand::DrawErrorState(cd->nmcd.hdc, rc, *d::CurrentTheme());
            else
                brand::DrawEmptyState(cd->nmcd.hdc, rc, *d::CurrentTheme());
            ui::DrawText(cd->nmcd.hdc, tip,
                         RECT{ rc.left + dpi::S(20), rc.bottom - dpi::S(40),
                               rc.right - dpi::S(20), rc.bottom - dpi::S(14) },
                         DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                         color::Text3, ui::res::Small);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        // 整行自绘：4 列文字、Hairline 行分隔、选中/危险底色全部由这里控制，
        // 不依赖系统 ListView 的主题绘制（深色系统下才不会串色）。
        const int idx = (int)cd->nmcd.dwItemSpec;
        if (idx < 0) return CDRF_SKIPDEFAULT;

        RECT row = {};
        if (!ListView_GetItemRect(list_, idx, &row, LVIR_BOUNDS)) return CDRF_SKIPDEFAULT;

        const bool danger = (idx < (int)result_.rows.size()) && result_.rows[idx].danger;
        // 不依赖 uItemState：部分系统上它在 owner-draw 里对每一行都为真，
        // 会把整张表刷成选中色。直接问控件真实选中状态。
        const bool selected = (ListView_GetItemState(list_, idx, LVIS_SELECTED) &
                               LVIS_SELECTED) != 0;
        COLORREF bg = color::Canvas;
        if (selected)     bg = color::AccentSoft;
        else if (danger)  bg = color::ChErrSoft;   // 危险行柔底（通道色只归数据层）
        FillSolid(cd->nmcd.hdc, row, bg);

        RECT hair = { row.left + dpi::S(14), row.bottom - 1,
                      row.right - dpi::S(14), row.bottom };
        FillSolid(cd->nmcd.hdc, hair, selected ? color::AccentSoft : color::Hairline);

        const int cols = header_ ? Header_GetItemCount(header_) : 4;
        IModule* curMod = CurModule();
        const bool tweaks = curMod && curMod->Kind() == ModuleKind::Tweaks;
        wchar_t buf[1024];
        for (int c = 0; c < cols; ++c) {
            RECT cr = row;
            if (header_ && Header_GetItemRect(header_, c, &cr)) {
                cr.top = row.top;
                cr.bottom = row.bottom;
            }
            ListView_GetItemText(list_, idx, c, buf, 1024);
            RECT tr = { cr.left + dpi::S(14), cr.top, cr.right - dpi::S(14), cr.bottom };
            UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
            fmt |= (c == 1) ? DT_RIGHT : DT_LEFT;   // 数值列右对齐
            COLORREF fg;
            if (danger)        fg = color::ChErr;
            else if (selected) fg = color::Text;
            else               fg = (c == 0) ? color::Text : color::Text2;
            // 开关型模块：状态列用通道色区分，扫一眼就知道哪些已开启
            // （Tweaks「已应用」= ChIn、「未应用」= Text3）
            if (tweaks && c == 1 && !danger) {
                // Tweaks 状态列文本来自模块行（中文原文），双语比较兼容后续英文化
                fg = (wcscmp(buf, L"已应用") == 0 || wcscmp(buf, T(L"已应用")) == 0) ? color::ChIn
                   : (wcscmp(buf, L"未应用") == 0 || wcscmp(buf, T(L"未应用")) == 0) ? color::Text3
                                                   : fg;
            }
            // 数值列 = Data 层等宽（JetBrains Mono 回落链），IP/端口天然对齐
            HFONT colFont = (c == 1) ? ui::res::Mono
                                     : (c == 0 ? ui::res::BodyStrong : ui::res::Body);
            ui::DrawText(cd->nmcd.hdc, buf, tr, fmt, fg, colFont);
        }
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT AppWindow::HeaderCustomDraw(LPARAM lp) {
    auto* cd = reinterpret_cast<LPNMCUSTOMDRAW>(lp);
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int col = (int)cd->dwItemSpec;
        RECT rc = cd->rc;
        FillSolid(cd->hdc, rc, color::Hover);
        RECT line = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
        FillSolid(cd->hdc, line, color::Hairline);

        const wchar_t* labels[] = { T(L"名称"), T(L"数值"), T(L"分类"), T(L"详情") };
        bool sorted = (sortCol_ == col);
        RECT tr = { rc.left + dpi::S(14), rc.top,
                    rc.right - (sorted ? dpi::S(34) : dpi::S(14)), rc.bottom };
        UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        if (col == 1) fmt |= DT_RIGHT;
        ui::DrawText(cd->hdc, T(labels[col]), tr, fmt,
                     sorted ? color::Accent : color::Text2, ui::res::SmallStrong);
        if (sorted) {
            wchar_t arrow = sortAsc_ ? 0xE70E : 0xE70D;  // Chevron Up/Down
            RECT ar = { rc.right - dpi::S(32), rc.top, rc.right - dpi::S(12), rc.bottom };
            ui::DrawGlyphCentered(cd->hdc, arrow, ar, color::Accent, ui::res::Icon);
        }
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

// ---------------------------------------------------------------- 消息

LRESULT AppWindow::OnCommand(WORD id, WORD code) {
    if (id == ID_NAV && code == LBN_SELCHANGE) {
        int sel = (int)SendMessageW(nav_, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)navs_.size()) return 0;
        if (navs_[sel].isGroup) {
            // v2.9 T01：组头点击已被 NavHoverProc 拦截为展开/收起；
            // 键盘方向键落到组头行时保持高亮即可，不再跳到组内第一项。
            return 0;
        }
        if (navs_[sel].moduleIndex != cur_) {
            SaveCurrentInputs();   // 切走前保存当前模块输入（v2.12 R1）
            cur_ = navs_[sel].moduleIndex;
            ApplyModuleToUi();
        }
        return 0;
    }
    if (id == ID_SEARCH && code == EN_CHANGE) {
        filter_ = GetWndText(searchEdit_);
        ui::RefreshSearchBox(searchEdit_, clearBtn_);
        BuildNav();
        return 0;
    }
    if (id == ID_SEARCH_CLEAR && code == BN_CLICKED) {
        SetWindowTextW(searchEdit_, L"");
        filter_.clear();
        ui::RefreshSearchBox(searchEdit_, clearBtn_);
        BuildNav();
        SetFocus(searchEdit_);
        return 0;
    }

    switch (id) {
    case ID_BTN_RUN:   StartWork(false); break;
    case ID_BTN_CLEAN: {
        // 扩展动作模块（Actions() 非空）：主按钮位 = 动作 0
        IModule* m = CurModule();
        if (m && !m->Actions().empty()) StartWork(true, false, 0);
        else StartWork(true, false);
        break;
    }
    case ID_BTN_UNDO: {
        // 扩展动作模块：第二按钮位 = 动作 1
        IModule* m = CurModule();
        if (m && m->Actions().size() > 1) StartWork(true, false, 1);
        else StartWork(true, true);
        break;
    }
    case ID_BTN_MORE:  ShowActionsMenu(); break;
    case ID_BTN_STOP:  StopWork(); break;
    case ID_NAV_EXPAND:   ExpandAllNav(); break;
    case ID_NAV_COLLAPSE: CollapseAllNav(); break;
    case ID_BTN_ALL:
        for (int i = 0; i < ListView_GetItemCount(list_); ++i)
            ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
        break;
    case ID_BTN_NONE:
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
        break;
    case ID_BTN_ADMIN:
        if (MessageBoxW(hwnd_, T(L"需要以管理员身份重新启动本程序。是否继续？"), T(L"提权"),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            mz::ElevateResult er = mz::RelaunchAsAdmin();
            if (er == mz::ElevateResult::Ok) {
                DestroyWindow(hwnd_);
            } else if (er == mz::ElevateResult::Cancelled) {
                // UAC 被取消：不再静默，Toast 明确告知当前仍为标准用户
                ui::ShowToast(hwnd_, T(L"已取消提权，继续以标准用户权限运行"),
                              color::GoldText, 0xE7EF /* Admin shield */);
            } else {
                ui::ShowToast(hwnd_, T(L"提权失败，请右键「以管理员身份运行」"),
                              color::ChErr, 0xE783 /* Error */);
            }
        }
        break;
    case ID_BTN_ABOUT:
        mz::ui::AboutDialog(hwnd_, (int)ModuleRegistry::Instance().All().size());
        break;
    case ID_BTN_THEME:
        mz::ui::ThemePickerDialog(hwnd_);
        break;
    case ID_BTN_LANG: {
        // 语言循环切换：简体 → 繁體 → English（v2.12 R4）
        using lang::Lang;
        Lang next = lang::Next(lang::Current());
        lang::SetLang(next);   // 切换 + 清翻译缓存
        lang::SaveLang();      // 落盘（重启保持）
        RefreshStaticTexts();
        ApplyModuleToUi();           // 回填按钮/占位符/字段并触发重排
        BuildNav();                  // 分组与模块标题按新语言重建
        InvalidateRect(hwnd_, nullptr, TRUE);
        ui::ShowToast(hwnd_, wstring(L"\xE8C1  ") + lang::NameOf(next),
                      color::Accent, 0);
        break;
    }
    }
    return 0;
}

LRESULT AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_SIZE:
        Layout(0, 0);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = dpi::S(1040);
        mm->ptMinTrackSize.y = dpi::S(660);
        return 0;
    }

    case WM_DPICHANGED: {
        auto* rc = reinterpret_cast<RECT*>(lp);
        dpi::SetCurrent(LOWORD(wp) ? LOWORD(wp) : HIWORD(wp));
        ui::InitResources(dpi::ForWindow(hwnd_));
        RebuildBrushes();
        ApplyFontsToChildren();
        SetupColumns();
        Layout(0, 0);
        SetWindowPos(hwnd_, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // 全部绘制在 WM_PAINT 双缓冲中完成

    case WM_PAINT:
        Paint();
        return 0;

    // ---- 「更多 ▾」扩展动作菜单的 owner-draw（v2.8）----
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mi && mi->CtlType == ODT_MENU &&
            mi->itemData >= (ULONG_PTR)kActMenuIdBase) {
            int ai = (int)mi->itemData - kActMenuIdBase;
            const wstring& s = (ai >= 2 && ai - 2 < (int)menuItems_.size())
                                   ? menuItems_[ai - 2].label
                                   : wstring(T(L"动作"));
            SIZE sz = { 0, 0 };
            HDC dc = GetDC(hwnd_);
            HFONT old = (HFONT)SelectObject(dc, ui::res::Body);
            GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
            mi->itemWidth = sz.cx + dpi::S(48);
            mi->itemHeight = dpi::S(34);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (d->CtlType == ODT_MENU) {
            // 「更多 ▾」扩展动作菜单项（owner-draw，danger 项红色）
            if (d->itemData >= (ULONG_PTR)kActMenuIdBase) {
                int ai = (int)d->itemData - kActMenuIdBase;
                if (ai >= 2 && ai - 2 < (int)menuItems_.size()) {
                    const ActionMenuItem& it = menuItems_[ai - 2];
                    bool hot = (d->itemState & ODS_SELECTED) != 0;
                    if (hot) FillSolid(d->hDC, d->rcItem, color::AccentSoft);
                    else FillSolid(d->hDC, d->rcItem, color::Canvas);
                    RECT tr = d->rcItem;
                    tr.left += dpi::S(16);
                    ui::DrawText(d->hDC, it.label.c_str(), tr,
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                     DT_END_ELLIPSIS,
                                 it.danger ? color::ChErr : color::Text,
                                 ui::res::Body);
                    return TRUE;
                }
            }
            return TRUE;   // 其它菜单项不存在（本菜单全部 owner-draw）
        }
        if (d->CtlID == ID_NAV) DrawNavItem(d);
        else DrawButton(d);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color::Text3);
        return (LRESULT)bSide_;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        if ((HWND)lp == searchEdit_) {
            SetBkColor(dc, color::Hover);
            SetTextColor(dc, color::Text);
            return (LRESULT)bField_;
        }
        SetBkColor(dc, color::Canvas);
        SetTextColor(dc, color::Text);
        return (LRESULT)bSurface_;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, color::Side);
        SetTextColor(dc, color::Text);
        return (LRESULT)bSide_;
    }

    case WM_NOTIFY: {
        auto* nh = reinterpret_cast<NMHDR*>(lp);
        if (nh->hwndFrom == list_) {
            if (nh->code == NM_CUSTOMDRAW) return ListCustomDraw(lp);
            if (nh->code == LVN_COLUMNCLICK) {
                auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
                SortByColumn(nmlv->iSubItem);
            }
            if (nh->code == NM_DBLCLK) {
                int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < (int)result_.rows.size()) {
                    auto& r = result_.rows[sel];
                    LogLine(L"· " + r.c1 + L"   " + r.c2 + L"   " + r.c3);
                    if (!r.c4.empty()) LogLine(L"   " + r.c4);
                }
            }
        } else if (header_ && nh->hwndFrom == header_ && nh->code == NM_CUSTOMDRAW) {
            return HeaderCustomDraw(lp);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerSelAnim) {
            selAnim_ += 1000 * d::motion::Frame / d::motion::Selection;
            if (selAnim_ >= 1000) { selAnim_ = 1000; KillTimer(hwnd_, kTimerSelAnim); }
            InvalidateRect(nav_, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_F5) { StartWork(false); return 0; }
        if (wp == VK_ESCAPE) {
            if (busy_) { StopWork(); return 0; }
            if (GetFocus() == searchEdit_ && !filter_.empty()) {
                SetWindowTextW(searchEdit_, L"");
                return 0;
            }
        }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            // 界面缩放：Ctrl + = 放大 / Ctrl + - 缩小 / Ctrl + 0 复位
            if (wp == VK_OEM_PLUS || wp == VK_ADD) { ChangeScale(+1); return 0; }
            if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) { ChangeScale(-1); return 0; }
            if (wp == '0' || wp == VK_NUMPAD0) { ChangeScale(0); return 0; }
        }
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SetFocus(searchEdit_);
            return 0;
        }
        if (wp == 'T' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Ctrl+T：纸亮 ⇄ 墨暗 双主题循环
            constexpr int kCount = (int)(sizeof(d::kAll) / sizeof(d::kAll[0]));
            int idx = 0;
            for (int i = 0; i < kCount; ++i) {
                if (d::kAll[i] == d::CurrentTheme()) { idx = i; break; }
            }
            const d::ThemeTokens* next = d::kAll[(idx + 1) % kCount];
            d::ApplyTheme(next);
            ui::ShowToast(hwnd_, T(L"主题：") + wstring(next->name),
                          color::Accent, 0xE790);
            return 0;
        }
        break;

    case WM_COMMAND:
        return OnCommand(LOWORD(wp), HIWORD(wp));

    case kMsgDone:
        OnWorkDone();
        return 0;

    case kMsgTick:
        if ((int)wp >= 0) SendMessageW(progress_, PBM_SETPOS, (WPARAM)wp, 0);
        FlushLog();
        return 0;

    case WM_DESTROY:
        OnDestroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int RunAppWindow(HINSTANCE hInst, int nCmdShow) {
    AppWindow app;
    return app.Run(hInst, nCmdShow);
}

} // namespace mz
