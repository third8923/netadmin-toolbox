#include "common.h"
#include <taskschd.h>
#include <cstdio>
#include <shellapi.h>

namespace mz {

// MinGW taskschd.h 未导出 CLSID_TaskScheduler，本地定义（标准值，与 taskmods.cpp 一致）
static const CLSID kCLSID_TaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };

// BSTR 临时包装：构造分配、析构释放（避免 _bstr_t 在 MinGW 的兼容性问题）
struct BStr {
    BSTR b = nullptr;
    explicit BStr(const wchar_t* s) : b(SysAllocString(s ? s : L"")) {}
    ~BStr() { if (b) SysFreeString(b); }
    operator BSTR() { return b; }
};

// 静默提权任务名（根文件夹下）
constexpr wchar_t kElevTaskName[] = L"NetAdminToolboxElev";

bool RunElevatedViaTask() {
    // schtasks /Run 以任务注册的 HIGHEST 令牌启动动作，不触发 UAC。
    // 任务不存在/被策略禁用/exe 已删除时返回非 0，调用方回落常规 UAC。
    int rc = RunHidden(std::wstring(L"schtasks /Run /TN \"") + kElevTaskName + L"\"");
    return rc == 0;
}

void EnsureElevationTask() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // COM 会话（RAII；已初始化其他模式时不重复释放）
    bool comUninit = false;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) comUninit = true;
    else if (hr != RPC_E_CHANGED_MODE) return;

    do {
        ITaskService* svc = nullptr;
        if (FAILED(CoCreateInstance(kCLSID_TaskScheduler, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_ITaskService,
                                    (void**)&svc)) || !svc)
            break;
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        bool ok = SUCCEEDED(svc->Connect(vEmpty, vEmpty, vEmpty, vEmpty));
        ITaskFolder* folder = nullptr;
        if (ok) ok = SUCCEEDED(svc->GetFolder(BStr(L"\\"), &folder)) && folder;
        if (!ok) { svc->Release(); break; }

        ITaskDefinition* def = nullptr;
        if (FAILED(svc->NewTask(0, &def)) || !def) {
            folder->Release(); svc->Release(); break;
        }

        // Principal：当前用户交互令牌 + HIGHEST 运行级别
        IPrincipal* prin = nullptr;
        if (SUCCEEDED(def->get_Principal(&prin)) && prin) {
            prin->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
            prin->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            prin->Release();
        }
        // Settings：允许电池供电启动；不限制执行时长
        ITaskSettings* set = nullptr;
        if (SUCCEEDED(def->get_Settings(&set)) && set) {
            set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            set->put_ExecutionTimeLimit(BStr(L"PT0S"));
            set->put_StartWhenAvailable(VARIANT_FALSE);
            set->Release();
        }
        // 触发器：一次性、2099 年 —— 永不自动触发，仅供 schtasks /Run
        ITriggerCollection* trigs = nullptr;
        if (SUCCEEDED(def->get_Triggers(&trigs)) && trigs) {
            ITrigger* trig = nullptr;
            if (SUCCEEDED(trigs->Create(TASK_TRIGGER_TIME, &trig)) && trig) {
                ITimeTrigger* tt = nullptr;
                if (SUCCEEDED(trig->QueryInterface(IID_ITimeTrigger, (void**)&tt)) && tt) {
                    tt->put_StartBoundary(BStr(L"2099-01-01T00:00:00"));
                    tt->Release();
                }
                trig->Release();
            }
            trigs->Release();
        }
        // 动作：当前 exe（GUI 模式无需参数）
        IActionCollection* acts = nullptr;
        if (SUCCEEDED(def->get_Actions(&acts)) && acts) {
            IAction* act = nullptr;
            if (SUCCEEDED(acts->Create(TASK_ACTION_EXEC, &act)) && act) {
                IExecAction* ex = nullptr;
                if (SUCCEEDED(act->QueryInterface(IID_IExecAction, (void**)&ex)) && ex) {
                    ex->put_Path(BStr(exe));
                    ex->Release();
                }
                act->Release();
            }
            acts->Release();
        }

        // 注册：TASK_CREATE_OR_UPDATE 覆盖旧任务（exe 挪位置后自愈）。
        // MinGW taskschd.h 的 ITaskService 缺 RegisterTaskDefinition，
        // 走 ITaskFolder 同名方法（根文件夹等价）。
        VARIANT vSddl;
        VariantInit(&vSddl);
        IRegisteredTask* reg = nullptr;
        folder->RegisterTaskDefinition(BStr(kElevTaskName), def,
                                       TASK_CREATE_OR_UPDATE, vEmpty, vEmpty,
                                       TASK_LOGON_INTERACTIVE_TOKEN, vSddl, &reg);
        if (reg) reg->Release();
        def->Release();
        folder->Release();
        svc->Release();
    } while (false);

    if (comUninit) CoUninitialize();
}

wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WToUtf8(const wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes < 1024ULL) {
        swprintf_s(buf, L"%llu B", bytes);
    } else if (bytes < 1024ULL * 1024) {
        swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        swprintf_s(buf, L"%.2f MB", bytes / (1024.0 * 1024));
    } else {
        swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

bool IsElevated() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin != FALSE;
}

bool ElevationAttempted() {
    wchar_t buf[8] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"MZ_ELEV_ATTEMPTED", buf, 8);
    return n > 0 && buf[0] != L'\0';
}

ElevateResult RelaunchAsAdmin(const wstring& params, const wstring& workDir) {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    // 防提权死循环主保险：先置环境变量再启动。子进程继承父进程环境块，
    // 启动后 ElevationAttempted() 命中即跳过自动提权（无具名对象泄漏）。
    SetEnvironmentVariableW(L"MZ_ELEV_ATTEMPTED", L"1");

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) CloseHandle(sei.hProcess);
        return ElevateResult::Ok;
    }
    // 1223 = ERROR_CANCELLED：用户在 UAC 弹窗点了「否」
    if (GetLastError() == ERROR_CANCELLED) return ElevateResult::Cancelled;
    return ElevateResult::Failed;
}

int RunHidden(const wstring& cmdline, wstring* outText) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi = {};
    wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return -1;
    }

    // 读取输出（std::wstring 版需自行转码）
    std::string raw;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
        raw.append(buf, read);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (outText) {
        // 命令行工具输出通常是 OEM/ANSI；先按 ANSI 转到宽字符
        int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        wstring w(n, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), w.data(), n);
        *outText = w;
    }
    return (int)code;
}

void RestartExplorer() {
    RunHidden(L"taskkill /f /im explorer.exe");
    Sleep(600);
    // 直接启动 explorer.exe
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"explorer.exe";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

} // namespace mz
