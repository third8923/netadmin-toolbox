// ============================================================================
//  主题系统实现：切换 / 持久化 / 变更通知
//  ---------------------------------------------------------------------------
//  持久化文件：exe 同目录 netadmin-toolbox-theme.ini
//      [ui]
//      theme=daylight
//  exe 放在 U 盘/只读目录时写入失败不影响运行（仅本次选择不记住）。
// ============================================================================
#include "ui/design.h"
#include "common.h"
#include <wincrypt.h>

#include <string>

namespace mz {
namespace ui {
namespace design {

namespace {

const Theme* g_Current = &Themes::Daylight;
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

// 把主题调色板写入当前生效令牌（inline 变量，全程序共享）
void RefreshTokens(const Theme* t) {
    const Palette& p = t->pal;
    color::Bg = p.bg;                   color::Surface = p.surface;
    color::SurfaceHi = p.surfaceHi;     color::SurfaceSel = p.surfaceSel;
    color::Sidebar = p.sidebar;         color::SidebarHi = p.sidebarHi;
    color::Console = p.console;         color::Border = p.border;
    color::BorderSoft = p.borderSoft;   color::Primary = p.primary;
    color::PrimaryHi = p.primaryHi;     color::PrimaryDim = p.primaryDim;
    color::PrimarySoft = p.primarySoft; color::PrimaryDisabled = p.primaryDisabled;
    color::Cyan = p.cyan;               color::CyanSoft = p.cyanSoft;
    color::Danger = p.danger;           color::DangerHi = p.dangerHi;
    color::DangerDim = p.dangerDim;     color::DangerSoft = p.dangerSoft;
    color::DangerDisabled = p.dangerDisabled;
    color::Warn = p.warn;               color::WarnSoft = p.warnSoft;
    color::Ok = p.ok;                   color::OkSoft = p.okSoft;
    color::Text = p.text;               color::TextSub = p.textSub;
    color::TextFaint = p.textFaint;     color::OnPrimary = p.onPrimary;
    color::OnDanger = p.onDanger;       color::Zebra = p.zebra;
    color::Caption = p.caption;
}

} // namespace

const Theme* CurrentTheme() { return g_Current; }

const Theme* FindTheme(const wchar_t* id) {
    if (!id || !*id) return nullptr;
    for (const Theme* t : Themes::kAll) {
        if (wcscmp(t->id, id) == 0) return t;
    }
    return nullptr;
}

void ApplyTheme(const Theme* t) {
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
    GetPrivateProfileStringW(L"ui", L"theme", L"daylight", id, 64, IniPath().c_str());
    const Theme* t = FindTheme(id);
    if (!t) t = &Themes::Daylight;
    g_Current = t;
    RefreshTokens(t);   // 启动时无回调注册，只刷新令牌
}

void SetThemeChangedCallback(ThemeChangedFn fn, void* ctx) {
    g_Callback = fn;
    g_CallbackCtx = ctx;
}

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

// ---- 侧栏展开状态持久化（v2.9 T01）----
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

// ---------------------------------------------------------------- 模块输入持久化（v2.12 R1）

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
