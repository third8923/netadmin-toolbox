#include "cli.h"
#include "module.h"
#include "cleaner.h"
#include "common.h"
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <algorithm>

namespace mz {

// 粗略计算显示宽度（CJK 记 2）
static size_t inline_wstr_width(const wstring& s) {
    size_t w = 0;
    for (wchar_t c : s) w += (c > 0x2E80) ? 2 : 1;
    return w;
}

static void Out(const wstring& s) {
    std::string u = WToUtf8(s);
    u += "\n";
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
}

// GUI 子系统程序被管道/重定向调用时也要能看到输出
static void EnsureConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD ft = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE || ft == FILE_TYPE_CHAR) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    FILE* fi = nullptr;
    freopen_s(&fi, "CONIN$", "r", stdin);
}

static const wchar_t* KindText(ModuleKind k) {
    switch (k) {
        case ModuleKind::Inventory: return T(L"盘点");
        case ModuleKind::Cleaner:   return T(L"清理");
        case ModuleKind::Action:    return T(L"动作");
        case ModuleKind::Tweaks:    return T(L"优化");
        case ModuleKind::Console:   return T(L"输出");
    }
    return L"?";
}

static void Help() {
    Out(T(L"网管工具箱 NetAdmin Toolbox v") +
        std::to_wstring(k_AppVersionMajor) + L"." + std::to_wstring(k_AppVersionMinor));
    Out(T(L"单文件、免安装、可扩展的网管工具集  |  MIT License"));
    Out(L"");
    Out(T(L"用法:"));
    Out(T(L"  netadmin                      启动图形界面"));
    Out(T(L"  netadmin --list               列出所有功能模块"));
    Out(T(L"  netadmin --help               显示本帮助"));
    Out(L"");
    Out(T(L"执行模块:"));
    Out(T(L"  netadmin run <模块ID> [--path <路径>] [--target <目标>] [--field <序号>=<值>]"));
    Out(T(L"  netadmin run <模块ID> --action <动作序号> [--pick <名称或PID>]  扩展动作"));
    Out(L"");
    Out(T(L"示例:"));
    Out(L"  netadmin --list");
    Out(L"  netadmin run clean.basic");
    Out(T(L"  netadmin run sys.process --action 0 --pick notepad.exe   结束进程"));
    Out(L"  netadmin run net.adapters");
    Out(L"  netadmin run net.diag --target 8.8.8.8");
    Out(L"  netadmin run net.portscan --target 192.168.1.1");
    Out(L"  netadmin run scan.large --path D:\\ --target 500");
    Out(L"  netadmin run tweak.winopt");
    Out(L"");
    Out(T(L"提示：新增功能 = 在 src/modules/ 加一个 .cpp 并用 REGISTER_MODULE 注册，"));
    Out(T(L"      界面与命令行会自动出现该模块。"));
}

static void ListModules() {
    auto& reg = ModuleRegistry::Instance();
    auto groups = reg.Grouped();
    Out(T(L"共 ") + std::to_wstring(reg.All().size()) + T(L" 个功能模块"));
    for (auto& g : groups) {
        Out(L"");
        Out(L"【" + g.group + L"】");
        for (auto* m : g.mods) {
            wstring line = L"  ";
            line += m->Id();
            size_t w = inline_wstr_width(line);
            size_t pad = 32;
            line += wstring(w < pad ? pad - w : 2, L' ');
            line += L"[";
            line += KindText(m->Kind());
            line += L"]  ";
            line += m->Title();
            Out(line);
        }
    }
}

static void PrintResult(const ModuleResult& r) {
    Out(L"");
    if (!r.summary.empty()) Out(r.summary);
    if (!r.hint.empty()) {
        // 模块可能自带「提示：」前缀，避免重复
        if (r.hint.rfind(T(L"提示"), 0) == 0) Out(r.hint);
        else Out(T(L"提示: ") + r.hint);
    }
    if (r.rows.empty()) {
        Out(T(L"(无数据)"));
        return;
    }
    Out(L"--------------------------------------------------------------");
    for (auto& row : r.rows) {
        wstring line = L"  " + row.c1;
        size_t w = inline_wstr_width(line);
        size_t target = 32;
        line += wstring(w < target ? target - w : 2, L' ');
        if (!row.c2.empty()) { line += row.c2; line += L"  "; }
        if (!row.c3.empty()) { line += row.c3; line += L"  "; }
        if (!row.c4.empty()) line += row.c4;
        Out(line);
    }
}

int RunCli(int argc, wchar_t** argv) {
    EnsureConsole();
    SetConsoleOutputCP(CP_UTF8);
    lang::LoadOnStartup();   // v2.18 i18n：CLI 同样遵循 ini 语言选择（zh-CN 缺省原样）

    if (argc < 2) { Help(); return 0; }
    wstring cmd = argv[1];

    if (cmd == L"--help" || cmd == L"-h" || cmd == L"/?") { Help(); return 0; }
    if (cmd == L"--list" || cmd == L"list" || cmd == L"modules") { ListModules(); return 0; }

    wstring id, pathArg, targetArg;
    int actionArg = -1;
    wstring pickArg;
    vector<wstring> fieldArgs;   // --field N=值（v2.12 R3，可重复）
    if (cmd == L"run") {
        if (argc < 3) { Out(T(L"缺少模块 ID，用 --list 查看")); return 1; }
        id = argv[2];
        for (int i = 3; i < argc; ++i) {
            wstring a = argv[i];
            if (a == L"--path" && i + 1 < argc) pathArg = argv[++i];
            else if (a == L"--target" && i + 1 < argc) targetArg = argv[++i];
            else if (a == L"--action" && i + 1 < argc) actionArg = _wtoi(argv[++i]);
            else if (a == L"--pick" && i + 1 < argc) pickArg = argv[++i];
            else if (a == L"--field" && i + 1 < argc) fieldArgs.push_back(argv[++i]);
        }
    } else {
        id = cmd;
        if (argc >= 3) pathArg = argv[2];
        if (argc >= 4) targetArg = argv[3];
    }

    IModule* m = ModuleRegistry::Instance().Find(id);
    if (!m) {
        Out(T(L"未知模块: ") + id);
        Out(T(L"用 netadmin --list 查看可用模块"));
        return 1;
    }

    ModuleContext ctx;
    ctx.log.onLine = [](const wstring& s) { Out(s); };
    ctx.progress = [](int p, const wstring& s) {
        if (s.empty()) return;
        static int tick = 0;
        if (p < 0 || (++tick % 12) == 0)
            fprintf(stderr, "\r%s        ", WToUtf8(s).c_str());
    };
    ctx.cancel = []() { return false; };

    m->SetInput(pathArg, targetArg);

    // ---- 扩展字段注入（v2.12 R3）：--field <序号>=<值>，可重复 ----
    if (!fieldArgs.empty()) {
        auto fs = m->Fields();
        vector<wstring> vals(fs.size());
        for (const wstring& fa : fieldArgs) {
            size_t eq = fa.find(L'=');
            if (eq == wstring::npos) continue;
            int idx = _wtoi(fa.substr(0, eq).c_str());
            if (idx >= 0 && idx < (int)fs.size()) vals[idx] = fa.substr(eq + 1);
            else Out(T(L"忽略 --field（序号越界）: ") + fa);
        }
        if (!vals.empty()) m->SetFields(vals);
    }

    // ---- 扩展动作模式（v2.8 T03）：--action N [--pick 名称或PID] ----
    // 先扫描拿到行数据，再按 --pick 匹配出选中行（tag 值），交给 RunAction。
    if (actionArg >= 0) {
        ModuleResult scan;
        m->Run(ctx, scan);
        vector<int> sel;
        bool numeric = !pickArg.empty() &&
                       pickArg.find_first_not_of(L"0123456789") == wstring::npos;
        for (auto& row : scan.rows) {
            bool hit = false;
            if (pickArg.empty()) {
                hit = true;   // 未指定 --pick = 全选
            } else if (numeric) {
                hit = (row.tag == _wtoi(pickArg.c_str()));   // 数字 = 精确 tag/PID 匹配
            } else {
                wstring low1 = row.c1, low2 = pickArg;
                for (auto& c : low1) c = (wchar_t)towlower(c);
                for (auto& c : low2) c = (wchar_t)towlower(c);
                hit = (low1 == low2) || (low1.find(low2) != wstring::npos);
            }
            if (hit) sel.push_back(row.tag);
        }
        if (!pickArg.empty() && sel.empty()) {
            Out(T(L"未匹配到任何行（--pick ") + pickArg + L"）");
            return 1;
        }
        // pickArg 为空 = 全选（无行数据模块如 sec.repair 则空选区，动作自足）
        Out(T(L"▶ 动作 ") + std::to_wstring(actionArg) + L" @ " + wstring(m->Title()) +
            T(L"  选中 ") + std::to_wstring(sel.size()) + T(L" 项"));
        m->SetSelection(sel);
        ModuleResult r;
        m->RunAction(ctx, actionArg, r);
        fputc('\n', stderr);
        PrintResult(r);
        return 0;
    }

    Out(L"▶ " + wstring(m->Title()) + L"  (" + m->Id() + L")");
    ModuleResult r;
    m->Run(ctx, r);
    fputc('\n', stderr);
    PrintResult(r);
    return 0;
}

} // namespace mz
