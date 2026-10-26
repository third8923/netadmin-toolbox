#include "dpi.h"

#include <shellscalingapi.h>

namespace mz {
namespace ui {
namespace dpi {

namespace {

UINT g_dpi = 96;   // UI 线程专属缓存，布局与绘制都在 UI 线程
double g_scale = 1.0;   // 用户界面缩放倍率（0.85 ~ 1.80）

using FnSetCtx = BOOL (WINAPI*)(HANDLE);
using FnForWindow = UINT (WINAPI*)(HWND);
using FnForSystem = UINT (WINAPI*)();

FnForWindow pGetDpiForWindow = nullptr;
FnForSystem pGetDpiForSystem = nullptr;

void ResolveApis() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (user) {
        pGetDpiForWindow = reinterpret_cast<FnForWindow>(
            GetProcAddress(user, "GetDpiForWindow"));
        pGetDpiForSystem = reinterpret_cast<FnForSystem>(
            GetProcAddress(user, "GetDpiForSystem"));
    }
}

} // namespace

void EnableAwareness() {
    ResolveApis();
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (user) {
        auto setCtx = reinterpret_cast<FnSetCtx>(
            GetProcAddress(user, "SetProcessDpiAwarenessContext"));
        if (setCtx) {
            // Per-Monitor-V2 → V1 → System → 放弃（老系统保持默认）
            const HANDLE kCtxV2 = reinterpret_cast<HANDLE>(-4);  // PER_MONITOR_DPI_AWARE_V2
            const HANDLE kCtxV1 = reinterpret_cast<HANDLE>(-3);  // PER_MONITOR_DPI_AWARE
            const HANDLE kCtxSys = reinterpret_cast<HANDLE>(-2); // SYSTEM_DPI_AWARE
            if (setCtx(kCtxV2)) return;
            if (setCtx(kCtxV1)) return;
            if (setCtx(kCtxSys)) return;
        }
    }
    // 最后的兜底：shcore（Win8.1）
    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (shcore) {
        using FnSetAwareness = HRESULT (WINAPI*)(int);
        auto setAwareness = reinterpret_cast<FnSetAwareness>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setAwareness) setAwareness(2);  // PROCESS_PER_MONITOR_DPI_AWARE
    }
}

UINT ForWindow(HWND hwnd) {
    ResolveApis();
    if (hwnd && pGetDpiForWindow) {
        UINT d = pGetDpiForWindow(hwnd);
        if (d) { g_dpi = d; return d; }
    }
    if (pGetDpiForSystem) {
        UINT d = pGetDpiForSystem();
        if (d) { g_dpi = d; return d; }
    }
    return g_dpi;
}

void SetCurrent(UINT d) {
    if (d >= 72 && d <= 480) g_dpi = d;
}

void SetUserScale(double s) {
    if (s < 0.85) s = 0.85;
    if (s > 1.80) s = 1.80;
    g_scale = s;
}

double UserScale() { return g_scale; }

void StepUserScale(int dir) {
    // 档位表：0.85 0.95 1.00 1.10 1.20 1.35 1.50 1.65 1.80
    static const double kSteps[] = { 0.85, 0.95, 1.00, 1.10, 1.20,
                                     1.35, 1.50, 1.65, 1.80 };
    const int n = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    int cur = 2;   // 默认 1.00 的索引
    double best = 1e9;
    for (int i = 0; i < n; ++i) {
        double d = kSteps[i] - g_scale;
        if (d < 0) d = -d;
        if (d < best) { best = d; cur = i; }
    }
    int next = cur + (dir > 0 ? 1 : -1);
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    g_scale = kSteps[next];
}

void ResetUserScale() { g_scale = 1.0; }

int S(int px) {
    // 定点运算避免浮点累积误差：px × dpi/96 × scale（scale 用千分位整数表示）
    const long long sc = static_cast<long long>(g_scale * 1000.0 + 0.5);
    return static_cast<int>((static_cast<long long>(px) * g_dpi * sc) / (96LL * 1000LL));
}

int S(double px) {
    return static_cast<int>(px * g_dpi * g_scale / 96.0 + 0.5);
}

} // namespace dpi
} // namespace ui
} // namespace mz
