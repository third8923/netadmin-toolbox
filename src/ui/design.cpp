// ============================================================================
//  设计系统 v2.17「VI-D2 蜕变之翼」实现：双主题实例 / 令牌刷新 / 持久化 / fontsys
//  ---------------------------------------------------------------------------
//  本文件是全仓唯一允许出现 VI 身份色 RGB 原值的地方（brand.cpp 只引用令牌）。
//  令牌值与 docs/design-ui-vi.md §1.7 映射表逐字段一致；
//  暗主题派生色推导见 §1.6（tools/qa_contrast.py 批量自检）。
//
//  持久化文件：exe 同目录 netadmin-toolbox-theme.ini
//      [ui]
//      theme=paper          （旧值兼容：daylight|warmpaper|mistblue→paper，
//                             deepspace→ink，未知→paper）
//  exe 放在 U 盘/只读目录时写入失败不影响运行（仅本次选择不记住）。
//
//  字体许可：Baloo 2 / JetBrains Mono — SIL Open Font License 1.1,
//  embedded as subset under OFL-labeled embedding terms（全文见
//  src/res/fonts/OFL.txt）。
// ============================================================================
#include "ui/design.h"
#include "common.h"
#include <wincrypt.h>

#include <string>
#include <vector>

namespace mz {
namespace ui {
namespace design {

// ---------------------------------------------------------------- 色值工具
// constexpr：内部链接，无需匿名命名空间
constexpr COLORREF C(unsigned int rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// ---------------------------------------------------------------- 双主题实例
//  亮：纸亮（默认）。手册四主色原值 + 中性派生原值。
const ThemeTokens kPaperLight{
    // ---- 身份层（手册原值）----
    /*Paper*/      C(0xFBF7EC), /*Ink*/   C(0x3A3A44),
    /*Wing*/       C(0x7C5CBF), /*Signal*/ C(0xF2B441),
    /*SignalText*/ C(0x9F6500), /*WingOnInk*/ C(0xB293FC),
    /*ChIn*/       C(0x2F7D6B), /*ChOut*/ C(0x9F6500), /*ChErr*/ C(0xB3453B),
    // ---- 中性派生（手册原值）----
    /*Panel*/  C(0xEFECE2), /*Panel2*/ C(0xE4E0D8), /*Line*/ C(0xD4D1CA),
    /*Ink70*/  C(0x646469), /*Ink55*/  C(0x706F73),
    // ---- 墨底变体（亮主题下冗余同值，仅供跨主题取值方使用）----
    /*PanelD*/  C(0x42424C), /*Panel2D*/ C(0x484852), /*LineD*/ C(0x5A5A64),
    /*Ink70D*/  C(0xC6C3CC), /*Ink55D*/  C(0xACA9B2),
    /*ChInD*/   C(0x57C2A8), /*ChOutD*/  C(0xE8A344), /*ChErrD*/ C(0xF28B7E),
    /*ChErrSoftD*/ C(0x463338),
    /*ChErrSoft*/  C(0xF8EAE7),   // 危险行柔底（ch_err 文字 4.68:1 达标）
    // ---- 区域角色 ----
    /*Canvas*/        C(0xFBF7EC),        // 主区/卡片/日志底 = paper
    /*Side*/          C(0xEFECE2),        // 侧栏 = panel
    /*Hover*/         C(0xE4E0D8),        // 悬停面/表头/进度轨 = panel2
    /*Text*/          C(0x3A3A44),        // ink
    /*Text2*/         C(0x646469),        // ink70（on 纸 5.50）
    /*Text3*/         C(0x706F73),        // ink55（on 纸 4.66）
    /*Accent*/        C(0x7C5CBF),        // wing（on 纸 4.74）
    /*OnAccent*/      C(0xFBF7EC),        // paper（wing 上字）
    /*AccentHover*/   C(0x6B4EA8),        // 加深保证纸字 5.98 达标
    /*AccentPressed*/ C(0x5C4290),
    /*AccentSoft*/    C(0xE9E2F6),        // 选中柔光底（ink 文字 8.93）
    /*BtnFill*/       C(0x3A3A44),        // 主按钮 = 墨底反白
    /*BtnText*/       C(0xFBF7EC),
    /*Gold*/          C(0xF2B441),        // 信号：进度/焊盘/金点
    /*GoldText*/      C(0x9F6500),        // 底上金色小字（on 纸 4.52）
    /*Hairline*/      C(0xD4D1CA),        // 行分隔 = line
    /*Zebra*/         C(0xF3F0E6),        // 斑马（panel 微淡）
    /*Cap*/           C(0xEFECE2),        // DWM 标题栏
    /*CapText*/       C(0x3A3A44),
    /*Dark*/ false,
    /*id*/   L"paper",
    /*name*/ L"纸亮",
    /*desc*/ L"纸底 + 墨字 + 藤紫 + 金信号",
};

// 暗：墨暗。墨底 + 纸字 + 金信号（墨底唯一允许彩色）+ 提亮紫 #B293FC。
// 派生值推导：docs/design-ui-vi.md §1.6（相对亮度法，全部 ≥4.5 达标）。
const ThemeTokens kInkDark{
    // ---- 身份层（两主题同值）----
    /*Paper*/      C(0xFBF7EC), /*Ink*/   C(0x3A3A44),
    /*Wing*/       C(0x7C5CBF), /*Signal*/ C(0xF2B441),
    /*SignalText*/ C(0x9F6500), /*WingOnInk*/ C(0xB293FC),
    /*ChIn*/       C(0x2F7D6B), /*ChOut*/ C(0x9F6500), /*ChErr*/ C(0xB3453B),
    // ---- 中性派生（暗=派生值）----
    /*Panel*/  C(0x42424C), /*Panel2*/ C(0x484852), /*Line*/ C(0x5A5A64),
    /*Ink70*/  C(0xC6C3CC), /*Ink55*/  C(0xACA9B2),
    // ---- 墨底变体 ----
    /*PanelD*/  C(0x42424C), /*Panel2D*/ C(0x484852), /*LineD*/ C(0x5A5A64),
    /*Ink70D*/  C(0xC6C3CC), /*Ink55D*/  C(0xACA9B2),
    /*ChInD*/   C(0x57C2A8), /*ChOutD*/  C(0xE8A344), /*ChErrD*/ C(0xF28B7E),
    /*ChErrSoftD*/ C(0x463338),
    /*ChErrSoft*/  C(0x463338),          // 暗主题危险行柔底（ch_err_dark 5.45 达标）
    // ---- 区域角色 ----
    /*Canvas*/        C(0x3A3A44),        // 墨底
    /*Side*/          C(0x42424C),        // panel_dark（paper 文字 9.27）
    /*Hover*/         C(0x484852),        // panel2_dark（paper 文字 8.44）
    /*Text*/          C(0xFBF7EC),        // paper 反白（10.50）
    /*Text2*/         C(0xC6C3CC),        // ink70_dark（6.47）
    /*Text3*/         C(0xACA9B2),        // ink55_dark（4.86）
    /*Accent*/        C(0xB293FC),        // wing_on_ink（藤紫原色禁止上墨底）
    /*OnAccent*/      C(0x3A3A44),        // ink（accent 上字 5.65+）
    /*AccentHover*/   C(0xC3ABFD),        // 悬停提亮（ink 文字 5.65）
    /*AccentPressed*/ C(0x9F80E8),
    /*AccentSoft*/    C(0x4A3F63),        // 选中柔光底（paper 文字 8.99）
    /*BtnFill*/       C(0xFBF7EC),        // 主按钮 = 纸底反白
    /*BtnText*/       C(0x3A3A44),
    /*Gold*/          C(0xF2B441),        // 墨底唯一彩色（6.09）
    /*GoldText*/      C(0xF2B441),        // 墨底金小字直接用 signal（6.09）
    /*Hairline*/      C(0x5A5A64),        // line_dark
    /*Zebra*/         C(0x42424C),        // = panel_dark
    /*Cap*/           C(0x3A3A44),
    /*CapText*/       C(0xFBF7EC),
    /*Dark*/ true,
    /*id*/   L"ink",
    /*name*/ L"墨暗",
    /*desc*/ L"墨底 + 纸字 + 金信号 + 提亮紫",
};

namespace {

const ThemeTokens* g_Current = &kPaperLight;
ThemeChangedFn g_Callback = nullptr;
void* g_CallbackCtx = nullptr;

// exe 同目录的 ini 路径：<exe 无扩展名>-theme.ini
std::wstring IniPath() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t dot = p.find_last_of(L'.');
    size_t sep = p.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep))
        p.resize(dot);
    p += L"-theme.ini";
    return p;
}

// 旧主题值映射：daylight|warmpaper|mistblue → paper，deepspace → ink
const wchar_t* MapLegacyTheme(const wchar_t* id) {
    if (!id || !*id) return L"paper";
    if (wcscmp(id, L"daylight") == 0 || wcscmp(id, L"warmpaper") == 0 ||
        wcscmp(id, L"mistblue") == 0)
        return L"paper";
    if (wcscmp(id, L"deepspace") == 0) return L"ink";
    return id;   // paper / ink 原样；未知值由 FindTheme 回落 paper
}

// 把主题令牌写入当前生效值（inline 变量，全程序共享）
void RefreshTokens(const ThemeTokens* t) {
    color::Paper = t->Paper;             color::Ink = t->Ink;
    color::Wing = t->Wing;               color::Signal = t->Signal;
    color::SignalText = t->SignalText;   color::WingOnInk = t->WingOnInk;
    color::Panel = t->Panel;             color::Panel2 = t->Panel2;
    color::Line = t->Line;               color::Ink70 = t->Ink70;
    color::Ink55 = t->Ink55;
    color::ChIn = t->Dark ? t->ChInD : t->ChIn;       // 数据通道主题化
    color::ChOut = t->Dark ? t->ChOutD : t->ChOut;
    color::ChErr = t->Dark ? t->ChErrD : t->ChErr;
    color::ChErrSoft = t->Dark ? t->ChErrSoftD : t->ChErrSoft;
    color::PanelD = t->PanelD;           color::Panel2D = t->Panel2D;
    color::LineD = t->LineD;             color::Ink70D = t->Ink70D;
    color::Ink55D = t->Ink55D;
    color::ChInD = t->ChInD;             color::ChOutD = t->ChOutD;
    color::ChErrD = t->ChErrD;           color::ChErrSoftD = t->ChErrSoftD;
    color::Canvas = t->Canvas;           color::Side = t->Side;
    color::Hover = t->Hover;
    color::Text = t->Text;               color::Text2 = t->Text2;
    color::Text3 = t->Text3;
    color::Accent = t->Accent;           color::OnAccent = t->OnAccent;
    color::AccentHover = t->AccentHover; color::AccentPressed = t->AccentPressed;
    color::AccentSoft = t->AccentSoft;
    color::BtnFill = t->BtnFill;         color::BtnText = t->BtnText;
    color::Gold = t->Gold;               color::GoldText = t->GoldText;
    color::Hairline = t->Hairline;       color::Zebra = t->Zebra;
    color::Cap = t->Cap;                 color::CapText = t->CapText;
}

} // namespace

const ThemeTokens* CurrentTheme() { return g_Current; }

const ThemeTokens* FindTheme(const wchar_t* id) {
    if (!id || !*id) return nullptr;
    for (const ThemeTokens* t : kAll) {
        if (wcscmp(t->id, id) == 0) return t;
    }
    return nullptr;
}

void ApplyTheme(const ThemeTokens* t) {
    if (!t) return;
    g_Current = t;
    RefreshTokens(t);

    // 持久化（只读目录失败可忽略）
    WritePrivateProfileStringW(L"ui", L"theme", t->id, IniPath().c_str());

    // 通知窗口层重建缓存资源（画刷 / chrome）
    if (g_Callback) g_Callback(g_CallbackCtx);
}

void LoadThemeOnStartup() {
    wchar_t id[64] = { 0 };
    GetPrivateProfileStringW(L"ui", L"theme", L"paper", id, 64, IniPath().c_str());
    const ThemeTokens* t = FindTheme(MapLegacyTheme(id));
    if (!t) t = &kPaperLight;
    g_Current = t;
    RefreshTokens(t);   // 启动时无回调注册，只刷新令牌
}

void SetThemeChangedCallback(ThemeChangedFn fn, void* ctx) {
    g_Callback = fn;
    g_CallbackCtx = ctx;
}

// ---------------------------------------------------------------- fontsys
//  内嵌字体子集（RCDATA 资源）→ AddFontMemResourceEx 内存加载。
//  资源名与 app.rc 中三条 RCDATA 一一对应：
//      FONT_BALOO2 / FONT_MONO / FONT_MONO_SB
//  失败静默降级：HasDisplayFont()/HasMonoFont() 返回 false，调用方回落
//  雅黑/系统等宽，功能不受影响（品牌保真降级但不破版）。

namespace fontsys {

namespace {
bool g_display = false;   // Baloo 2 注册成功
bool g_mono = false;      // JetBrains Mono Regular 注册成功

bool LoadFontResource(const wchar_t* resName) {
    HRSRC hr = FindResourceW(nullptr, resName, (LPCWSTR)RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    const void* data = LockResource(hg);
    DWORD size = SizeofResource(nullptr, hr);
    if (!data || size == 0) return false;
    // 句柄由系统持有（文档约定无需 RemoveFontMemResourceEx 之外的释放，
    // 进程退出即回收）；nFonts>0 即注册成功。
    ULONG nFonts = 0;
    HANDLE h = AddFontMemResourceEx(const_cast<void*>(data), size, nullptr, &nFonts);
    return h != nullptr && nFonts > 0;
}
} // namespace

// 探测系统是否安装了指定字族（EnumFontFamiliesExW 精确匹配）
bool FamilyAvailable(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    struct Ctx { const wchar_t* want; bool hit; } ctx = { face, false };
    EnumFontFamiliesExW(dc, &lf,
                        reinterpret_cast<FONTENUMPROCW>(
                            +[](const LOGFONTW* plf, const TEXTMETRICW*, DWORD,
                                LPARAM lp) -> int {
                                auto* c = reinterpret_cast<Ctx*>(lp);
                                if (plf && c->want &&
                                    wcscmp(plf->lfFaceName, c->want) == 0) {
                                    c->hit = true;
                                    return 0;   // 命中即停
                                }
                                return 1;
                            }),
                        reinterpret_cast<LPARAM>(&ctx), 0);
    ReleaseDC(nullptr, dc);
    return ctx.hit;
}

bool LoadEmbeddedFonts() {
    g_display = LoadFontResource(L"FONT_BALOO2");
    g_mono = LoadFontResource(L"FONT_MONO") && LoadFontResource(L"FONT_MONO_SB");
    return g_display || g_mono;
}

bool HasDisplayFont() { return g_display; }
bool HasMonoFont() { return g_mono; }

HFONT DataFont(int px, bool semibold) {
    // 回落链：JetBrains Mono（内嵌子集）→ Cascadia Mono（Win10 2004+）→ Consolas
    const wchar_t* face = L"Consolas";
    if (g_mono)                 face = L"JetBrains Mono";
    else if (FamilyAvailable(L"Cascadia Mono")) face = L"Cascadia Mono";
    return CreateFontW(px, 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL,
                       0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
}

} // namespace fontsys

// ---- 界面缩放持久化 ----
// 用「百分比整数」存盘（120 而不是 1.20）：GetPrivateProfileIntW 不受
// 区域设置的小数点差异影响，U 盘跨机器复制也不会读出 1,2 之类的怪值。
void SaveUserScale(double scale) {
    wchar_t buf[16];
    wsprintfW(buf, L"%d", (int)(scale * 100.0 + 0.5));
    WritePrivateProfileStringW(L"ui", L"scale", buf, IniPath().c_str());
}

double LoadUserScale() {
    int v = (int)GetPrivateProfileIntW(L"ui", L"scale", 100, IniPath().c_str());
    if (v < 85) v = 85;
    if (v > 180) v = 180;
    return v / 100.0;
}

// ---- 侧栏展开状态持久化 ----
// 键 ui/navExpanded：展开组名以分号分隔；全收起写占位符 "-"
// （空值已被「全展开」缺省语义占用，需要占位符区分两种状态）。
// 写入后用全空参调用立即刷盘，防止进程被强杀时丢配置。
void SaveNavExpanded(const wchar_t* csv) {
    const wchar_t* v = (csv && *csv) ? csv : L"-";
    WritePrivateProfileStringW(L"ui", L"navExpanded", v, IniPath().c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, IniPath().c_str());  // 立即落盘
}

std::wstring LoadNavExpanded() {
    wchar_t buf[1024] = { 0 };
    GetPrivateProfileStringW(L"ui", L"navExpanded", L"", buf, 1024, IniPath().c_str());
    return buf;
}

// ---------------------------------------------------------------- 模块输入持久化

namespace {

// DPAPI 加密（绑定当前用户）+ Base64 包装，输出形如 "dpapi:<base64>"。
// 密码类扩展字段绝不能以明文落在 ini 里（工具箱常被拷到共享目录）。
std::wstring ProtectSecret(const std::wstring& plain) {
    if (plain.empty()) return plain;
    DATA_BLOB in, out;
    in.pbData = (BYTE*)plain.data();
    in.cbData = (DWORD)(plain.size() * sizeof(wchar_t));
    if (!CryptProtectData(&in, L"NetAdminToolbox", nullptr, nullptr, nullptr,
                          0, &out))
        return plain;   // 加密失败宁可不存：返回原文由调用方跳过
    std::string b64;
    DWORD need = 0;
    if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64,
                             nullptr, &need) && need > 0) {
        std::wstring w((size_t)need, L'\0');
        if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64,
                                 &w[0], &need))
            b64 = WToUtf8(w);
    }
    LocalFree(out.pbData);
    // Base64 换行会破坏 ini 值：压成一行
    std::string flat;
    for (char c : b64)
        if (c != '\r' && c != '\n') flat += c;
    if (flat.empty()) return plain;
    return L"dpapi:" + Utf8ToW(flat);
}

bool UnprotectSecret(const std::wstring& stored, std::wstring& plain) {
    if (stored.rfind(L"dpapi:", 0) != 0) { plain = stored; return true; }
    std::wstring b64w = Utf8ToW(WToUtf8(stored.substr(6)));   // 归一
    DWORD need = 0;
    if (!CryptStringToBinaryW(b64w.c_str(), (DWORD)b64w.size(),
                              CRYPT_STRING_BASE64, nullptr, &need,
                              nullptr, nullptr) || need == 0)
        return false;
    std::vector<BYTE> blob(need);
    if (!CryptStringToBinaryW(b64w.c_str(), (DWORD)b64w.size(),
                              CRYPT_STRING_BASE64, blob.data(), &need,
                              nullptr, nullptr))
        return false;
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = need;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return false;
    plain.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    LocalFree(out.pbData);
    return true;
}

} // namespace

void SaveModuleInput(const wchar_t* id, const ModuleInputSnapshot& s) {
    if (!id || !*id) return;
    const std::wstring ini = IniPath();
    const std::wstring sec = std::wstring(L"input:") + id;
    WritePrivateProfileStringW(sec.c_str(), L"path", s.path.c_str(), ini.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"target", s.target.c_str(), ini.c_str());
    auto sv = [&](int i, const std::wstring& v, bool secret) {
        std::wstring key = L"f" + std::to_wstring(i);
        if (secret && !v.empty()) {
            std::wstring enc = ProtectSecret(v);
            if (enc.rfind(L"dpapi:", 0) != 0) return;   // 加密失败：不落盘明文
            WritePrivateProfileStringW(sec.c_str(), key.c_str(), enc.c_str(), ini.c_str());
        } else {
            WritePrivateProfileStringW(sec.c_str(), key.c_str(), v.c_str(), ini.c_str());
        }
    };
    for (size_t i = 0; i < s.fields.size() && i < 8; ++i) {
        bool secret = (i < s.secret.size()) && s.secret[i];
        sv((int)i, s.fields[i], secret);
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());  // 立即落盘
}

bool LoadModuleInput(const wchar_t* id, ModuleInputSnapshot& out) {
    if (!id || !*id) return false;
    const std::wstring ini = IniPath();
    const std::wstring sec = std::wstring(L"input:") + id;
    wchar_t buf[2048] = { 0 };
    bool any = false;
    auto rd = [&](const wchar_t* key) {
        GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 2048, ini.c_str());
        std::wstring v = buf;
        if (!v.empty()) any = true;
        return v;
    };
    out.path = rd(L"path");
    out.target = rd(L"target");
    out.fields.clear();
    out.secret.clear();
    for (int i = 0; i < 8; ++i) {
        std::wstring key = L"f" + std::to_wstring(i);
        std::wstring v = rd(key.c_str());
        std::wstring plain;
        if (v.rfind(L"dpapi:", 0) == 0) {
            if (UnprotectSecret(v, plain)) v = plain;
            else v.clear();                        // 解密失败（换用户等）：置空
        }
        out.fields.push_back(v);
        out.secret.push_back(false);
        if (!v.empty()) any = true;
    }
    return any;
}

} // namespace design
} // namespace ui
} // namespace mz
