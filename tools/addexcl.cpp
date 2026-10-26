// 一次性开发辅助：UAC 提权执行 Add-MpPreference 添加 Defender 排除目录
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t cmd[1024];
    swprintf_s(cmd, 1024,
        L"-NoProfile -Command \"Add-MpPreference -ExclusionPath 'D:\\WorkBuddyAI\\netadmin-toolbox';"
        L" Add-MpPreference -ExclusionPath $env:TEMP;"
        L" Add-MpPreference -ExclusionProcess 'netadmin-toolbox.exe'\"");
    wchar_t ps[MAX_PATH];
    GetSystemDirectoryW(ps, MAX_PATH);
    lstrcatW(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = ps;
    sei.lpParameters = cmd;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) return 1;
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}
