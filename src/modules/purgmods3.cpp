// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 净化中心模块集三期（v2.15 需求 1）
//  ---------------------------------------------------------------------------
//  purg.wulock  系统更新锁定（Tweaks + Actions 组合，参照 purg.scan 范式）：
//    扫描：①组策略注册表（WindowsUpdate 与 AU 子键全部封锁值）
//          ②服务 wuauserv/UsoSvc/WaaSMedicSvc/DoSvc（OpenSCManager/
//            QueryServiceConfig/QueryServiceStatus，不用 sc.exe——沙箱黑名单）
//          ③计划任务 \Microsoft\Windows\WindowsUpdate\* 与
//            \Microsoft\Windows\UpdateOrchestrator\*（taskschd COM；
//            MinGW get_Item 坑：Task/FolderCollection 用 VARIANT 下标，
//            见 taskmods.cpp 范式）
//    锁定：策略五值写 1 → 服务禁用（ChangeServiceConfig，被 ACL 拒绝
//          时直写注册表 Start=4 并把 FailureActions 的 01 重启动作改 00
//          防 WaaSMedicSvc 自愈）→ 计划任务全禁 → 可选 hosts 域名拦截块
//    还原：按备份 TSV 反向恢复（策略写回/删值、服务 Start 还原、任务启用、
//          hosts 拦截块移除）
//    备份：exe 目录 wulock-backup.tsv（purgcore EscapeField 转义）
//    主页锁定手法（SetupComplete.cmd / Default User NTUSER.DAT / 浏览器策略
//    Homepage）由 v2.14 purg.homepage 覆盖，见 docs/dev-notes-wulock.md。
//  链接：-ladvapi32 -ltaskschd -lole32 -loleaut32（build.bat 已统一追加）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"   // ErrorTextZh
#include "../purgcore.h"      // purg::EscapeField

#include <cstdio>
#include <algorithm>

#include <taskschd.h>
#include <oleauto.h>

namespace mz {
namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32s(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }
wstring U64s(unsigned long long v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%llu", v); return b; }

wstring ExeDirW() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return wstring(L".");
    wstring p(buf, n);
    size_t s = p.find_last_of(L'\\');
    return (s == wstring::npos) ? wstring(L".") : p.substr(0, s);
}

bool FileExistsW2(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

wstring HostsFilePath() {
    wchar_t wd[1024] = {};
    UINT n = GetWindowsDirectoryW(wd, 1024);
    return ((n > 0 && n < 1024) ? wstring(wd, n) : wstring(L"C:\\Windows"))
           + L"\\System32\\drivers\\etc\\hosts";
}

wstring FoldLowerW(wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

wstring HrTextZh2(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return ErrorTextZh(HRESULT_CODE(hr));
    return ErrorTextZh((DWORD)hr);
}

// ---------------------------------------------------------------- 注册表小工具

wstring RegRootName(HKEY r) {
    return (r == HKEY_LOCAL_MACHINE) ? wstring(L"HKLM") : wstring(L"HKCU");
}

// 读注册表值（扫描/备份用）：SZ 返回文本，DWORD 返回十进制；type 带回（可空）
wstring RegReadAny(HKEY root, const wstring& sub, const wstring& val, DWORD* type) {
    DWORD t = 0, sz = 0;
    if (RegGetValueW(root, sub.c_str(), val.c_str(), RRF_RT_ANY, &t, nullptr, &sz)
        != ERROR_SUCCESS) return wstring();
    if (sz == 0) { if (type) *type = t; return wstring(); }
    vector<BYTE> buf(sz);
    if (RegGetValueW(root, sub.c_str(), val.c_str(), RRF_RT_ANY, &t, buf.data(), &sz)
        != ERROR_SUCCESS) return wstring();
    if (type) *type = t;
    if (t == REG_SZ || t == REG_EXPAND_SZ) return wstring((wchar_t*)buf.data());
    if (t == REG_DWORD && sz >= 4) return U32s(*(DWORD*)buf.data());
    return L"(二进制)";
}

bool RegWriteDword(HKEY root, const wstring& sub, const wstring& val, DWORD data) {
    HKEY k = nullptr;
    LSTATUS st = RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                 nullptr, &k, nullptr);
    if (st != ERROR_SUCCESS) {
        SetLastError((DWORD)st);   // 让调用方 ErrorTextZh(GetLastError()) 归因准确
        return false;
    }
    st = RegSetValueExW(k, val.c_str(), 0, REG_DWORD,
                        (const BYTE*)&data, sizeof(DWORD));
    RegCloseKey(k);
    if (st != ERROR_SUCCESS) SetLastError((DWORD)st);
    return st == ERROR_SUCCESS;
}

bool RegDelValue2(HKEY root, const wstring& sub, const wstring& val) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegDeleteValueW(k, val.c_str());
    RegCloseKey(k);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------- 备份 TSV

// 行格式：
//   SET  \t root \t sub \t val \t type \t data(空 = 原本不存在 → 还原时删值)
//   SVC  \t 名称 \t 原Start 类型
//   TASK \t 任务路径 \t 原Enabled(1/0)
wstring WuBackupFile() { return ExeDirW() + L"\\wulock-backup.tsv"; }

void WuBackupAppend(const wstring& line) {
    bool exists = FileExistsW2(WuBackupFile());
    HANDLE h = CreateFileW(WuBackupFile().c_str(), FILE_APPEND_DATA, 0,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (!exists) WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    std::string u8 = WToUtf8(purg::EscapeField(line) + L"\r\n");
    WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
}

// 逐行取还原记录（EscapeField 内部把字段内 \t 转义为 \s，此处行内无裸 \t 冲突）
vector<wstring> WuBackupLines() {
    vector<wstring> out;
    HANDLE h = CreateFileW(WuBackupFile().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
    CloseHandle(h);
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF) raw.erase(0, 3);
    wstring text = Utf8ToW(raw);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        wstring line = (nl == wstring::npos) ? text.substr(pos)
                                             : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        // 还原转义（EscapeField 逆运算：\\ → \，\s → \t，\n → 换行）
        line = purg::UnescapeField(line);
        if (!line.empty()) out.push_back(line);
        if (nl == wstring::npos) break;
        pos = nl + 1;
    }
    return out;
}

// ---------------------------------------------------------------- 服务（SCM）

struct SvcState {
    bool found = false;        // 服务存在
    DWORD startType = 0;       // SERVICE_AUTO_START / DEMAND / DISABLED ...
    bool running = false;
};

SvcState QuerySvcState(const wstring& name) {
    SvcState s;
    // Start 类型从注册表读（非提权下 QueryServiceConfig 对部分系统服务被拒，
    // 服务注册表键标准用户可读；语义一致：HKLM\...\Services\<name>\Start）
    const wstring sub = L"SYSTEM\\CurrentControlSet\\Services\\" + name;
    DWORD t = 0, sz = sizeof(DWORD), start = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"Start", RRF_RT_REG_DWORD,
                     &t, &start, &sz) == ERROR_SUCCESS) {
        s.found = true;
        s.startType = start;
    }
    // 运行状态经 SCM（SERVICE_QUERY_STATUS 对标准用户普遍放行）
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
        if (svc) {
            SERVICE_STATUS st{};
            if (QueryServiceStatus(svc, &st)) s.running = (st.dwCurrentState == SERVICE_RUNNING);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    return s;
}

const wchar_t* StartTypeName(DWORD t) {
    switch (t) {
    case SERVICE_BOOT_START:    return L"系统启动";
    case SERVICE_SYSTEM_START:  return L"系统加载";
    case SERVICE_AUTO_START:    return L"自动";
    case SERVICE_DEMAND_START:  return L"手动";
    case SERVICE_DISABLED:      return L"已禁用";
    default:                    return L"未知";
    }
}

// 直写服务注册表 Start（ChangeServiceConfig 被 ACL 拒绝时的兜底，如 WaaSMedicSvc）
bool SvcRegWriteStart(const wstring& name, DWORD start) {
    return RegWriteDword(HKEY_LOCAL_MACHINE,
                         wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + name,
                         L"Start", start);
}

// FailureActions 自愈补丁：REG_BINARY 中偏移 ≥16 的 4 字节对齐 DWORD==1
// （SC_ACTION_RESTART）改 0（SC_ACTION_NONE），防服务被停后自动拉起。
// 仅在本服务 FailureActions 上小范围执行，命中即记日志。
bool PatchFailureActions(const wstring& name, ModuleContext& ctx) {
    const wstring sub = L"SYSTEM\\CurrentControlSet\\Services\\" + name;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                      &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = 0;
    bool patched = false;
    if (RegQueryValueExW(k, L"FailureActions", nullptr, &type, nullptr, &sz) == ERROR_SUCCESS
        && type == REG_BINARY && sz >= 20) {
        vector<BYTE> b(sz);
        if (RegQueryValueExW(k, L"FailureActions", nullptr, &type, b.data(), &sz)
            == ERROR_SUCCESS) {
            DWORD cActions = (sz >= 16) ? *(DWORD*)(b.data() + 12) : 0;
            for (DWORD a = 0; a < cActions && 16 + (a + 1) * 8 <= sz; ++a) {
                BYTE* act = b.data() + 16 + a * 8;
                // SC_ACTION 双布局兼容：任一 4 字节对齐 DWORD==1 都清零
                for (int off = 0; off < 8; off += 4) {
                    DWORD v = *(DWORD*)(act + off);
                    if (v == 1) {
                        *(DWORD*)(act + off) = 0;
                        patched = true;
                    }
                }
            }
            if (patched) {
                patched = RegSetValueExW(k, L"FailureActions", 0, REG_BINARY,
                                         b.data(), (DWORD)b.size()) == ERROR_SUCCESS;
                if (patched)
                    ctx.Log(L"  已修补 " + name + L" FailureActions（重启动作 01→00，防自愈拉起）");
            }
        }
    }
    RegCloseKey(k);
    return patched;
}

// ---------------------------------------------------------------- 计划任务 COM

// MinGW taskschd.h 未导出 CLSID_TaskScheduler，本地定义（标准值，同 taskmods.cpp）
static const CLSID kCLSID_TaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };

wstring TakeBstr2(BSTR b) {
    wstring out = b ? wstring(b, SysStringLen(b)) : wstring();
    SysFreeString(b);
    return out;
}

struct TaskComSession2 {
    ITaskService* svc = nullptr;
    bool comUninit = false;
    wstring error;

    ~TaskComSession2() {
        if (svc) svc->Release();
        if (comUninit) CoUninitialize();
    }
    bool Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) comUninit = true;
        else if (hr != RPC_E_CHANGED_MODE) {
            error = L"COM 初始化失败（错误码 " + U32s((unsigned)hr) + L"）";
            return false;
        }
        hr = CoCreateInstance(kCLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITaskService, (void**)&svc);
        if (FAILED(hr) || !svc) {
            error = L"无法创建任务计划程序 COM 实例：" + HrTextZh2(hr);
            return false;
        }
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        if (FAILED(svc->Connect(vEmpty, vEmpty, vEmpty, vEmpty))) {
            error = L"连接任务计划程序服务失败：" + HrTextZh2(hr);
            return false;
        }
        return true;
    }
};

// 枚举单个任务文件夹下全部任务：回调 (完整路径, enabled)
bool EnumTaskFolder(ITaskService* svc, const wchar_t* folderPath,
                    vector<std::pair<wstring, bool>>& out) {
    BSTR fp = SysAllocString(folderPath);
    ITaskFolder* folder = nullptr;
    bool opened = (fp && SUCCEEDED(svc->GetFolder(fp, &folder)));
    SysFreeString(fp);
    if (!opened || !folder) return false;
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
        LONG n = 0;
        tasks->get_Count(&n);
        for (LONG i = 0; i < n; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;      // MinGW 坑：TaskCollection::get_Item 必须用 VARIANT
            IRegisteredTask* t = nullptr;
            if (SUCCEEDED(tasks->get_Item(v, &t)) && t) {
                BSTR pb = nullptr;
                VARIANT_BOOL en = VARIANT_TRUE;
                t->get_Path(&pb);
                t->get_Enabled(&en);
                out.push_back({ TakeBstr2(pb), en != VARIANT_FALSE });
                t->Release();
            }
            VariantClear(&v);
        }
        tasks->Release();
    }
    folder->Release();
    return true;
}

// ================================================================ purg.wulock

const wchar_t* kWuPolicyKey  = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
const wchar_t* kWuPolicyAu   = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
const wchar_t* kWuTaskDirs[] = {
    L"\\Microsoft\\Windows\\WindowsUpdate",
    L"\\Microsoft\\Windows\\UpdateOrchestrator",
};
const wchar_t* kWuSvcs[] = { L"wuauserv", L"UsoSvc", L"WaaSMedicSvc", L"DoSvc" };
const wchar_t* kWuHostsMark1 = L"# >>> NetAdmin Toolbox WU Lock (v2.15) >>>";
const wchar_t* kWuHostsMark2 = L"# <<< NetAdmin Toolbox WU Lock <<<";

// 锁定用策略值：<值名, 是否 AU 子键>
struct WuPolicyItem { const wchar_t* val; bool au; };
const WuPolicyItem kWuPolicies[] = {
    { L"NoAutoUpdate", false },
    { L"DisableWindowsUpdateAccess", false },
    { L"DoNotConnectToWindowsUpdateInternetLocations", false },
    { L"SetDisableUXWUAccess", true },
    { L"ExcludeWUDriversInQualityUpdate", false },
};

const wchar_t* kWuHostsDomains[] = {
    L"download.windowsupdate.com",
    L"update.microsoft.com",
    L"windowsupdate.microsoft.com",
    L"wustat.windows.com",
    L"ntservicepack.microsoft.com",
    L"tlu.dl.delivery.mp.microsoft.com",
};

class PurgWuLockModule : public IModule {
public:
    const wchar_t* Id() const override { return L"purg.wulock"; }
    const wchar_t* Group() const override { return L"净化中心"; }
    const wchar_t* Title() const override { return L"系统更新锁定"; }
    const wchar_t* Description() const override {
        return L"扫描 Windows 更新封锁状态（组策略 / 服务 / 计划任务 / hosts）；"
               L"一键锁定（封装镜像级封锁：策略五值 + 四服务禁用含 WaaSMedic 自愈"
               L"对抗 + 更新计划任务全禁 + 可选 hosts 域名拦截）与还原；"
               L"主页锁定见「主页锁定」模块";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* RunLabel() const override { return L"扫描更新状态"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"hosts 域名拦截", L"锁定时同时拦截更新域名：填 yes 或 开 启用（默认关）", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fHosts_ = values[0];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"锁定系统更新", nullptr, true, 0xE72E },   // 0 → btnClean 位
            { L"还原更新锁定", nullptr, false, 0xE72C },  // 1 → btnUndo 位
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>&) const override {
        switch (actionIndex) {
        case 0:
            return L"将执行封装镜像级更新封锁：\n"
                   L"· 写入组策略封锁值（NoAutoUpdate / DisableWindowsUpdateAccess 等 5 项）\n"
                   L"· 禁用服务 wuauserv / UsoSvc / WaaSMedicSvc / DoSvc（含防自愈修补）\n"
                   L"· 禁用计划任务 \\Microsoft\\Windows\\WindowsUpdate 与 UpdateOrchestrator 全部任务\n"
                   + (HostsBlockEnabled()
                          ? wstring(L"· 向 hosts 追加更新域名拦截块（已勾选）\n")
                          : wstring()) +
                   L"\n锁定前自动备份（wulock-backup.tsv），可用「还原更新锁定」回滚。\n是否继续？";
        case 1:
            return L"将按备份记录还原全部更新封锁（策略值/服务启动类型/计划任务/hosts）。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    // ---- 扫描 ----
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        int polCnt = 0, polLocked = 0, svcCnt = 0, svcDisabled = 0, svcRun = 0;
        int taskCnt = 0, taskDisabled = 0;

        // ① 组策略
        for (const WuPolicyItem& p : kWuPolicies) {
            wstring data = RegReadAny(HKEY_LOCAL_MACHINE, p.au ? kWuPolicyAu : kWuPolicyKey,
                                      p.val, nullptr);
            bool locked = (data == L"1");
            if (!data.empty()) ++polCnt;
            if (locked) ++polLocked;
            ModuleRow r;
            r.c1 = p.val;
            r.c2 = data.empty() ? L"未设置" : data;
            r.c3 = L"组策略";
            r.c4 = (p.au ? wstring(L"HKLM\\...\\WindowsUpdate\\AU") : wstring(L"HKLM\\...\\WindowsUpdate"));
            r.danger = locked;
            out.rows.push_back(std::move(r));
        }
        // WUServer / WUStatusServer 假 WSUS 劫持列报
        for (const wchar_t* v : { L"WUServer", L"WUStatusServer" }) {
            wstring data = RegReadAny(HKEY_LOCAL_MACHINE, kWuPolicyKey, v, nullptr);
            if (data.empty()) continue;
            ++polCnt;
            ModuleRow r;
            r.c1 = v;
            r.c2 = data;
            r.c3 = L"WSUS 指向";
            r.c4 = wstring(L"HKLM\\...\\WindowsUpdate");
            r.danger = FoldLowerW(data).find(L"127.0.0.1") != wstring::npos;
            out.rows.push_back(std::move(r));
        }

        // ② 服务
        for (const wchar_t* s : kWuSvcs) {
            SvcState st = QuerySvcState(s);
            ++svcCnt;
            if (st.found && st.startType == SERVICE_DISABLED) ++svcDisabled;
            if (st.running) ++svcRun;
            ModuleRow r;
            r.c1 = s;
            r.c2 = st.found ? StartTypeName(st.startType) : L"服务不存在";
            r.c3 = L"服务";
            r.c4 = st.found ? (st.running ? wstring(L"正在运行") : wstring(L"已停止"))
                            : wstring();
            r.danger = false;
            out.rows.push_back(std::move(r));
        }

        // ③ 计划任务
        TaskComSession2 com;
        if (com.Init()) {
            for (const wchar_t* dir : kWuTaskDirs) {
                vector<std::pair<wstring, bool>> tasks;
                EnumTaskFolder(com.svc, dir, tasks);
                for (auto& t : tasks) {
                    ++taskCnt;
                    if (!t.second) ++taskDisabled;
                    ModuleRow r;
                    r.c1 = t.first;
                    r.c2 = t.second ? L"已启用" : L"已禁用";
                    r.c3 = L"计划任务";
                    r.c4 = wstring();
                    r.danger = false;
                    out.rows.push_back(std::move(r));
                }
            }
        } else {
            ctx.Log(L"计划任务枚举失败：" + com.error);
        }

        // ④ hosts 拦截块
        bool hostsLocked = HostsBlockPresent();
        ModuleRow hr;
        hr.c1 = L"hosts 更新域名拦截";
        hr.c2 = hostsLocked ? L"已启用" : L"未启用";
        hr.c3 = L"hosts";
        hr.c4 = HostsFilePath();
        hr.danger = hostsLocked;
        out.rows.push_back(std::move(hr));

        wchar_t b[256];
        swprintf_s(b, 256,
                   L"更新状态：封锁策略 %d 项 · 服务禁用 %d/%d（运行中 %d）· "
                   L"任务禁用 %d/%d · hosts 拦截 %s",
                   polLocked, svcDisabled, svcCnt, svcRun,
                   taskDisabled, taskCnt, hostsLocked ? L"开" : L"关");
        out.summary = b;
        out.hint = (polLocked || svcDisabled || taskDisabled)
            ? T(L"检测到更新已被封锁；「还原更新锁定」可回滚（按 wulock-backup.tsv）。")
            : T(L"更新未被封锁。点「锁定系统更新」执行封装镜像级封锁（先自动备份）。");
        out.canClean = true;
        ctx.Report(100, L"更新状态扫描完成");
    }

    // ---- 锁定 ----
    void Lock(ModuleContext& ctx, ModuleResult& out) {
        int ok = 0, fail = 0;

        // ① 策略封锁值（先备份原值再写）
        for (const WuPolicyItem& p : kWuPolicies) {
            const wstring sub = p.au ? kWuPolicyAu : kWuPolicyKey;
            DWORD t = REG_SZ;
            wstring cur = RegReadAny(HKEY_LOCAL_MACHINE, sub, p.val, &t);
            WuBackupAppend(L"SET\tHKLM\t" + wstring(sub) + L"\t" + p.val + L"\t" +
                           U32s((unsigned)t) + L"\t" + cur);
            if (RegWriteDword(HKEY_LOCAL_MACHINE, sub, p.val, 1)) {
                ++ok;
                ctx.Log(L"  已写策略 " + wstring(kWuPolicyKey) +
                        (p.au ? wstring(L"\\AU\\") : wstring(L"\\")) +
                        p.val + L" = 1");
            } else {
                ++fail;
                ctx.Log(wstring(L"  策略写入被拒绝：") + p.val + L"（" + ErrorTextZh(GetLastError()) +
                        L"）— 需管理员权限");
            }
        }

        // ② 服务禁用
        for (const wchar_t* s : kWuSvcs) {
            SvcState before = QuerySvcState(s);
            WuBackupAppend(wstring(L"SVC\t") + s + L"\t" + U32s(before.startType));
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            bool viaScm = false;
            if (scm) {
                SC_HANDLE svc = OpenServiceW(scm, s, SERVICE_CHANGE_CONFIG | SERVICE_STOP);
                if (svc) {
                    if (ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                                             SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr)) {
                        ++ok;
                        viaScm = true;
                        ctx.Log(L"  已禁用服务：" + wstring(s));
                    } else {
                        ctx.Log(L"  ChangeServiceConfig 拒绝：" + wstring(s) + L"（" +
                                ErrorTextZh(GetLastError()) + L"）→ 尝试注册表直写");
                    }
                    if (before.running && ControlService(svc, SERVICE_CONTROL_STOP, nullptr)) {
                        ctx.Log(L"  已请求停止服务：" + wstring(s));
                    } else if (before.running) {
                        ctx.Log(L"  停止服务请求失败（不影响禁用）：" + wstring(s) +
                                L"（" + ErrorTextZh(GetLastError()) + L"）");
                    }
                    CloseServiceHandle(svc);
                }
                CloseServiceHandle(scm);
            }
            if (!viaScm) {
                // 兜底：直写 Start=4 + FailureActions 防自愈（WaaSMedicSvc 被 ACL 保护路径）
                if (SvcRegWriteStart(s, SERVICE_DISABLED)) {
                    ++ok;
                    ctx.Log(L"  已直写注册表禁用服务：" + wstring(s) + L"（Start=4）");
                    PatchFailureActions(s, ctx);
                } else {
                    ++fail;
                    ctx.Log(L"  服务禁用失败：" + wstring(s) + L"（" +
                            ErrorTextZh(GetLastError()) + L"）— 键被系统保护，请提权后重试");
                }
            }
        }

        // ③ 计划任务禁用
        TaskComSession2 com;
        if (com.Init()) {
            for (const wchar_t* dir : kWuTaskDirs) {
                vector<std::pair<wstring, bool>> tasks;
                EnumTaskFolder(com.svc, dir, tasks);
                for (auto& t : tasks) {
                    WuBackupAppend(L"TASK\t" + t.first + L"\t" + (t.second ? L"1" : L"0"));
                    if (!t.second) continue;   // 已禁用
                    size_t slash = t.first.find_last_of(L'\\');
                    wstring name = (slash == wstring::npos) ? t.first : t.first.substr(slash + 1);
                    BSTR pb = SysAllocString(dir);
                    BSTR nb = SysAllocString(name.c_str());
                    ITaskFolder* folder = nullptr;
                    if (pb && SUCCEEDED(com.svc->GetFolder(pb, &folder)) && folder) {
                        IRegisteredTask* task = nullptr;
                        if (nb && SUCCEEDED(folder->GetTask(nb, &task)) && task) {
                            if (SUCCEEDED(task->put_Enabled(VARIANT_FALSE))) {
                                ++ok;
                                ctx.Log(L"  已禁用计划任务：" + t.first);
                            } else {
                                ++fail;
                                ctx.Log(L"  任务禁用被拒绝（记录跳过）：" + t.first + L"（" +
                                        HrTextZh2(GetLastError()) + L"）");
                            }
                            task->Release();
                        }
                        folder->Release();
                    }
                    SysFreeString(pb);
                    SysFreeString(nb);
                }
            }
        } else {
            ++fail;
            ctx.Log(L"计划任务 COM 初始化失败：" + com.error);
        }

        // ④ 可选 hosts 拦截
        if (HostsBlockEnabled()) LockHosts(ctx, ok, fail);

        wchar_t b[160];
        swprintf_s(b, 160, L"锁定完成：成功 %d 项，失败 %d 项%s", ok, fail,
                   fail && !IsElevated() ? L"（非管理员运行：写系统区被拒绝，请提权后重试）" : L"");
        out.summary = b;
        out.hint = fail ? T(L"存在失败项：全部失败原因已在日志中归因；提权后重试即可补齐。")
                        : T(L"更新封锁已生效。可用「还原更新锁定」随时回滚。");
        out.canClean = true;
    }

    // ---- 还原 ----
    void Restore(ModuleContext& ctx, ModuleResult& out) {
        vector<wstring> lines = WuBackupLines();
        if (lines.empty()) {
            // 无备份也按出厂默认还原服务（wuauserv=demand / UsoSvc=auto /
            // WaaSMedicSvc=demand / DoSvc=auto），其余按封锁表反向删除
            lines.push_back(L"SET\tHKLM\t" + wstring(kWuPolicyKey) + L"\tNoAutoUpdate\t4\t");
            lines.push_back(L"SET\tHKLM\t" + wstring(kWuPolicyKey) +
                            L"\tDisableWindowsUpdateAccess\t4\t");
            lines.push_back(L"SET\tHKLM\t" + wstring(kWuPolicyKey) +
                            L"\tDoNotConnectToWindowsUpdateInternetLocations\t4\t");
            lines.push_back(L"SET\tHKLM\t" + wstring(kWuPolicyKey) +
                            L"\tExcludeWUDriversInQualityUpdate\t4\t");
            lines.push_back(L"SET\tHKLM\t" + wstring(kWuPolicyAu) +
                            L"\tSetDisableUXWUAccess\t4\t");
            lines.push_back(L"SVC\twuauserv\t3");
            lines.push_back(L"SVC\tUsoSvc\t2");
            lines.push_back(L"SVC\tWaaSMedicSvc\t3");
            lines.push_back(L"SVC\tDoSvc\t2");
            ctx.Log(L"未找到备份记录，按出厂默认值还原");
        }
        int ok = 0, fail = 0;
        for (const wstring& line : lines) {
            size_t f1 = line.find(L'\t');
            if (f1 == wstring::npos) continue;
            wstring op = line.substr(0, f1);
            if (op == L"SET") {
                size_t f2 = line.find(L'\t', f1 + 1);
                size_t f3 = (f2 == wstring::npos) ? wstring::npos : line.find(L'\t', f2 + 1);
                size_t f4 = (f3 == wstring::npos) ? wstring::npos : line.find(L'\t', f3 + 1);
                size_t f5 = (f4 == wstring::npos) ? wstring::npos : line.find(L'\t', f4 + 1);
                if (f5 == wstring::npos) continue;
                HKEY root = (line.substr(f1 + 1, f2 - f1 - 1) == L"HKLM")
                                ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
                wstring sub = line.substr(f2 + 1, f3 - f2 - 1);
                wstring val = line.substr(f3 + 1, f4 - f3 - 1);
                wstring data = line.substr(f5 + 1);
                bool done = data.empty() ? RegDelValue2(root, sub, val)
                                         : RegWriteDword(root, sub, val,
                                                         (DWORD)wcstoul(data.c_str(), nullptr, 10));
                if (done) ++ok; else ++fail;
                ctx.Log(wstring(done ? L"  已还原 " : L"  还原失败 ") + sub + L"\\" + val);
            } else if (op == L"SVC") {
                size_t f2 = line.find(L'\t', f1 + 1);
                if (f2 == wstring::npos) continue;
                wstring name = line.substr(f1 + 1, f2 - f1 - 1);
                DWORD start = (DWORD)wcstoul(line.substr(f2 + 1).c_str(), nullptr, 10);
                if (start == 0 || start > 4) start = SERVICE_DEMAND_START;
                SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                bool done = false;
                if (scm) {
                    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_CHANGE_CONFIG);
                    if (svc) {
                        done = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, start,
                                                    SERVICE_NO_CHANGE, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr, nullptr,
                                                    nullptr) != FALSE;
                        CloseServiceHandle(svc);
                    }
                    CloseServiceHandle(scm);
                }
                if (!done) done = SvcRegWriteStart(name, start);
                if (done) ++ok; else ++fail;
                ctx.Log(wstring(done ? L"  已还原服务 " : L"  服务还原失败 ") + name +
                        L" → Start=" + U32s((unsigned)start));
            } else if (op == L"TASK") {
                size_t f2 = line.find(L'\t', f1 + 1);
                if (f2 == wstring::npos) continue;
                wstring path = line.substr(f1 + 1, f2 - f1 - 1);
                if (line.substr(f2 + 1) != L"0") continue;   // 原本就启用，无需动
                size_t slash = path.find_last_of(L'\\');
                wstring dir = (slash <= 0) ? wstring(L"\\") : path.substr(0, slash);
                wstring name = (slash == wstring::npos) ? path : path.substr(slash + 1);
                TaskComSession2 com;
                bool done = false;
                if (com.Init() && com.svc) {
                    BSTR db = SysAllocString(dir.c_str());
                    ITaskFolder* folder = nullptr;
                    if (db && SUCCEEDED(com.svc->GetFolder(db, &folder)) && folder) {
                        BSTR nb = SysAllocString(name.c_str());
                        IRegisteredTask* task = nullptr;
                        if (nb && SUCCEEDED(folder->GetTask(nb, &task)) && task) {
                            done = SUCCEEDED(task->put_Enabled(VARIANT_TRUE));
                            task->Release();
                        }
                        folder->Release();
                    }
                    SysFreeString(db);
                }
                if (done) ++ok; else ++fail;
                ctx.Log(wstring(done ? L"  已还原任务 " : L"  任务还原失败 ") + path);
            }
        }
        // hosts 拦截块移除
        if (HostsBlockPresent()) UnlockHosts(ctx, ok, fail);

        wchar_t b[160];
        swprintf_s(b, 160, L"还原完成：成功 %d 项，失败 %d 项%s", ok, fail,
                   fail && !IsElevated() ? L"（非管理员运行：写系统区被拒绝）" : L"");
        out.summary = b;
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: Lock(ctx, out); break;
        case 1: Restore(ctx, out); break;
        default: break;
        }
    }

private:
    wstring fHosts_;

    bool HostsBlockEnabled() const {
        wstring low = FoldLowerW(fHosts_);
        return low == L"yes" || low == L"y" || low == L"1" || low == L"开" ||
               low == L"启用" || low.find(L"开") != wstring::npos;
    }

    static bool HostsBlockPresent() {
        HANDLE h = CreateFileW(HostsFilePath().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        char buf[8192] = {};
        DWORD got = 0;
        std::string head;
        while (head.size() < 64 * 1024 &&
               ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
            head.append(buf, got);
        CloseHandle(h);
        wstring text = Utf8ToW(head);
        return text.find(kWuHostsMark1) != wstring::npos;
    }

    // 追加 hosts 拦截块（只读属性自动摘除并还原）
    void LockHosts(ModuleContext& ctx, int& ok, int& fail) {
        wstring path = HostsFilePath();
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            ++fail;
            ctx.Log(L"  hosts 拦截失败：无法访问 " + path + L"（" +
                    ErrorTextZh(GetLastError()) + L"）");
            return;
        }
        if (HostsBlockPresent()) {
            ctx.Log(L"  hosts 拦截块已存在，跳过");
            return;
        }
        bool wasRo = (attr & FILE_ATTRIBUTE_READONLY) != 0;
        if (wasRo) SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (wasRo) SetFileAttributesW(path.c_str(), attr);
            ++fail;
            ctx.Log(L"  hosts 追加失败：" + ErrorTextZh(GetLastError()) +
                    L" — 需管理员权限");
            return;
        }
        wstring block = wstring(L"\r\n") + kWuHostsMark1 + L"\r\n";
        for (const wchar_t* d : kWuHostsDomains) block += wstring(L"127.0.0.1 ") + d + L"\r\n";
        block += wstring(kWuHostsMark2) + L"\r\n";
        std::string u8 = WToUtf8(block);
        DWORD w = 0;
        bool done = WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr) != FALSE;
        CloseHandle(h);
        if (wasRo) SetFileAttributesW(path.c_str(), attr);
        if (done) {
            ++ok;
            ctx.Log(L"  已追加 hosts 更新域名拦截块（" + U32s((unsigned)kWuHostsDomainsCount()) +
                    L" 个域名，标记注释可识别可还原）");
        } else {
            ++fail;
            ctx.Log(L"  hosts 写入失败：" + ErrorTextZh(GetLastError()));
        }
    }

    // 移除标记块之间的整块内容
    void UnlockHosts(ModuleContext& ctx, int& ok, int& fail) {
        wstring path = HostsFilePath();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ++fail;
            ctx.Log(L"  hosts 读取失败：" + ErrorTextZh(GetLastError()));
            return;
        }
        std::string raw;
        char buf[8192];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
        CloseHandle(h);
        wstring text = Utf8ToW(raw);
        size_t b1 = text.find(kWuHostsMark1);
        size_t b2 = text.find(kWuHostsMark2);
        if (b1 == wstring::npos || b2 == wstring::npos || b2 < b1) return;
        wstring outText = text.substr(0, b1) + text.substr(b2 + wcslen(kWuHostsMark2));

        DWORD attr = GetFileAttributesW(path.c_str());
        bool wasRo = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_READONLY);
        if (wasRo) SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w == INVALID_HANDLE_VALUE) {
            if (wasRo) SetFileAttributesW(path.c_str(), attr);
            ++fail;
            ctx.Log(L"  hosts 重写失败：" + ErrorTextZh(GetLastError()) + L" — 需管理员权限");
            return;
        }
        std::string u8 = WToUtf8(outText);
        DWORD wr = 0;
        bool done = WriteFile(w, u8.data(), (DWORD)u8.size(), &wr, nullptr) != FALSE;
        CloseHandle(w);
        if (wasRo) SetFileAttributesW(path.c_str(), attr);
        if (done) {
            ++ok;
            ctx.Log(L"  已移除 hosts 更新域名拦截块");
        } else {
            ++fail;
            ctx.Log(L"  hosts 重写失败：" + ErrorTextZh(GetLastError()));
        }
    }

    static unsigned kWuHostsDomainsCount() {
        return (unsigned)(sizeof(kWuHostsDomains) / sizeof(kWuHostsDomains[0]));
    }
};

REGISTER_MODULE(PurgWuLockModule);

} // namespace
} // namespace mz
