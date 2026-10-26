// ============================================================================
//  NetAdmin Toolbox — 设计系统（Design Tokens）
//  ---------------------------------------------------------------------------
//  biohacker.fun 旗舰产品视觉体系：多主题运行时切换
//
//  四套内置主题（侧栏主题按钮 / Ctrl+T 随时切换，选择持久化到 exe 同目录 ini）：
//    1. 晴空白 Daylight   —— 默认。冷调浅灰底 + 纯白卡面 + 靛蓝主色，清爽专业
//    2. 暖阳米 WarmPaper  —— 米纸暖底 + 陶土棕主色，长时间阅读不疲劳
//    3. 薄雾蓝 MistBlue   —— 雾蓝冷底 + 钢蓝主色，技术理性感
//    4. 深空夜 DeepSpace  —— 石墨深空暗色 + 亮靛蓝主色（低光环境/夜间使用）
//
//  实现要点：
//    * Palette 收编全部颜色；Theme = Palette + 元信息
//    * color:: 命名空间为 inline 变量（C++17），ApplyTheme 统一刷新，
//      全部调用点零改动；字体阶梯四主题共享，切换不重建字体
//    * 所有取值均为 96 DPI 基准像素，实际渲染统一走 ui::dpi::S() 缩放
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace mz {
namespace ui {
namespace design {

// ---------------------------------------------------------------- 调色板
//  一套主题的完整颜色集合。命名即语义，新增主题只需填一套值。
struct Palette {
    // 表面层次（由深到浅 / 由底到卡）
    COLORREF bg;          // 窗口底色
    COLORREF surface;     // 卡片 / 面板
    COLORREF surfaceHi;   // 悬停表面
    COLORREF surfaceSel;  // 选中表面
    COLORREF sidebar;     // 侧栏
    COLORREF sidebarHi;   // 侧栏悬停
    COLORREF console;     // 日志控制台
    // 描边
    COLORREF border;      // 强分隔线
    COLORREF borderSoft;  // 弱分隔线
    // 主色（主操作 / 选中态）
    COLORREF primary;
    COLORREF primaryHi;   // 悬停提亮
    COLORREF primaryDim;  // 按下压暗
    COLORREF primarySoft; // 选中底色（徽标/提示底）
    COLORREF primaryDisabled;  // 主按钮禁用态
    // 辅助色（克制使用：还原类次级信息操作）
    COLORREF cyan;
    COLORREF cyanSoft;
    // 语义色
    COLORREF danger;
    COLORREF dangerHi;
    COLORREF dangerDim;
    COLORREF dangerSoft;
    COLORREF dangerDisabled;
    COLORREF warn;
    COLORREF warnSoft;
    COLORREF ok;
    COLORREF okSoft;
    // 文本（四级对比）
    COLORREF text;
    COLORREF textSub;
    COLORREF textFaint;
    COLORREF onPrimary;   // 主色按钮上的文字
    COLORREF onDanger;    // 危险按钮上的文字
    // 列表
    COLORREF zebra;       // 斑马纹
    // 标题栏（Win11 沉浸式由 DWM 负责，此处为自绘兜底）
    COLORREF caption;
};

// ---------------------------------------------------------------- 主题
struct Theme {
    const wchar_t* id;    // 持久化用唯一 ID
    const wchar_t* name;  // 显示名
    const wchar_t* desc;  // 一句话描述
    Palette pal;
    bool dark;            // 是否暗色（驱动标题栏/滚动条 chrome）
};

namespace Themes {

// 1. 晴空白（默认）—— 冷调中性灰 + 纯白卡面 + 靛蓝主色，克制而清晰
inline const Theme Daylight{
    L"daylight", L"晴空白", L"冷调浅灰 + 纯白卡面 + 靛蓝主色",
    {
        /* bg */            RGB(0xF5, 0xF6, 0xF9),
        /* surface */       RGB(0xFF, 0xFF, 0xFF),
        /* surfaceHi */     RGB(0xF2, 0xF4, 0xF8),
        /* surfaceSel */    RGB(0xED, 0xF1, 0xFE),
        /* sidebar */       RGB(0xFF, 0xFF, 0xFF),
        /* sidebarHi */     RGB(0xF2, 0xF4, 0xF8),
        /* console */       RGB(0xFA, 0xFB, 0xFD),
        /* border */        RGB(0xE3, 0xE7, 0xEE),
        /* borderSoft */    RGB(0xED, 0xF0, 0xF5),
        /* primary */       RGB(0x3E, 0x63, 0xDD),
        /* primaryHi */     RGB(0x55, 0x78, 0xE8),
        /* primaryDim */    RGB(0x31, 0x50, 0xBE),
        /* primarySoft */   RGB(0xEE, 0xF2, 0xFF),
        /* primaryDisabled */ RGB(0xCB, 0xD6, 0xF6),
        /* cyan */          RGB(0x0E, 0x8A, 0x7E),
        /* cyanSoft */      RGB(0xE6, 0xF5, 0xF2),
        /* danger */        RGB(0xD6, 0x45, 0x45),
        /* dangerHi */      RGB(0xE4, 0x5C, 0x5C),
        /* dangerDim */     RGB(0xB5, 0x37, 0x37),
        /* dangerSoft */    RGB(0xFD, 0xF0, 0xF0),
        /* dangerDisabled */ RGB(0xF2, 0xD4, 0xD4),
        /* warn */          RGB(0xB4, 0x53, 0x09),
        /* warnSoft */      RGB(0xFF, 0xF6, 0xE9),
        /* ok */            RGB(0x0E, 0x8A, 0x5F),
        /* okSoft */        RGB(0xE7, 0xF6, 0xEF),
        /* text */          RGB(0x10, 0x18, 0x28),
        /* textSub */       RGB(0x4A, 0x55, 0x65),
        /* textFaint */     RGB(0x8B, 0x95, 0xA5),
        /* onPrimary */     RGB(0xFF, 0xFF, 0xFF),
        /* onDanger */      RGB(0xFF, 0xFF, 0xFF),
        /* zebra */         RGB(0xFA, 0xFB, 0xFD),
        /* caption */       RGB(0xFF, 0xFF, 0xFF),
    },
    false
};

// 2. 暖阳米 —— 米纸暖底 + 陶土棕主色，护眼纸质感
inline const Theme WarmPaper{
    L"warmpaper", L"暖阳米", L"米纸暖底 + 陶土棕主色，护眼",
    {
        /* bg */            RGB(0xF6, 0xF1, 0xE7),
        /* surface */       RGB(0xFF, 0xFD, 0xF8),
        /* surfaceHi */     RGB(0xF5, 0xEF, 0xE3),
        /* surfaceSel */    RGB(0xFB, 0xEF, 0xDC),
        /* sidebar */       RGB(0xFF, 0xFD, 0xF8),
        /* sidebarHi */     RGB(0xF3, 0xEC, 0xDF),
        /* console */       RGB(0xFB, 0xF7, 0xEE),
        /* border */        RGB(0xE3, 0xD9, 0xC6),
        /* borderSoft */    RGB(0xED, 0xE4, 0xD4),
        /* primary */       RGB(0xB4, 0x53, 0x09),
        /* primaryHi */     RGB(0xC9, 0x6A, 0x15),
        /* primaryDim */    RGB(0x8F, 0x42, 0x07),
        /* primarySoft */   RGB(0xFB, 0xEF, 0xDD),
        /* primaryDisabled */ RGB(0xE8, 0xD5, 0xBC),
        /* cyan */          RGB(0x4F, 0x7F, 0x7A),
        /* cyanSoft */      RGB(0xE7, 0xF0, 0xEE),
        /* danger */        RGB(0xB9, 0x4A, 0x3C),
        /* dangerHi */      RGB(0xCE, 0x63, 0x54),
        /* dangerDim */     RGB(0x97, 0x3C, 0x30),
        /* dangerSoft */    RGB(0xF9, 0xEC, 0xE8),
        /* dangerDisabled */ RGB(0xEB, 0xD8, 0xD2),
        /* warn */          RGB(0x9C, 0x6B, 0x12),
        /* warnSoft */      RGB(0xF9, 0xF1, 0xDB),
        /* ok */            RGB(0x5A, 0x7D, 0x2E),
        /* okSoft */        RGB(0xEE, 0xF3, 0xE1),
        /* text */          RGB(0x2A, 0x24, 0x1B),
        /* textSub */       RGB(0x6A, 0x61, 0x53),
        /* textFaint */     RGB(0x9C, 0x93, 0x84),
        /* onPrimary */     RGB(0xFF, 0xFF, 0xFF),
        /* onDanger */      RGB(0xFF, 0xFF, 0xFF),
        /* zebra */         RGB(0xFC, 0xF8, 0xEF),
        /* caption */       RGB(0xFF, 0xFD, 0xF8),
    },
    false
};

// 3. 薄雾蓝 —— 雾蓝冷底 + 钢蓝主色，技术理性
inline const Theme MistBlue{
    L"mistblue", L"薄雾蓝", L"雾蓝冷底 + 钢蓝主色",
    {
        /* bg */            RGB(0xED, 0xF1, 0xF7),
        /* surface */       RGB(0xFF, 0xFF, 0xFF),
        /* surfaceHi */     RGB(0xF1, 0xF5, 0xFA),
        /* surfaceSel */    RGB(0xE8, 0xF1, 0xFC),
        /* sidebar */       RGB(0xFF, 0xFF, 0xFF),
        /* sidebarHi */     RGB(0xF0, 0xF4, 0xF9),
        /* console */       RGB(0xF8, 0xFA, 0xFD),
        /* border */        RGB(0xD8, 0xE1, 0xEC),
        /* borderSoft */    RGB(0xE6, 0xEC, 0xF4),
        /* primary */       RGB(0x1F, 0x6F, 0xB2),
        /* primaryHi */     RGB(0x2E, 0x86, 0xCC),
        /* primaryDim */    RGB(0x18, 0x5A, 0x91),
        /* primarySoft */   RGB(0xE8, 0xF2, 0xFB),
        /* primaryDisabled */ RGB(0xC9, 0xDC, 0xEE),
        /* cyan */          RGB(0x0E, 0x7C, 0x86),
        /* cyanSoft */      RGB(0xE3, 0xF1, 0xF2),
        /* danger */        RGB(0xC9, 0x45, 0x3F),
        /* dangerHi */      RGB(0xDB, 0x5E, 0x57),
        /* dangerDim */     RGB(0xA6, 0x37, 0x32),
        /* dangerSoft */    RGB(0xFB, 0xEC, 0xEB),
        /* dangerDisabled */ RGB(0xEF, 0xD8, 0xD6),
        /* warn */          RGB(0x9A, 0x6B, 0x12),
        /* warnSoft */      RGB(0xF8, 0xF1, 0xDC),
        /* ok */            RGB(0x1D, 0x7F, 0x5F),
        /* okSoft */        RGB(0xE5, 0xF4, 0xEE),
        /* text */          RGB(0x0F, 0x1B, 0x2A),
        /* textSub */       RGB(0x46, 0x58, 0x6B),
        /* textFaint */     RGB(0x84, 0x94, 0xA6),
        /* onPrimary */     RGB(0xFF, 0xFF, 0xFF),
        /* onDanger */      RGB(0xFF, 0xFF, 0xFF),
        /* zebra */         RGB(0xF6, 0xF9, 0xFD),
        /* caption */       RGB(0xFF, 0xFF, 0xFF),
    },
    false
};

// 4. 深空夜 —— 暗色备选（低光环境）
inline const Theme DeepSpace{
    L"deepspace", L"深空夜", L"石墨深空暗色，夜间低光",
    {
        /* bg */            RGB(0x0A, 0x0C, 0x11),
        /* surface */       RGB(0x11, 0x14, 0x1B),
        /* surfaceHi */     RGB(0x17, 0x1B, 0x24),
        /* surfaceSel */    RGB(0x1A, 0x21, 0x36),
        /* sidebar */       RGB(0x0D, 0x10, 0x16),
        /* sidebarHi */     RGB(0x16, 0x1A, 0x22),
        /* console */       RGB(0x0C, 0x0F, 0x15),
        /* border */        RGB(0x23, 0x29, 0x36),
        /* borderSoft */    RGB(0x1A, 0x1F, 0x29),
        /* primary */       RGB(0x7C, 0x93, 0xFF),
        /* primaryHi */     RGB(0x93, 0xA6, 0xFF),
        /* primaryDim */    RGB(0x5F, 0x76, 0xE0),
        /* primarySoft */   RGB(0x1A, 0x21, 0x40),
        /* primaryDisabled */ RGB(0x23, 0x2A, 0x42),
        /* cyan */          RGB(0x4F, 0xC1, 0xB5),
        /* cyanSoft */      RGB(0x11, 0x28, 0x2A),
        /* danger */        RGB(0xF0, 0x6A, 0x6A),
        /* dangerHi */      RGB(0xF5, 0x8A, 0x8A),
        /* dangerDim */     RGB(0xC9, 0x4B, 0x4B),
        /* dangerSoft */    RGB(0x2A, 0x16, 0x18),
        /* dangerDisabled */ RGB(0x2A, 0x18, 0x1B),
        /* warn */          RGB(0xE0, 0xA6, 0x4A),
        /* warnSoft */      RGB(0x2A, 0x21, 0x13),
        /* ok */            RGB(0x4E, 0xCB, 0x8E),
        /* okSoft */        RGB(0x12, 0x26, 0x1C),
        /* text */          RGB(0xE8, 0xEC, 0xF2),
        /* textSub */       RGB(0x9B, 0xA6, 0xB6),
        /* textFaint */     RGB(0x62, 0x6D, 0x7D),
        /* onPrimary */     RGB(0x0A, 0x0F, 0x24),
        /* onDanger */      RGB(0xFF, 0xFF, 0xFF),
        /* zebra */         RGB(0x14, 0x18, 0x22),
        /* caption */       RGB(0x0A, 0x0C, 0x11),
    },
    true
};

// 全部主题（切换器 / 持久化按此表遍历）
inline constexpr const Theme* kAll[] = { &Daylight, &WarmPaper, &MistBlue, &DeepSpace };

} // namespace Themes

// ---------------------------------------------------------------- 当前生效值
//  inline 变量：ApplyTheme 统一刷新，所有绘制调用点零改动。
//  注意：控件默认参数（如 accent = color::Primary）在每次调用时求值，
//  切换主题后自动取到新值。
namespace color {
inline COLORREF Bg              = Themes::Daylight.pal.bg;
inline COLORREF Surface         = Themes::Daylight.pal.surface;
inline COLORREF SurfaceHi       = Themes::Daylight.pal.surfaceHi;
inline COLORREF SurfaceSel      = Themes::Daylight.pal.surfaceSel;
inline COLORREF Sidebar         = Themes::Daylight.pal.sidebar;
inline COLORREF SidebarHi       = Themes::Daylight.pal.sidebarHi;
inline COLORREF Console         = Themes::Daylight.pal.console;
inline COLORREF Border          = Themes::Daylight.pal.border;
inline COLORREF BorderSoft      = Themes::Daylight.pal.borderSoft;
inline COLORREF Primary         = Themes::Daylight.pal.primary;
inline COLORREF PrimaryHi       = Themes::Daylight.pal.primaryHi;
inline COLORREF PrimaryDim      = Themes::Daylight.pal.primaryDim;
inline COLORREF PrimarySoft     = Themes::Daylight.pal.primarySoft;
inline COLORREF PrimaryDisabled = Themes::Daylight.pal.primaryDisabled;
inline COLORREF Cyan            = Themes::Daylight.pal.cyan;
inline COLORREF CyanSoft        = Themes::Daylight.pal.cyanSoft;
inline COLORREF Danger          = Themes::Daylight.pal.danger;
inline COLORREF DangerHi        = Themes::Daylight.pal.dangerHi;
inline COLORREF DangerDim       = Themes::Daylight.pal.dangerDim;
inline COLORREF DangerSoft      = Themes::Daylight.pal.dangerSoft;
inline COLORREF DangerDisabled  = Themes::Daylight.pal.dangerDisabled;
inline COLORREF Warn            = Themes::Daylight.pal.warn;
inline COLORREF WarnSoft        = Themes::Daylight.pal.warnSoft;
inline COLORREF Ok              = Themes::Daylight.pal.ok;
inline COLORREF OkSoft          = Themes::Daylight.pal.okSoft;
inline COLORREF Text            = Themes::Daylight.pal.text;
inline COLORREF TextSub         = Themes::Daylight.pal.textSub;
inline COLORREF TextFaint       = Themes::Daylight.pal.textFaint;
inline COLORREF OnPrimary       = Themes::Daylight.pal.onPrimary;
inline COLORREF OnDanger        = Themes::Daylight.pal.onDanger;
inline COLORREF Zebra           = Themes::Daylight.pal.zebra;
inline COLORREF Caption         = Themes::Daylight.pal.caption;
} // namespace color

// ---------------------------------------------------------------- 字体阶梯
//  四主题共享同一阶梯（切换不重建字体）。
//  协调原则：相邻层级差 ≥2px；标题/正文对比明确；辅助信息不低于 13px。
//  96 DPI 基准下正文 17px —— 微软雅黑 14/15px 时笔画偏细，实际屏幕上
//  「看着小」；17px 相当于印刷四号字，长时间盯屏幕不费眼。
//  需要更大仍可 Ctrl+= 整体缩放（见 dpi::SetUserScale）。
namespace font {
constexpr int PageTitle   = 27;   // 页面大标题（semibold）
constexpr int Title       = 19;   // 区块标题 / 侧栏品牌名 / 模态标题（semibold）
constexpr int Body        = 17;   // 正文 / 按钮 / 导航 / 列表 / 输入框
constexpr int Small       = 14;   // 辅助说明 / 状态栏 / 模态正文
constexpr int SmallStrong = 14;   // 分组标题 / 列表列头（semibold）
constexpr int Tiny        = 13;   // 侧栏页脚
constexpr int Mono        = 16;   // 日志控制台（与正文同族，保证中文回退一致）
} // namespace font

// ---------------------------------------------------------------- 间距 / 尺寸
namespace space {
constexpr int XS = 4;
constexpr int S  = 8;
constexpr int M  = 12;
constexpr int L  = 16;
constexpr int XL = 20;
constexpr int XXL = 24;
} // namespace space

namespace size {
constexpr int SidebarWidth   = 276;  // 侧栏宽（导航文字 17px 需要呼吸感）
constexpr int SidebarHeader  = 78;   // 侧栏品牌区高
constexpr int NavItem        = 42;   // 导航项高（v2.9：二级模块行，46→42 视觉从属）
constexpr int NavGroup       = 44;   // 分组标题高（v2.9：一级骨架行，36→44 容纳 17px 半粗）
constexpr int ButtonH        = 44;   // 主按钮高
constexpr int ButtonHS       = 34;   // 小按钮高
constexpr int EditH          = 40;   // 输入框高
constexpr int HeaderH        = 98;   // 内容区页头高
constexpr int StatusH        = 36;   // 状态栏高
constexpr int LogMinH        = 150;  // 日志区最小高（内容少时自动放大）
constexpr int BannerH        = 44;   // 保留（旧布局的横幅高度）
constexpr int ToastW         = 380;  // Toast 宽
constexpr int CardPad        = 16;   // 卡片内边距
constexpr int CardTitleH     = 34;   // 卡片段标题行高
} // namespace size

// ---------------------------------------------------------------- 圆角
namespace radius {
constexpr int Button = 9;
constexpr int Card   = 12;
constexpr int Field  = 9;
constexpr int Pill   = 18;   // 全圆角（状态胶囊、徽标）
} // namespace radius

// ---------------------------------------------------------------- 动效（毫秒）
namespace motion {
constexpr int Hover     = 120;   // 悬停淡入
constexpr int Selection = 180;   // 导航选中滑动
constexpr int ToastIn   = 200;   // Toast 入场
constexpr int ToastHold = 2600;  // Toast 停留
constexpr int ToastOut  = 240;   // Toast 退场
constexpr int Frame     = 16;    // 动画帧间隔（≈60fps）
} // namespace motion

// ---------------------------------------------------------------- 标识
namespace brand {
constexpr const wchar_t* kAppNameCn   = L"网管工具箱";
constexpr const wchar_t* kAppNameEn   = L"NetAdmin Toolbox";
constexpr const wchar_t* kTaglineCn   = L"系统清理 · 网络诊断 · Windows 优化";
constexpr const wchar_t* kPublisher   = L"biohacker.fun";
constexpr const wchar_t* kPublisherCn = L"biohacker.fun 旗舰产品";
constexpr const wchar_t* kSiteUrl     = L"https://biohacker.fun";
constexpr const wchar_t* kSlogan      = L"Decode your biology. Optimize your life.";
} // namespace brand

// ---------------------------------------------------------------- 主题操作
const Theme* CurrentTheme();                       // 当前主题
const Theme* FindTheme(const wchar_t* id);         // 按 ID 查找（找不到返回 nullptr）
void ApplyTheme(const Theme* t);                   // 切换：刷新令牌 + 持久化 + 通知
void LoadThemeOnStartup();                         // 启动时读取持久化选择
using ThemeChangedFn = void (*)(void*);            // 主题变更回调（重画画刷等）
void SetThemeChangedCallback(ThemeChangedFn fn, void* ctx);

// 界面缩放持久化（与主题同一个 ini，独立键）
void SaveUserScale(double scale);                  // 按百分比整数存储，规避小数点 locale
double LoadUserScale();                            // 未设置时返回 1.0

// 侧栏展开状态持久化（v2.9 T01，与缩放同一个 ini，键 ui/navExpanded）
// 值 = 分号分隔的展开组名列表；全收起时写占位符 "-"；
// 键缺失/空值 = 全展开（v2.8 用户零学习成本的缺省态）。
void SaveNavExpanded(const wchar_t* csv);          // 空串自动写占位符
std::wstring LoadNavExpanded();                    // 原样返回（含占位符 "-"），缺省返回空

// ---------------------------------------------------------------- 模块输入持久化（v2.12 R1）
// 每个模块的输入快照（路径/目标/扩展字段）按模块 ID 存进同一个 ini 的
// [input:<模块ID>] 节。界面在「切模块 / 执行 / 退出」时保存，选中模块时回填，
// 实现「填过一次，下次启动即默认值」。secret 位为 true 的字段先经 DPAPI
// （CryptProtectData，绑定当前用户）加密再落盘，密码不以明文出现在 ini。
struct ModuleInputSnapshot {
    std::wstring path, target;
    std::vector<std::wstring> fields;
    std::vector<bool> secret;              // 与 fields 对齐；缺省视为 false
};
void SaveModuleInput(const wchar_t* id, const ModuleInputSnapshot& s);
bool LoadModuleInput(const wchar_t* id, ModuleInputSnapshot& out);   // false = 无存档

} // namespace design
} // namespace ui
} // namespace mz
