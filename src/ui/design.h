// ============================================================================
//  NetAdmin Toolbox — 设计系统 v2.17「VI-D2 蜕变之翼」
//  ---------------------------------------------------------------------------
//  biohacker.fun VI-D2 品牌体系落地（docs/vi-d2-tokens.md 为最高依据）：
//    * 双主题：纸亮 kPaperLight（默认）/ 墨暗 kInkDark，实例定义在 design.cpp
//    * ThemeTokens 字段 = 全部 VI token（§1.7 映射表逐字段一致）
//    * color:: 命名空间为「角色令牌」——绘制代码唯一取色入口；
//      身份色原值（Paper/Ink/Wing/Signal…）只允许 design.cpp（定义）与
//      brand.cpp（品牌图形）引用
//    * 七级字阶（96 基准 px，14.5 用 double 走 dpi::S(double)）
//    * fontsys：内嵌字体子集内存加载（Baloo 2 / JetBrains Mono，OFL 1.1）
//    * 主题持久化沿用 exe 同目录 ini，键 ui/theme=paper|ink
//      （旧 4 值兼容：daylight|warmpaper|mistblue→paper，deepspace→ink）
//  墨底纪律（违者打回）：墨底唯一彩色 = 金 #F2B441；藤紫一律 #B293FC；
//  暗主题数据面（表格/日志）一律铺 ink —— panel_dark 只用于侧栏与 chrome。
//
//  字体许可：Baloo 2 / JetBrains Mono — SIL Open Font License 1.1,
//  embedded as subset under OFL-labeled embedding terms（全文见
//  src/res/fonts/OFL.txt；许可与版权字段随子集保留）。
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace mz {
namespace ui {
namespace design {

// ---------------------------------------------------------------- 主题令牌
//  字段 = 手册 token + 区域角色。两套实例 kPaperLight / kInkDark 定义在
//  design.cpp（色值只出现在那里，供 qa_contrast.py 解析自检）。
struct ThemeTokens {
    // ---- 身份层（两主题同值，手册原值）----
    COLORREF Paper, Ink, Wing, Signal, SignalText, WingOnInk;
    COLORREF ChIn, ChOut, ChErr;              // 数据通道（亮底原值）
    // ---- 中性派生 + 墨底变体（§1.6 推导表）----
    COLORREF Panel, Panel2, Line, Ink70, Ink55;      // 亮=手册原值，暗=派生
    COLORREF PanelD, Panel2D, LineD, Ink70D, Ink55D; // 暗变体（亮主题下冗余同值）
    COLORREF ChInD, ChOutD, ChErrD, ChErrSoftD, ChErrSoft;
    // ---- 区域角色（绘制代码唯一取色入口）----
    COLORREF Canvas, Side, Hover, Text, Text2, Text3;
    COLORREF Accent, OnAccent, AccentHover, AccentPressed, AccentSoft;
    COLORREF BtnFill, BtnText, Gold, GoldText, Hairline, Zebra;
    COLORREF Cap, CapText;
    bool     Dark;
    const wchar_t* id;      // "paper" / "ink"
    const wchar_t* name;    // 纸亮 / 墨暗
    const wchar_t* desc;
};

// 双主题实例（定义在 design.cpp；色值不出现在头文件）
extern const ThemeTokens kPaperLight;
extern const ThemeTokens kInkDark;

// 全部主题（切换器 / 持久化 / Ctrl+T 循环按此表遍历）
inline const ThemeTokens* const kAll[] = { &kPaperLight, &kInkDark };

// ---------------------------------------------------------------- 当前生效值
//  角色令牌：ApplyTheme 统一刷新，所有绘制调用点每次绘制即时读取。
//  注意：控件默认参数（如 accent = color::Accent）在每次调用时求值，
//  切换主题后自动取到新值。
namespace color {
// 身份层（brand 品牌图形使用）
inline COLORREF Paper      = kPaperLight.Paper;
inline COLORREF Ink        = kPaperLight.Ink;
inline COLORREF Wing       = kPaperLight.Wing;
inline COLORREF Signal     = kPaperLight.Signal;
inline COLORREF SignalText = kPaperLight.SignalText;
inline COLORREF WingOnInk  = kPaperLight.WingOnInk;
// 中性派生
inline COLORREF Panel  = kPaperLight.Panel;
inline COLORREF Panel2 = kPaperLight.Panel2;
inline COLORREF Line   = kPaperLight.Line;
inline COLORREF Ink70  = kPaperLight.Ink70;
inline COLORREF Ink55  = kPaperLight.Ink55;
// 数据通道（主题化：暗主题自动取墨底变体）
inline COLORREF ChIn      = kPaperLight.ChIn;
inline COLORREF ChOut     = kPaperLight.ChOut;
inline COLORREF ChErr     = kPaperLight.ChErr;
inline COLORREF ChErrSoft = kPaperLight.ChErrSoft;
// 墨底变体（brand::DrawButterfly 变体 C 等显式场景使用）
inline COLORREF PanelD   = kPaperLight.PanelD;
inline COLORREF Panel2D  = kPaperLight.Panel2D;
inline COLORREF LineD    = kPaperLight.LineD;
inline COLORREF Ink70D   = kPaperLight.Ink70D;
inline COLORREF Ink55D   = kPaperLight.Ink55D;
inline COLORREF ChInD    = kPaperLight.ChInD;
inline COLORREF ChOutD   = kPaperLight.ChOutD;
inline COLORREF ChErrD   = kPaperLight.ChErrD;
inline COLORREF ChErrSoftD = kPaperLight.ChErrSoftD;
// 区域角色（绘制代码从这里取色）
inline COLORREF Canvas        = kPaperLight.Canvas;
inline COLORREF Side          = kPaperLight.Side;
inline COLORREF Hover         = kPaperLight.Hover;
inline COLORREF Text          = kPaperLight.Text;
inline COLORREF Text2         = kPaperLight.Text2;
inline COLORREF Text3         = kPaperLight.Text3;
inline COLORREF Accent        = kPaperLight.Accent;
inline COLORREF OnAccent      = kPaperLight.OnAccent;
inline COLORREF AccentHover   = kPaperLight.AccentHover;
inline COLORREF AccentPressed = kPaperLight.AccentPressed;
inline COLORREF AccentSoft    = kPaperLight.AccentSoft;
inline COLORREF BtnFill       = kPaperLight.BtnFill;
inline COLORREF BtnText       = kPaperLight.BtnText;
inline COLORREF Gold          = kPaperLight.Gold;
inline COLORREF GoldText      = kPaperLight.GoldText;
inline COLORREF Hairline      = kPaperLight.Hairline;
inline COLORREF Zebra         = kPaperLight.Zebra;
inline COLORREF Cap           = kPaperLight.Cap;
inline COLORREF CapText       = kPaperLight.CapText;
} // namespace color

// ---------------------------------------------------------------- 字体阶梯
//  七级字阶（96 基准 px；14.5 用 double 走 dpi::S(double)）。
//  Wordmark/Display 仅关于页与闪屏；应用内模块页题 = Title(22) 档。
namespace font {
inline constexpr double Wordmark = 88;   // 字标 88/800（仅关于页）
inline constexpr double Display  = 40;   // 页题 40/800（关于页/闪屏大题）
inline constexpr double Title    = 22;   // 标题 22/700（模块页题/卡片标题）
inline constexpr double Lead     = 18;   // 引言 18/500（页头导语）
inline constexpr double Body     = 16;   // 正文 16/400-700（正文/按钮/导航）
inline constexpr double Label    = 14.5; // 标签 14.5/500（角标/列头）
inline constexpr double Data     = 15;   // 数据 15/400-600（等宽数据/时间戳）
} // namespace font

namespace fontsys {
bool LoadEmbeddedFonts();               // AddFontMemResourceEx ×3，先于 InitResources
bool HasDisplayFont();                  // Baloo 2 是否注册成功
bool HasMonoFont();                     // JetBrains Mono 是否注册成功
bool FamilyAvailable(const wchar_t* face);  // 系统是否安装指定字族（回落链探测）
HFONT DataFont(int px, bool semibold);  // JetBrains Mono → Cascadia Mono → Consolas
} // namespace fontsys

// ---------------------------------------------------------------- 间距 / 尺寸
namespace space {
constexpr int XS = 4;
constexpr int S  = 8;
constexpr int M  = 12;
constexpr int L  = 16;
constexpr int XL = 20;
constexpr int XXL = 24;   // 12 栏网格栏距（手册 §3）
} // namespace space

namespace size {
constexpr int SidebarWidth   = 276;  // 侧栏宽（导航文字 16px 需要呼吸感）
constexpr int SidebarHeader  = 78;   // 侧栏品牌区高
constexpr int NavItem        = 42;   // 导航项高
constexpr int NavGroup       = 44;   // 分组标题高
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
//  两档制（手册 §3）：卡片 12 / 胶囊 999。旧 Button/Field 第三档（9）已归并。
namespace radius {
constexpr int Card   = 12;   // 卡片 / 输入框 / 模态
constexpr int Pill   = 999;  // 按钮 / 导航胶囊 / 徽标（FillRoundRect 内部钳制）
} // namespace radius

// ---------------------------------------------------------------- 动效（毫秒）
namespace motion {
constexpr int Hover     = 120;   // 悬停淡入
constexpr int Selection = 180;   // 导航选中滑动
constexpr int ToastIn   = 200;   // Toast 入场
constexpr int ToastHold = 2600;  // Toast 停留
constexpr int ToastOut  = 240;   // Toast 退场
constexpr int Frame     = 16;    // 动画帧间隔（≈60fps）
constexpr int SplashIn  = 300;   // 闪屏淡入
constexpr int SplashMax = 1500;  // 闪屏最长停留（到点淡出）
constexpr int SplashOut = 240;   // 闪屏淡出
} // namespace motion

// ---------------------------------------------------------------- 标识
namespace brand {
constexpr const wchar_t* kAppNameCn   = L"网管工具箱";
constexpr const wchar_t* kAppNameEn   = L"NetAdmin Toolbox";
constexpr const wchar_t* kWordmark    = L"biohacker.fun";  // 字标文案（金点=焊盘）
constexpr const wchar_t* kTaglineCn   = L"系统清理 · 网络诊断 · Windows 优化";
constexpr const wchar_t* kPublisher   = L"biohacker.fun";
constexpr const wchar_t* kPublisherCn = L"biohacker.fun 旗舰产品";
constexpr const wchar_t* kSiteUrl     = L"https://biohacker.fun";
constexpr const wchar_t* kSlogan      = L"Decode your biology. Optimize your life.";
} // namespace brand

// ---------------------------------------------------------------- 主题操作
const ThemeTokens* CurrentTheme();                       // 当前主题
const ThemeTokens* FindTheme(const wchar_t* id);         // 按 ID 查找（找不到返回 nullptr）
void ApplyTheme(const ThemeTokens* t);                   // 切换：刷新令牌 + 持久化 + 通知
void LoadThemeOnStartup();                               // 启动时读取持久化选择（含旧值映射）
using ThemeChangedFn = void (*)(void*);                  // 主题变更回调（重画画刷等）
void SetThemeChangedCallback(ThemeChangedFn fn, void* ctx);

// 界面缩放持久化（与主题同一个 ini，独立键）
void SaveUserScale(double scale);                  // 按百分比整数存储，规避小数点 locale
double LoadUserScale();                            // 未设置时返回 1.0

// 侧栏展开状态持久化（与缩放同一个 ini，键 ui/navExpanded）
// 值 = 分号分隔的展开组名列表；全收起时写占位符 "-"；
// 键缺失/空值 = 全展开（用户零学习成本的缺省态）。
void SaveNavExpanded(const wchar_t* csv);          // 空串自动写占位符
std::wstring LoadNavExpanded();                    // 原样返回（含占位符 "-"），缺省返回空

// ---------------------------------------------------------------- 模块输入持久化
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
