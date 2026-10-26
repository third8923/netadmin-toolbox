// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 计划任务管理模块（v2.8 T03/R4）
//  ---------------------------------------------------------------------------
//  sys.tasks 计划任务：ITaskService COM（taskschd.h，路线 B，见 dev-plan §1.4）
//    * 枚举：根文件夹递归 GetFolders + GetTasks(TASK_ENUM_HIDDEN)
//    * 展示：任务名 / 状态(中文) / 所在文件夹+作者 / 下次·上次运行·结果码
//    * 动作：①立即运行 ②结束选中 ③更多 ▾（启用/禁用/删除）
//    * ModuleRow.tag = paths_ 下标（排序安全回溯）
//  COM 初始化在 Run/RunAction 内各做一次（APARTMENTTHREADED），
//  RPC_E_CHANGED_MODE 视为可用（不配对 CoUninitialize）。
//  链接：-ltaskschd -lole32 -loleaut32（build.bat）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../cleanresult.h"   // ErrorTextZh（HRESULT/Win32 中文归因）

#include <algorithm>
#include <cstdio>

#include <taskschd.h>
#include <oleauto.h>

namespace mz {
namespace {

// MinGW taskschd.h 未导出 CLSID_TaskScheduler，本地定义（标准值）
static const CLSID kCLSID_TaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };

// ---- COM 小工具 ----

// BSTR 拷贝为 wstring 并释放
wstring TakeBstr(BSTR b) {
    wstring out = b ? wstring(b, SysStringLen(b)) : wstring();
    SysFreeString(b);
    return out;
}

// OLE DATE → "YYYY-MM-DD HH:MM"；无效值（<=0）返回「从不」
wstring FormatOleDate(DATE d) {
    if (d <= 0) return L"从不";
    SYSTEMTIME st = {};
    if (!VariantTimeToSystemTime(d, &st)) return L"-";
    wchar_t b[40] = {};
    swprintf_s(b, 40, L"%04u-%02u-%02u %02u:%02u",
               (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
               (unsigned)st.wHour, (unsigned)st.wMinute);
    return b;
}

// 任务状态 → 中文（Enabled=false 时优先显示「已禁用」）
wstring TaskStateText(TASK_STATE s, VARIANT_BOOL enabled) {
    if (!enabled && s != TASK_STATE_RUNNING) return L"已禁用";
    switch (s) {
    case TASK_STATE_DISABLED: return L"已禁用";
    case TASK_STATE_QUEUED:   return L"排队中";
    case TASK_STATE_READY:    return L"就绪";
    case TASK_STATE_RUNNING:  return L"正在运行";
    default:                  return L"未知";
    }
}

// LastTaskResult → 展示文本（0=成功；0x41300/0x41303=尚未运行；其余显示 HEX）
wstring TaskResultText(LONG r) {
    if (r == 0) return L"成功";
    if (r == 0x41300 || r == 0x41303) return L"尚未运行";
    wchar_t b[32] = {};
    swprintf_s(b, 32, L"0x%08lX", (unsigned long)r);
    return b;
}

// HRESULT → 中文（Win32 facility 展开为系统错误码归因）
wstring HrTextZh(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return ErrorTextZh(HRESULT_CODE(hr));
    return ErrorTextZh((DWORD)hr);
}

// 从完整任务路径取父文件夹（"\A\B\task" → "\A\B"；根任务 → "\"）
wstring ParentFolderOf(const wstring& path) {
    size_t slash = path.find_last_of(L'\\');
    if (slash == wstring::npos || slash == 0) return L"\\";
    return path.substr(0, slash);
}

// COM 会话：CoInitializeEx + CoCreateInstance + Connect（RAII 释放）
struct TaskComSession {
    ITaskService* svc = nullptr;
    bool comUninit = false;
    wstring error;   // 非空 = 初始化失败原因

    ~TaskComSession() {
        if (svc) svc->Release();
        if (comUninit) CoUninitialize();
    }

    bool Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            comUninit = true;                    // 配对释放
        } else if (hr == RPC_E_CHANGED_MODE) {
            comUninit = false;                   // 已按其他模式初始化，可用但不释放
        } else {
            error = L"COM 初始化失败（错误码 " + std::to_wstring((DWORD)hr) + L"）";
            return false;
        }
        hr = CoCreateInstance(kCLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITaskService, (void**)&svc);
        if (FAILED(hr) || !svc) {
            error = L"无法创建任务计划程序 COM 实例：" + HrTextZh(hr);
            return false;
        }
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        hr = svc->Connect(vEmpty, vEmpty, vEmpty, vEmpty);
        if (FAILED(hr)) {
            error = L"连接任务计划程序服务失败：" + HrTextZh(hr);
            return false;
        }
        return true;
    }
};

// 从任务计划程序解析单个任务对象（RunAction 用）
// 成功时 *folder / *task 由调用方 Release
bool ResolveTask(ITaskService* svc, const wstring& fullPath,
                 ITaskFolder** folder, IRegisteredTask** task, wstring* err) {
    size_t slash = fullPath.find_last_of(L'\\');
    wstring name = (slash == wstring::npos) ? fullPath : fullPath.substr(slash + 1);
    wstring parent = ParentFolderOf(fullPath);
    BSTR parentB = SysAllocString(parent.c_str());
    BSTR nameB = SysAllocString(name.c_str());
    bool ok = false;
    if (parentB && nameB && SUCCEEDED(svc->GetFolder(parentB, folder))) {
        if (SUCCEEDED((*folder)->GetTask(nameB, task))) {
            ok = true;
        } else {
            *err = L"任务不存在（可能已被删除）";
        }
    } else if (err->empty()) {
        *err = L"打开任务文件夹失败：" + parent;
    }
    SysFreeString(parentB);
    SysFreeString(nameB);
    return ok;
}

class SysTasksModule : public IModule {
public:
    const wchar_t* Id() const override { return L"sys.tasks"; }
    const wchar_t* Group() const override { return L"系统管理"; }
    const wchar_t* Title() const override { return L"计划任务"; }
    const wchar_t* Description() const override { return L"任务计划程序全量枚举与操作：立即运行/结束/启用/禁用/删除"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    // ---- 扫描：递归枚举全部文件夹与任务 ----
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        paths_.clear();
        isMs_.clear();

        TaskComSession com;
        if (!com.Init()) {
            out.summary = com.error;
            out.hint = L"本模块通过任务计划程序 COM 接口枚举，请确认 Task Scheduler 服务正在运行。";
            return;
        }

        ITaskFolder* root = nullptr;
        BSTR rootB = SysAllocString(L"\\");
        HRESULT hr = rootB ? com.svc->GetFolder(rootB, &root) : E_OUTOFMEMORY;
        SysFreeString(rootB);
        if (FAILED(hr) || !root) {
            out.summary = L"打开任务计划程序根文件夹失败：" + HrTextZh(hr);
            return;
        }

        int hidden = 0;
        WalkFolder(ctx, com.svc, root, &hidden, out);
        root->Release();

        std::stable_sort(out.rows.begin(), out.rows.end(),
                         [](const ModuleRow& a, const ModuleRow& b) {
                             if (a.c3 != b.c3) return a.c3 < b.c3;   // 按文件夹聚合
                             return a.c1 < b.c1;
                         });

        int running = 0, disabledCnt = 0;
        for (auto& r : out.rows) {
            if (r.c2 == L"正在运行") ++running;
            if (r.c2 == L"已禁用") ++disabledCnt;
        }
        wchar_t b[256];
        swprintf_s(b, L"共 %zu 个计划任务 · 正在运行 %d · 已禁用 %d（含隐藏任务）",
                   out.rows.size(), running, disabledCnt);
        out.summary = b;
        if (!IsElevated()) out.hint = L"提示：标准用户权限下部分系统任务信息可能读取不全。";
        out.canClean = true;
        ctx.Report(100, L"计划任务枚举完成");
    }

    // ---- 扩展动作：0 立即运行 / 1 结束选中 / 2..4 收进「更多 ▾」菜单 ----
    vector<ModuleAction> Actions() const override {
        return {
            { L"立即运行选中", nullptr, false, 0xE768 },
            { L"结束选中任务", nullptr, false, 0xE71A },
            { L"启用选中任务", nullptr, false, 0 },
            { L"禁用选中任务", L"禁用后任务将不再按计划自动执行（可随时重新启用）。\n\n是否继续？", false, 0 },
            { L"删除选中任务", nullptr, true, 0xE74D },
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        if (tags.empty()) return L"未选中任何任务。\n\n请先在列表中勾选要操作的任务。";
        wstring names;
        int msCount = 0;
        for (int t : tags) {
            wstring p = (t >= 0 && t < (int)paths_.size()) ? paths_[t]
                                                           : (L"任务索引 " + std::to_wstring(t));
            if (t >= 0 && t < (int)isMs_.size() && isMs_[t]) ++msCount;
            names += p + L"\n";
        }
        wstring body;
        switch (actionIndex) {
        case 0: body = L"将立即触发运行 " + std::to_wstring(tags.size()) + L" 个任务：\n" + names; break;
        case 1: body = L"将请求结束 " + std::to_wstring(tags.size()) +
                       L" 个任务（仅对正在运行的任务有效）：\n" + names; break;
        case 2: body = L"将启用 " + std::to_wstring(tags.size()) + L" 个任务：\n" + names; break;
        case 3: body = L"将禁用 " + std::to_wstring(tags.size()) + L" 个任务：\n" + names +
                       L"\n禁用后任务不再按计划自动执行（可重新启用）。"; break;
        case 4: body = L"将永久删除 " + std::to_wstring(tags.size()) + L" 个任务：\n" + names +
                       L"\n删除后任务定义不可恢复。"; break;
        }
        if (actionIndex == 4 && msCount > 0)
            body += L"\n注意：其中 " + std::to_wstring(msCount) +
                    L" 个为 \\Microsoft\\ 官方任务，可能影响系统功能，请谨慎确认。";
        body += L"\n是否继续？";
        return body;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex < 0 || actionIndex > 4) return;
        if (selection_.empty()) {
            out.summary = L"未选中任何任务";
            out.canClean = true;
            return;
        }
        TaskComSession com;
        if (!com.Init()) {
            out.summary = com.error;
            return;
        }
        int ok = 0, skip = 0, fail = 0;
        for (int tag : selection_) {
            if (ctx.Cancelled()) break;
            if (tag < 0 || tag >= (int)paths_.size()) continue;
            const wstring& fullPath = paths_[tag];
            wstring dispName = fullPath;

            ITaskFolder* folder = nullptr;
            IRegisteredTask* task = nullptr;
            wstring err;
            if (!ResolveTask(com.svc, fullPath, &folder, &task, &err) || !folder || !task) {
                ++fail;
                ctx.Log(L"  失败: " + fullPath + L" — " + err);
                continue;
            }

            switch (actionIndex) {
            case 0: {   // 立即运行
                VARIANT pv;
                VariantInit(&pv);
                IRunningTask* running = nullptr;
                HRESULT hr = task->Run(pv, &running);
                if (SUCCEEDED(hr)) {
                    ++ok;
                    ctx.Log(L"  已触发运行: " + fullPath);
                    if (running) running->Release();
                } else {
                    ++fail;
                    ctx.Log(L"  失败: " + fullPath + L" — " + HrTextZh(hr));
                }
                VariantClear(&pv);
            } break;
            case 1: {   // 结束选中（仅运行中有效）
                TASK_STATE st = TASK_STATE_UNKNOWN;
                task->get_State(&st);
                if (st != TASK_STATE_RUNNING) {
                    ++skip;
                    ctx.Log(L"  跳过: " + fullPath + L" — 未在运行（状态：" +
                            TaskStateText(st, VARIANT_TRUE) + L"）");
                } else if (SUCCEEDED(task->Stop(0))) {
                    ++ok;
                    ctx.Log(L"  已请求结束: " + fullPath);
                } else {
                    ++fail;
                    ctx.Log(L"  失败: " + fullPath + L" — " + HrTextZh(GetLastError()));
                }
            } break;
            case 2: case 3: {   // 启用 / 禁用
                VARIANT_BOOL want = (actionIndex == 2) ? VARIANT_TRUE : VARIANT_FALSE;
                VARIANT_BOOL cur = want;
                task->get_Enabled(&cur);
                if (cur == want) {
                    ++skip;
                    ctx.Log(wstring(L"  跳过: ") + fullPath +
                            (want ? L" — 已是启用状态" : L" — 已是禁用状态"));
                } else if (SUCCEEDED(task->put_Enabled(want))) {
                    ++ok;
                    ctx.Log(wstring(want ? L"  已启用: " : L"  已禁用: ") + fullPath);
                } else {
                    ++fail;
                    ctx.Log(L"  失败: " + fullPath + L" — " + HrTextZh(GetLastError()));
                }
            } break;
            case 4: {   // 删除
                if (tag < (int)isMs_.size() && isMs_[tag])
                    ctx.Log(L"  警示: " + fullPath + L" 为 \\Microsoft\\ 官方任务，可能影响系统功能");
                size_t slash = fullPath.find_last_of(L'\\');
                wstring name = (slash == wstring::npos) ? fullPath : fullPath.substr(slash + 1);
                BSTR nameB = SysAllocString(name.c_str());
                HRESULT hr = nameB ? folder->DeleteTask(nameB, 0) : E_OUTOFMEMORY;
                SysFreeString(nameB);
                if (SUCCEEDED(hr)) {
                    ++ok;
                    ctx.Log(L"  已删除: " + fullPath);
                } else {
                    ++fail;
                    ctx.Log(L"  失败: " + fullPath + L" — " + HrTextZh(hr));
                }
            } break;
            }
            (void)dispName;
            task->Release();
            folder->Release();
        }
        static const wchar_t* kNames[] = { L"运行任务", L"结束任务", L"启用任务",
                                           L"禁用任务", L"删除任务" };
        wchar_t b[256];
        swprintf_s(b, L"%s完成：成功 %d，跳过 %d，失败 %d", kNames[actionIndex], ok, skip, fail);
        out.summary = b;
        if (fail > 0 && !IsElevated())
            out.hint = L"部分操作失败，可能需要管理员权限（请提权后重试）。";
        out.canClean = true;
    }

private:
    vector<int> selection_;
    vector<wstring> paths_;   // tag → 任务完整路径（\Folder\Name）
    vector<bool> isMs_;       // 是否 \Microsoft\ 官方任务

    // 读取任务作者（Definition → RegistrationInfo → Author；失败返回空）
    wstring TaskAuthor(IRegisteredTask* task) {
        ITaskDefinition* def = nullptr;
        if (FAILED(task->get_Definition(&def)) || !def) return wstring();
        IRegistrationInfo* ri = nullptr;
        wstring author;
        if (SUCCEEDED(def->get_RegistrationInfo(&ri)) && ri) {
            BSTR a = nullptr;
            if (SUCCEEDED(ri->get_Author(&a))) author = TakeBstr(a);
            ri->Release();
        }
        def->Release();
        return author;
    }

    // 单个任务 → 列表行
    void AddTaskRow(IRegisteredTask* task, ModuleResult& out) {
        BSTR bName = nullptr, bPath = nullptr;
        TASK_STATE st = TASK_STATE_UNKNOWN;
        VARIANT_BOOL enabled = VARIANT_TRUE;
        DATE last = 0, next = 0;
        LONG result = 0;
        task->get_Name(&bName);
        task->get_Path(&bPath);
        task->get_State(&st);
        task->get_Enabled(&enabled);
        task->get_LastRunTime(&last);
        task->get_NextRunTime(&next);
        task->get_LastTaskResult(&result);
        wstring name = TakeBstr(bName);
        wstring path = TakeBstr(bPath);

        // tag → paths_ 下标
        int tag = (int)paths_.size();
        paths_.push_back(path);
        // \Microsoft\ 官方任务识别（大小写不敏感）
        wstring low = path;
        for (auto& c : low) {
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        }
        isMs_.push_back(low.rfind(L"\\microsoft\\", 0) == 0);

        size_t slash = path.find_last_of(L'\\');
        wstring folder = (slash <= 0) ? L"根目录" : path.substr(1, slash - 1);
        wstring author = TaskAuthor(task);
        wstring c3 = folder;
        if (!author.empty()) c3 += L" · " + author;

        wchar_t b[256];
        swprintf_s(b, L"下次 %s · 上次 %s · 结果 %s",
                   FormatOleDate(next).c_str(), FormatOleDate(last).c_str(),
                   TaskResultText(result).c_str());

        ModuleRow r;
        r.c1 = name.empty() ? path : name;
        r.c2 = TaskStateText(st, enabled);
        r.c3 = c3;
        r.c4 = b;
        r.sortKey = (unsigned long long)tag;
        r.danger = false;
        r.tag = tag;
        out.rows.push_back(r);
    }

    // 递归遍历任务文件夹
    void WalkFolder(ModuleContext& ctx, ITaskService* svc, ITaskFolder* folder,
                    int* hidden, ModuleResult& out) {
        (void)svc;
        // 本文件夹任务（含隐藏）
        IRegisteredTaskCollection* tasks = nullptr;
        if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
            LONG n = 0;
            tasks->get_Count(&n);
            for (LONG i = 0; i < n; ++i) {
                if (ctx.Cancelled()) break;
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_I4;
                v.lVal = i;
                IRegisteredTask* t = nullptr;
                if (SUCCEEDED(tasks->get_Item(v, &t)) && t) {
                    VARIANT_BOOL en = VARIANT_TRUE;
                    t->get_Enabled(&en);
                    if (!en) ++*hidden;
                    AddTaskRow(t, out);
                    t->Release();
                }
                VariantClear(&v);
            }
            tasks->Release();
        }
        // 子文件夹递归
        ITaskFolderCollection* subs = nullptr;
        if (SUCCEEDED(folder->GetFolders(0, &subs)) && subs) {
            LONG n = 0;
            subs->get_Count(&n);
            for (LONG i = 0; i < n && !ctx.Cancelled(); ++i) {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_I4;
                v.lVal = i;
                ITaskFolder* sub = nullptr;
                if (SUCCEEDED(subs->get_Item(v, &sub)) && sub) {
                    WalkFolder(ctx, svc, sub, hidden, out);
                    sub->Release();
                }
                VariantClear(&v);
            }
            subs->Release();
        }
    }
};

} // namespace

REGISTER_MODULE(SysTasksModule);

} // namespace mz
