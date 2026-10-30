// ============================================================================
//  NetAdmin Toolbox — 网管工具箱
//  系统清理 · 网络诊断 · Windows 优化
//  本文件: 公共类型、工具函数
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ---- 统一字符串 ----
using std::wstring;
using std::vector;

namespace mz {

// UTF-8 <-> UTF-16
wstring Utf8ToW(const std::string& s);
std::string WToUtf8(const wstring& s);

// 大小格式化
wstring FormatSize(unsigned long long bytes);

// 日志行（同时回调给 UI）
struct LogSink {
    std::function<void(const wstring&)> onLine;
    void Line(const wstring& s) const { if (onLine) onLine(s); }
};

// 进度回调：0..100
using ProgressFn = std::function<void(int percent, const wstring& status)>;
// 取消检查
using CancelFn = std::function<bool()>;

constexpr int k_AppVersionMajor = 2;
constexpr int k_AppVersionMinor = 18;

// 是否以管理员权限运行
bool IsElevated();

// ---- 启动自动提权（v2.8 R1）----
// 提权重启结果：UAC 同意 / 取消 / 其它失败
enum class ElevateResult { Ok, Cancelled, Failed };

// 是否已尝试过提权（环境变量 MZ_ELEV_ATTEMPTED，由 RelaunchAsAdmin 先置再启动，
// 子进程继承环境块 → 防提权死循环的主保险）
bool ElevationAttempted();

// 以管理员权限重启自身；params 传给新实例的命令行参数、workDir 为工作目录。
// 返回 Ok / Cancelled（用户在 UAC 取消）/ Failed。
ElevateResult RelaunchAsAdmin(const wstring& params = L"",
                              const wstring& workDir = L"");

// ---- 计划任务静默提权（v2.12 R2）----
// 原理：提权成功的实例用 ITaskService 注册一个 RunLevel=HIGHEST、
// 触发器停在 2099 年（永不自动触发）的任务，动作指向当前 exe。
// 之后非提权启动直接 schtasks /Run 拉起管理员实例——运行计划任务不弹 UAC。
// 任务每次由提权实例启动时重建（exe 挪位置后自愈）。
bool RunElevatedViaTask();     // 非提权实例调用：任务存在则静默拉起，返回是否成功
void EnsureElevationTask();    // 提权实例调用：注册/重建静默提权任务

// 执行外部命令（隐藏窗口），返回退出码
int RunHidden(const wstring& cmdline, wstring* outText = nullptr);

// 刷新/重启资源管理器
void RestartExplorer();

} // namespace mz
