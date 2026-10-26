// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) — biohacker.fun 旗舰产品
//  系统清理 / 网络诊断 / Windows 优化 / 系统信息 / 软件管家
//  许可: MIT
// ============================================================================
#include "common.h"
#include "cli.h"
#include "ui/appwindow.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return mz::RunAppWindow(hInst, nCmdShow);

    // ---- 防提权死循环（机制 B 兜底）：在 CLI/GUI 分流之前剥离内部标记 ----
    // 提权重启的子进程命令行带 --elev-attempted；若不在此处剥离，子进程会因
    // argc>=2 被误判为 CLI 模式。机制 A（主保险）在 RelaunchAsAdmin 内部：
    // 先置环境变量 MZ_ELEV_ATTEMPTED 再启动，子进程继承环境块后
    // ElevationAttempted() 直接命中，参数根本不会被传入。
    bool elevAttempted = mz::ElevationAttempted();
    vector<wchar_t*> rest;              // 剥离标记后的剩余参数（含 argv[0]）
    rest.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--elev-attempted") == 0) { elevAttempted = true; continue; }
        rest.push_back(argv[i]);
    }
    int restArgc = (int)rest.size();
    bool guiMode = (restArgc < 2) ||
                   wcscmp(rest[1], L"--gui") == 0 || wcscmp(rest[1], L"/gui") == 0;

    if (!guiMode) {
        // ---- CLI 分支：永不自动提权（脚本自动化场景不能弹 UAC）----
        wstring a1 = rest[1];
        if (a1 != L"--gui" && a1 != L"/gui") {
            // GUI 子系统程序默认没有控制台。若 stdout 已被重定向（管道/文件），
            // 直接使用；否则尝试附着到父控制台。
            bool hasStdout = (GetStdHandle(STD_OUTPUT_HANDLE) != nullptr);
            DWORD ft = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
            if (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE) {
                hasStdout = true;
            } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* f = nullptr;
                freopen_s(&f, "CONOUT$", "w", stdout);
                freopen_s(&f, "CONOUT$", "w", stderr);
                hasStdout = true;
            }
            (void)hasStdout;

            int rc = mz::RunCli(restArgc, rest.data());
            fflush(stdout);
            LocalFree(argv);
            return rc;
        }
    }
    LocalFree(argv);

    // ---- GUI 分支：标准用户启动时自动请求提权（v2.8 R1 / v2.12 R2）----
    // v2.12 静默通道：提权实例启动时已注册 RunLevel=HIGHEST 的计划任务
    // （触发器停在 2099 年，永不自动触发），标准用户启动直接 schtasks /Run
    // 拉起管理员实例——运行计划任务不弹 UAC，实现「默认无感拿到管理员权限」。
    // 任务不存在（首次运行/策略删除/Run 失败）时回落常规 UAC 流程；
    // UAC 取消/失败时仍静默降级为标准用户运行。
    if (!mz::IsElevated() && !elevAttempted) {
        if (mz::RunElevatedViaTask()) return 0;   // 静默：管理员实例由任务拉起
        // 幂等兜底（主保险在 RelaunchAsAdmin 内部，先置环境变量再启动）
        SetEnvironmentVariableW(L"MZ_ELEV_ATTEMPTED", L"1");
        wchar_t cwd[MAX_PATH] = { 0 };
        GetCurrentDirectoryW(MAX_PATH, cwd);
        // 内部标记 + 前向 --gui 之外的用户参数（当前产品 CLI 参数与 GUI 不共享，
        // 接口先行：引号包裹，防参数含空格）
        wstring params = L"--elev-attempted";
        for (int i = 2; i < restArgc; ++i) {
            params += L" \"";
            params += rest[i];
            params += L"\"";
        }
        if (mz::RelaunchAsAdmin(params, cwd) == mz::ElevateResult::Ok)
            return 0;   // UAC 同意：当前标准用户实例退出，管理员实例接管
        // Cancelled / Failed：继续标准用户运行（静默降级）
    } else if (mz::IsElevated()) {
        // 提权实例每次启动重建任务（exe 挪位置后自愈；失败不影响运行）
        mz::EnsureElevationTask();
    }

    return mz::RunAppWindow(hInst, nCmdShow);
}
