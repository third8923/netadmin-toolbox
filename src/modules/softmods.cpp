// ============================================================================
//  软件管家 —— winget 图形化管理模块组（v2.7 新增）
//  ---------------------------------------------------------------------------
//  基于系统自带的 winget（Windows 包管理器），零第三方依赖：
//    * 装机必备推荐榜单：8 个模块（总榜 + 7 个分类），勾选一键安装/卸载
//    * 软件搜索：winget search，支持关键词
//    * 已安装软件：winget list，可勾选卸载
//    * 软件升级：winget upgrade，可勾选升级
//
//  设计约定：
//    * 所有 winget 调用隐藏窗口执行并捕获输出（RunWingetCapture），
//      失败/超时只降级为 summary/hint 文案 + 日志，不弹窗、不崩溃
//    * 与 DebloatMod 一致：ModuleRow.tag 记录内部下标，排序后勾选仍正确
//    * 长循环内检查 ctx.Cancelled()，支持 Esc 中止
// ============================================================================
#include "../module.h"
#include <algorithm>
#include <map>
#include <functional>

namespace mz {
namespace {

// ---------------------------------------------------------------- 基础工具

// 去除首尾空白（空格 / 制表符）
wstring TrimStr(const wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == wstring::npos) return wstring();
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

// 按 \r\n / \n / \r 切行（winget 重定向输出为 \r\n，控制台模式可能只有 \r）
vector<wstring> SplitLines(const wstring& text) {
    vector<wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find_first_of(L"\r\n", start);
        if (nl == wstring::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        size_t next = nl;
        while (next < text.size() && (text[next] == L'\r' || text[next] == L'\n')) ++next;
        start = next;
    }
    return lines;
}

// 字符显示宽度：CJK / 全角记 2 列（winget 表格按显示宽度填充列，
// 中文行若按字符位置切列会错位，必须按显示宽度对齐）
int DispWidth(wchar_t c) {
    if (c >= 0x1100 && c <= 0x115F) return 2;    // 谚文字母
    if (c >= 0x2E80 && c <= 0x303E) return 2;    // CJK 部首 / 标点
    if (c >= 0x3041 && c <= 0x33FF) return 2;    // 平假名 / 片假名 / 注音
    if (c >= 0x3400 && c <= 0x4DBF) return 2;    // CJK 统一表意扩展 A
    if (c >= 0x4E00 && c <= 0x9FFF) return 2;    // CJK 统一表意
    if (c >= 0xA000 && c <= 0xA4CF) return 2;    // 彝文
    if (c >= 0xAC00 && c <= 0xD7A3) return 2;    // 谚文音节
    if (c >= 0xF900 && c <= 0xFAFF) return 2;    // CJK 兼容表意
    if (c >= 0xFE30 && c <= 0xFE4F) return 2;    // CJK 兼容形式
    if (c >= 0xFF00 && c <= 0xFF60) return 2;    // 全角形式
    if (c >= 0xFFE0 && c <= 0xFFE6) return 2;    // 全角符号
    if (c >= 0x1F300 && c <= 0x1FAFF) return 2;  // 表情符号
    if (c >= 0x20000 && c <= 0x2FA1F) return 2;  // CJK 扩展 B 及以后
    return 1;
}

// 输出前几行非空内容（失败时给日志一点上下文，每行截断到 120 字）
wstring FirstLines(const wstring& s, int maxLines) {
    wstring out;
    int n = 0;
    for (auto& l : SplitLines(s)) {
        wstring t = TrimStr(l);
        if (t.empty()) continue;
        if (t.size() > 120) t = t.substr(0, 120) + L"…";
        out += (out.empty() ? L"" : L"  |  ") + t;
        if (++n >= maxLines) break;
    }
    return out;
}

// ---------------------------------------------------------------- winget 执行层

// winget 命令统一开关：接受源协议 + 禁止交互（无人值守时不弹确认框）
const wchar_t* const kWgBase = L"--accept-source-agreements --disable-interactivity";

// 单次 winget 调用等待上限：安装/升级大包可能远超 common.cpp RunHidden 的
// 120 秒，这里放宽到 15 分钟；超时结束进程并返回 -2
constexpr int kWingetTimeoutMs = 900000;

// 隐藏窗口执行命令并捕获输出，返回退出码（-1 = 启动失败，-2 = 超时）。
// 管道方案与 common.cpp RunHidden 相同，仅等待上限不同（不改动公共文件）。
int RunWingetCapture(const wstring& cmdline, int timeoutMs, wstring* outText) {
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

    // 读取输出（管道 EOF 即读取完毕）
    std::string raw;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
        raw.append(buf, read);
    }
    CloseHandle(hRead);

    // 等待进程退出（放宽后的上限，分片等待便于将来扩展取消检查）
    DWORD waited = 0;
    bool finished = false;
    while (waited < (DWORD)timeoutMs) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) { finished = true; break; }
        waited += 200;
    }
    DWORD code = 0;
    if (finished) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        code = (DWORD)-2;   // 超时
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (outText) {
        // winget 重定向到管道时输出 UTF-8；失败再按 OEM / ANSI 兜底
        UINT cp = CP_UTF8;
        int n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
        if (n <= 0) { cp = CP_OEMCP; n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0); }
        if (n <= 0) { cp = CP_ACP;   n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0); }
        wstring w(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), w.data(), n);
        *outText = w;
    }
    return (int)code;
}

// 探测 winget 是否可用；成功时出版本号（首行非空文本）
bool ProbeWinget(wstring* version) {
    wstring out;
    int code = RunWingetCapture(L"winget --version", 60000, &out);
    if (code != 0 || out.empty()) return false;
    for (auto& l : SplitLines(out)) {
        wstring t = TrimStr(l);
        if (!t.empty()) {
            if (version) *version = t;
            return true;
        }
    }
    return false;
}

// 命令构造（统一带基础开关）
wstring CmdList()    { return wstring(L"winget list ") + kWgBase; }
wstring CmdUpgrade() { return wstring(L"winget upgrade ") + kWgBase; }
wstring CmdSearch(const wstring& keyword) {
    // 关键词统一加引号；先去掉用户可能手输的引号避免嵌套
    wstring clean;
    for (wchar_t c : keyword) if (c != L'"') clean += c;
    return L"winget search \"" + clean + L"\" " + kWgBase;
}
wstring CmdInstall(const wstring& id) {
    return L"winget install --id \"" + id + L"\" -e --silent -h --accept-package-agreements " +
           kWgBase;
}
wstring CmdUninstall(const wstring& id) {
    return L"winget uninstall --id \"" + id + L"\" -e --silent -h " + kWgBase;
}
wstring CmdUpgradeOne(const wstring& id) {
    return L"winget upgrade --id \"" + id + L"\" -e --silent -h " + kWgBase;
}

// ---------------------------------------------------------------- 表格解析

// winget 表格列（list/upgrade 有 Available，search 有 Match）
enum class WgCol { Name, Id, Version, Available, Match, Source };

// winget 表格输出的一行
struct WgRow {
    wstring name;
    wstring id;
    wstring version;
    wstring available;
    wstring source;
};

// 是否进度/汇总行：进度条（█）或 "Found ..." 开头的行
bool IsNoiseLine(const wstring& ln) {
    if (ln.find(L'\u2588') != wstring::npos) return true;
    if (ln.rfind(L"Found ", 0) == 0) return true;
    return false;
}

// 解析 winget 的表格输出：
//  * 表头行按 Name/Id/Version/Available/Match/Source 的出现顺序定位列起点
//    （下一行须为短横线，防止把含这些单词的数据行误判为表头）
//  * 数据行按「显示宽度」切列：winget 按显示宽度填充（中文记 2 列），
//    中文软件名（如「Windows 终端」）也不会把后面的列带偏
//  * 自然过滤：短横线行、进度行、以及 id 列为空的汇总行（如 "7 upgrades available."）
//  * 找不到表头时退化为按空白切分，取第一个「含点且有字母」的记号为 id
vector<WgRow> ParseWingetTable(const wstring& text) {
    vector<WgRow> rows;
    vector<wstring> lines = SplitLines(text);

    // ---- 定位表头行 ----
    struct ColMark { int pos; WgCol col; };
    vector<ColMark> marks;
    int hdr = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        const wstring& ln = lines[i];
        struct Cand { const wchar_t* name; WgCol col; };
        static const Cand kCands[] = {
            { L"Name",      WgCol::Name },
            { L"Id",        WgCol::Id },
            { L"Version",   WgCol::Version },
            { L"Available", WgCol::Available },
            { L"Match",     WgCol::Match },
            { L"Source",    WgCol::Source },
        };
        vector<ColMark> found;
        bool ordered = true;
        int last = -1;
        for (const auto& c : kCands) {
            size_t p = ln.find(c.name);
            if (p == wstring::npos) continue;
            if ((int)p <= last) { ordered = false; break; }
            last = (int)p;
            found.push_back({ (int)p, c.col });
        }
        if (!ordered || found.size() < 3) continue;
        bool hasName = false, hasId = false, hasVer = false;
        for (auto& m : found) {
            if (m.col == WgCol::Name)    hasName = true;
            if (m.col == WgCol::Id)      hasId = true;
            if (m.col == WgCol::Version) hasVer = true;
        }
        if (!(hasName && hasId && hasVer)) continue;
        // 下一非空行必须是短横线（winget 表头与数据之间的分隔行）
        bool dash = false;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            wstring t = TrimStr(lines[j]);
            if (t.empty()) continue;
            dash = t.size() >= 8 && t.find_first_not_of(L'-') == wstring::npos;
            break;
        }
        if (!dash) continue;
        hdr = (int)i;
        marks = found;
        break;
    }

    // ---- 数据行 ----
    for (size_t i = (hdr >= 0 ? (size_t)hdr + 1 : 0); i < lines.size(); ++i) {
        const wstring& ln = lines[i];
        wstring t = TrimStr(ln);
        if (t.empty()) continue;
        if (IsNoiseLine(ln)) continue;
        if (t.find_first_not_of(L'-') == wstring::npos) continue;   // 短横线分隔行

        WgRow r;
        if (hdr >= 0) {
            // 显示宽度 → 字符索引 映射
            vector<int> cum;
            cum.reserve(ln.size() + 1);
            cum.push_back(0);
            for (wchar_t ch : ln) cum.push_back(cum.back() + DispWidth(ch));
            auto cut = [&](int disp) -> int {
                for (size_t k = 0; k + 1 < cum.size(); ++k)
                    if (cum[k] >= disp) return (int)k;
                return (int)ln.size();
            };
            for (size_t c = 0; c < marks.size(); ++c) {
                int from = marks[c].pos;
                // 最后一列延伸到行尾（注意：行尾边界是字符数，不是显示位置，
                // 中文行的显示宽度大于字符数，不能也过 cut()）
                int s = cut(from);
                int e = (c + 1 < marks.size()) ? cut(marks[c + 1].pos) : (int)ln.size();
                if (s >= (int)ln.size() || e <= s) continue;
                wstring v = TrimStr(ln.substr(s, e - s));
                switch (marks[c].col) {
                case WgCol::Name:      r.name = v;      break;
                case WgCol::Id:        r.id = v;        break;
                case WgCol::Version:   r.version = v;   break;
                case WgCol::Available: r.available = v; break;
                case WgCol::Match:     break;           // 搜索匹配说明，不用
                case WgCol::Source:    r.source = v;    break;
                }
            }
            // id 为空 = 汇总行（如 "7 upgrades available."），自然滤除
            if (r.id.empty()) continue;
        } else {
            // 无表头：按空白切分，取第一个「含点且有字母、首尾不是点」的
            // 记号作为 id（可挡住 "No package found matching input criteria."）
            vector<wstring> tk;
            size_t p = 0;
            while (p < ln.size()) {
                while (p < ln.size() && (ln[p] == L' ' || ln[p] == L'\t')) ++p;
                size_t q = p;
                while (q < ln.size() && ln[q] != L' ' && ln[q] != L'\t') ++q;
                if (q > p) tk.push_back(ln.substr(p, q - p));
                p = q;
            }
            int idIdx = -1;
            for (size_t k = 0; k < tk.size(); ++k) {
                const wstring& s = tk[k];
                size_t dot = s.find(L'.');
                if (dot == wstring::npos || dot == 0 || dot + 1 >= s.size()) continue;
                if (s.front() == L'.' || s.back() == L'.') continue;
                bool alpha = false;
                for (wchar_t ch : s) if (iswalpha(ch)) { alpha = true; break; }
                if (!alpha) continue;
                idIdx = (int)k;
                break;
            }
            if (idIdx < 0) continue;
            wstring name;
            for (int k = 0; k < idIdx; ++k) name += (k ? L" " : L"") + tk[k];
            r.name = name;
            r.id = tk[idIdx];
            if (idIdx + 1 < (int)tk.size()) r.version = tk[idIdx + 1];
            r.source = tk.back();
        }
        rows.push_back(r);
    }
    return rows;
}

// ---------------------------------------------------------------- 批量执行

// 一条待操作的软件（显示名 + winget ID）
struct SoftRef {
    wstring name;
    wstring id;
};

// 批量执行 winget 动作（安装/卸载/升级）：逐条串行，前后记日志，
// 循环内检查取消。返回成功数；失败项 ID 追加到 failedIds（顿号分隔）。
int WingetBatch(ModuleContext& ctx, const vector<SoftRef>& items,
                const std::function<wstring(const wstring&)>& cmdOf,
                const wstring& verb, wstring* failedIds, bool* cancelled) {
    int ok = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (ctx.Cancelled()) {
            if (cancelled) *cancelled = true;
            break;
        }
        ctx.Report((int)((i * 100) / (items.empty() ? 1 : items.size())),
                   L"[" + std::to_wstring(i + 1) + L"/" + std::to_wstring(items.size()) +
                       L"] " + verb + L" " + items[i].name);
        ctx.Log(L"▸ " + verb + L" " + items[i].name + L"  (" + items[i].id + L")");
        wstring o;
        int code = RunWingetCapture(cmdOf(items[i].id), kWingetTimeoutMs, &o);
        if (code == 0) {
            ++ok;
            ctx.Log(L"  ✓ 完成");
        } else {
            if (failedIds) *failedIds += items[i].id + L"、";
            ctx.Log(L"  ✗ 失败（退出码 " + std::to_wstring(code) +
                    (code == -2 ? L"，已超时结束" : L"）"));
            wstring tail = FirstLines(o, 2);
            if (!tail.empty()) ctx.Log(L"    " + tail);
        }
    }
    return ok;
}

// 探测 winget；不可用时填充友好提示（引导安装「应用安装程序」）并返回 false
bool EnsureWinget(ModuleContext& ctx, ModuleResult& out) {
    wstring ver;
    if (ProbeWinget(&ver)) {
        ctx.Log(L"✓ winget " + ver + L"（Windows 包管理器可用）");
        return true;
    }
    out.summary = L"未检测到 winget（Windows 包管理器），软件管家暂不可用";
    out.hint = wstring(T(L"winget 随「应用安装程序」分发：请在 Microsoft Store 搜索安装"))
               + T(L"「应用安装程序」，或将系统升级到 Windows 10 1709+ / Windows 11 后重试");
    out.canClean = false;
    ctx.Log(L"✗ 未检测到 winget，本次操作已取消（不弹窗、不崩溃）");
    return false;
}

// ---------------------------------------------------------------- 装机榜单

// 榜单级别：2 = 必备 / 1 = 推荐 / 0 = 可选
struct CatalogItem {
    const wchar_t* id;         // winget ID（本机 winget show --id 实测可解析）
    const wchar_t* name;       // 显示名
    const wchar_t* category;   // 分类（与 soft.rec.* 模块一一对应）
    const wchar_t* desc;       // 一句简介
    int level;                 // 2=必备 1=推荐 0=可选
};

// 装机必备榜单（63 款免费软件，全部经本机 winget 实测可解析）
static const CatalogItem kCatalog[] = {
    // ================= 浏览器上网 =================
    { L"Google.Chrome",                    L"Chrome 浏览器",   L"浏览器",   L"谷歌官方浏览器，扩展生态最全",   2 },
    { L"Mozilla.Firefox",                  L"Firefox 浏览器",  L"浏览器",   L"注重隐私的独立内核浏览器",       1 },
    { L"Microsoft.Edge",                   L"Edge 浏览器",     L"浏览器",   L"Windows 原生，兼容与性能兼备",    1 },
    { L"Vivaldi.Vivaldi",                  L"Vivaldi 浏览器",  L"浏览器",   L"高度可定制的进阶浏览器",         0 },
    { L"Opera.Opera",                      L"Opera 浏览器",    L"浏览器",   L"内置免费 VPN 与侧边栏",           0 },

    // ================= 压缩与文件管理 =================
    { L"7zip.7zip",                        L"7-Zip",           L"压缩文件", L"开源压缩工具，格式支持最全",     2 },
    { L"M2Team.NanaZip",                   L"NanaZip",         L"压缩文件", L"7-Zip 的 Win11 现代版",          0 },
    { L"Bandisoft.Bandizip",               L"Bandizip",        L"压缩文件", L"无广告极速压缩，智能解压",        1 },
    { L"voidtools.Everything",             L"Everything",      L"压缩文件", L"毫秒级全盘文件名搜索",           2 },
    { L"AntibodySoftware.WizTree",         L"WizTree",         L"压缩文件", L"磁盘空间占用可视化分析",         1 },
    { L"WinMerge.WinMerge",                L"WinMerge",        L"压缩文件", L"文件与文件夹差异对比合并",       1 },
    { L"WinSCP.WinSCP",                    L"WinSCP",          L"压缩文件", L"SFTP/FTP 文件传输客户端",        1 },
    { L"Balena.Etcher",                    L"balenaEtcher",    L"压缩文件", L"系统镜像写入 U 盘",              0 },
    { L"Ventoy.Ventoy",                    L"Ventoy",          L"压缩文件", L"多镜像 U 盘启动神器",            0 },
    { L"Rufus.Rufus",                      L"Rufus",           L"压缩文件", L"轻量快速制作启动 U 盘",          0 },

    // ================= 办公与效率 =================
    { L"Kingsoft.WPSOffice",               L"WPS Office",      L"办公效率", L"国产办公套件，兼容 Office 格式",  2 },
    { L"TheDocumentFoundation.LibreOffice", L"LibreOffice",     L"办公效率", L"开源免费办公套件",               1 },
    { L"ONLYOFFICE.DesktopEditors",        L"ONLYOFFICE",      L"办公效率", L"高度兼容 MS Office 文档",         0 },
    { L"SumatraPDF.SumatraPDF",            L"SumatraPDF",      L"办公效率", L"极速轻量 PDF/电子书阅读器",       1 },
    { L"Notepad++.Notepad++",              L"Notepad++",       L"办公效率", L"经典代码与文本编辑器",           2 },
    { L"Obsidian.Obsidian",                L"Obsidian",        L"办公效率", L"本地双链笔记知识库",             1 },
    { L"Microsoft.PowerToys",              L"PowerToys",       L"办公效率", L"微软官方效率增强工具集",         1 },
    { L"Bitwarden.Bitwarden",              L"Bitwarden",       L"办公效率", L"开源跨平台密码管理器",           1 },
    { L"KeePassXCTeam.KeePassXC",          L"KeePassXC",       L"办公效率", L"本地离线密码保险箱",             0 },
    { L"Microsoft.WindowsTerminal",        L"Windows Terminal", L"办公效率", L"现代化终端模拟器",               1 },

    // ================= 开发与运维 =================
    { L"Git.Git",                          L"Git",             L"开发运维", L"分布式版本控制系统",             2 },
    { L"Microsoft.VisualStudioCode",       L"VS Code",         L"开发运维", L"最流行的轻量代码编辑器",         2 },
    { L"Neovim.Neovim",                    L"Neovim",          L"开发运维", L"现代 Vim 编辑器",                0 },
    { L"Python.Python.3.12",               L"Python 3.12",     L"开发运维", L"主流编程语言运行时",             2 },
    { L"OpenJS.NodeJS.LTS",                L"Node.js LTS",     L"开发运维", L"JavaScript 运行时（长期支持）",  1 },
    { L"Docker.DockerDesktop",             L"Docker Desktop",  L"开发运维", L"容器化开发与运行环境",           1 },
    { L"Microsoft.PowerShell",             L"PowerShell",      L"开发运维", L"跨平台命令行与脚本引擎",         1 },
    { L"GitHub.GitHubDesktop",             L"GitHub Desktop",  L"开发运维", L"图形化 Git 客户端",              0 },
    { L"Postman.Postman",                  L"Postman",         L"开发运维", L"API 接口调试工具",                1 },

    // ================= 影音与图像 =================
    { L"VideoLAN.VLC",                     L"VLC 播放器",      L"影音图像", L"全格式开源播放器",               2 },
    { L"CodecGuide.K-LiteCodecPack.Full",  L"K-Lite 解码包",   L"影音图像", L"全能影音解码器合集",             0 },
    { L"OBSProject.OBSStudio",             L"OBS Studio",      L"影音图像", L"直播与录屏神器",                 1 },
    { L"ShareX.ShareX",                    L"ShareX",          L"影音图像", L"截图/录屏/GIF 全能工具",         1 },
    { L"Flameshot.Flameshot",              L"Flameshot",       L"影音图像", L"可标注的截图小工具",             0 },
    { L"GIMP.GIMP",                        L"GIMP",            L"影音图像", L"开源图像编辑器（PS 平替）",      1 },
    { L"Inkscape.Inkscape",                L"Inkscape",        L"影音图像", L"开源矢量绘图（AI 平替）",        0 },
    { L"dotPDN.PaintDotNet",               L"Paint.NET",       L"影音图像", L"轻量级图像编辑",                 0 },
    { L"IrfanSkiljan.IrfanView",           L"IrfanView",       L"影音图像", L"极速图片查看器",                 0 },
    { L"Daum.PotPlayer",                   L"PotPlayer",       L"影音图像", L"全能本地影音播放器",             0 },
    { L"Spotify.Spotify",                  L"Spotify",         L"影音图像", L"正版流媒体音乐",                 0 },

    // ================= 通讯与社交 =================
    { L"Tencent.WeChat",                   L"微信",            L"通讯社交", L"国民级社交通讯工具",             2 },
    { L"Tencent.QQ",                       L"QQ",              L"通讯社交", L"腾讯 QQ 客户端",                 1 },
    { L"Alibaba.DingTalk.Mainland",        L"钉钉",            L"通讯社交", L"企业协同办公平台",               0 },
    { L"ByteDance.Feishu",                 L"飞书",            L"通讯社交", L"字节跳动企业协作套件",           0 },
    { L"Tencent.WeCom",                    L"企业微信",        L"通讯社交", L"办公版微信",                     0 },
    { L"SlackTechnologies.Slack",          L"Slack",           L"通讯社交", L"海外团队协作工具",               0 },
    { L"Discord.Discord",                  L"Discord",         L"通讯社交", L"游戏与社群语音聊天",             0 },
    { L"Telegram.TelegramDesktop",         L"Telegram",        L"通讯社交", L"注重隐私的即时通讯",             0 },
    { L"Zoom.Zoom",                        L"Zoom",            L"通讯社交", L"视频会议软件",                   0 },
    { L"Tencent.TencentMeeting",           L"腾讯会议",        L"通讯社交", L"云视频会议",                     0 },
    { L"NetEase.CloudMusic",               L"网易云音乐",      L"通讯社交", L"流媒体音乐平台",                 0 },

    // ================= 系统与安全 =================
    { L"CPUID.CPU-Z",                      L"CPU-Z",           L"系统安全", L"硬件信息检测",                   1 },
    { L"CrystalDewWorld.CrystalDiskInfo",  L"CrystalDiskInfo", L"系统安全", L"硬盘健康状态监控",               1 },
    { L"CrystalDewWorld.CrystalDiskMark",  L"CrystalDiskMark", L"系统安全", L"硬盘性能测试",                   0 },
    { L"TechPowerUp.GPU-Z",                L"GPU-Z",           L"系统安全", L"显卡参数检测",                   0 },
    { L"IDRIX.VeraCrypt",                  L"VeraCrypt",       L"系统安全", L"开源磁盘加密工具",               0 },
    { L"Malwarebytes.Malwarebytes",        L"Malwarebytes",    L"系统安全", L"恶意软件查杀",                   1 },
    { L"Youqu.ToDesk",                     L"ToDesk",          L"系统安全", L"远程桌面控制",                   0 },
};

// 装机榜单基类：8 个实例（总榜 + 7 个分类）共用，仅过滤条件不同
class CatalogMod : public IModule {
public:
    CatalogMod(const wchar_t* id, const wchar_t* title, const wchar_t* desc,
               const wchar_t* category)
        : id_(id), title_(title), desc_(desc), category_(category) {}

    const wchar_t* Id() const override { return id_; }
    const wchar_t* Group() const override { return L"软件管家"; }
    const wchar_t* Title() const override { return title_; }
    const wchar_t* Description() const override { return desc_; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    // 主操作 = 安装选中（蓝色主按钮，非危险）；还原 = 卸载选中
    const wchar_t* ApplyLabel() const override { return L"安装选中"; }
    const wchar_t* UndoLabel() const override { return L"卸载选中"; }
    const wchar_t* ApplyWarning() const override {
        return L"将通过 winget 对所选软件执行安装 / 卸载（联网下载，大包可能耗时数分钟）。\n\n是否继续？";
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在探测 winget…");
        if (!EnsureWinget(ctx, out)) return;

        // 已安装映射：id → 版本（winget list 失败时退化为全「未安装」）
        installed_.clear();
        ctx.Report(20, L"正在读取已安装软件列表…");
        wstring listOut;
        int code = RunWingetCapture(CmdList(), kWingetTimeoutMs, &listOut);
        if (code == 0) {
            for (auto& r : ParseWingetTable(listOut))
                if (!r.id.empty()) installed_.emplace(r.id, r.version);
            ctx.Log(L"✓ 已安装列表读取完成（" + std::to_wstring(installed_.size()) + L" 条）");
        } else {
            ctx.Log(L"⚠ winget list 失败（退出码 " + std::to_wstring(code) +
                    L"），已安装状态可能不准确");
        }

        items_.clear();
        out.rows.clear();
        int nInstalled = 0;
        const int total = (int)(sizeof(kCatalog) / sizeof(kCatalog[0]));
        for (int i = 0; i < total; ++i) {
            const CatalogItem& it = kCatalog[i];
            if (category_ && wcscmp(it.category, category_) != 0) continue;
            auto found = installed_.find(it.id);
            bool inst = found != installed_.end();
            ModuleRow r;
            r.c1 = it.name;
            r.c2 = inst ? found->second : L"未安装";
            r.c3 = it.category;
            r.c4 = wstring(it.id) + L"  " +
                   (it.level == 2 ? L"【必备】" : it.level == 1 ? L"【推荐】" : L"【可选】") +
                   it.desc;
            r.tag = (int)items_.size();   // 记录内部下标，排序后勾选仍可回溯
            r.sortKey = inst ? 0 : 1;     // 未安装排在前，方便一键装机
            if (inst) ++nInstalled;
            items_.push_back(i);
            out.rows.push_back(r);
        }
        std::stable_sort(out.rows.begin(), out.rows.end(),
                         [](const ModuleRow& a, const ModuleRow& b) {
                             return a.sortKey > b.sortKey;
                         });

        wchar_t buf[256];
        swprintf_s(buf, L"共 %zu 款 · 已装 %d · 未装 %zu",
                   items_.size(), nInstalled, items_.size() - (size_t)nInstalled);
        out.summary = buf;
        out.hint = wstring(T(L"勾选后点「安装选中」；不勾选 = 安装本榜全部未安装软件。"))
                   + T(L"卸载请勾选后点「卸载选中」");
        out.canClean = !items_.empty();
    }

    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    // 安装选中（未勾选 = 本榜全部未安装，全家桶一键装机）
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        vector<SoftRef> targets;
        if (selection_.empty()) {
            for (int k = 0; k < (int)items_.size(); ++k) {
                const CatalogItem& it = kCatalog[items_[k]];
                if (installed_.count(it.id)) {
                    ctx.Log(L"  跳过（已安装）: " + wstring(it.name));
                    continue;
                }
                targets.push_back({ it.name, it.id });
            }
        } else {
            for (int t : selection_) {
                if (t < 0 || t >= (int)items_.size()) continue;
                const CatalogItem& it = kCatalog[items_[t]];
                targets.push_back({ it.name, it.id });
            }
        }
        if (targets.empty()) {
            out.summary = L"没有需要安装的软件（已全部安装或未勾选）";
            out.hint = T(L"不勾选直接点「安装选中」= 安装本榜全部未安装软件");
            out.canClean = !items_.empty();
            return;
        }
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdInstall, L"安装", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"安装完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();   // 去掉末尾顿号
            out.hint = T(L"失败项：") + failed +
                       T(L"（多为网络中断或安装器需要交互；可重试，或以管理员身份运行）");
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
        out.canClean = !items_.empty();
    }

    bool SupportsUndo() const override { return true; }

    // 卸载选中（未勾选 = 拒绝执行并提示，避免误删）
    void Undo(ModuleContext& ctx, ModuleResult& out) override {
        if (selection_.empty()) {
            out.summary = L"请先勾选要卸载的软件";
            out.hint = T(L"在列表中勾选后点击「卸载选中」；本操作通过 winget uninstall 执行");
            return;
        }
        vector<SoftRef> targets;
        for (int t : selection_) {
            if (t < 0 || t >= (int)items_.size()) continue;
            const CatalogItem& it = kCatalog[items_[t]];
            targets.push_back({ it.name, it.id });
        }
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdUninstall, L"卸载", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"卸载完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();
            out.hint = T(L"失败项：") + failed + T(L"（可重试，或以管理员身份运行）");
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
    }

private:
    const wchar_t* id_;
    const wchar_t* title_;
    const wchar_t* desc_;
    const wchar_t* category_;                 // nullptr = 总榜（全部分类）
    vector<int> items_;                       // 本榜条目在 kCatalog 中的下标
    std::map<wstring, wstring> installed_;    // 已安装 id → 版本
    vector<int> selection_;
};

// ---------------------------------------------------------------- 软件搜索

class SearchMod : public IModule {
public:
    const wchar_t* Id() const override { return L"soft.search"; }
    const wchar_t* Group() const override { return L"软件管家"; }
    const wchar_t* Title() const override { return L"软件搜索"; }
    const wchar_t* Description() const override { return L"winget 全网搜索软件包，勾选一键安装/卸载"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsTarget() const override { return true; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* TargetHint() const override {
        return L"输入软件名称或关键词，如 python / 压缩 / 浏览器";
    }
    const wchar_t* ApplyLabel() const override { return L"安装选中"; }
    const wchar_t* UndoLabel() const override { return L"卸载选中"; }
    const wchar_t* ApplyWarning() const override {
        return L"将通过 winget 对搜索结果执行安装 / 卸载（联网下载，大包可能耗时数分钟）。\n\n是否继续？";
    }

    void SetInput(const wstring& path, const wstring& target) override {
        path_ = path;
        target_ = target;
    }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        keyword_ = TrimStr(target_);
        if (keyword_.empty()) {
            out.summary = L"请输入搜索关键词";
            out.hint = T(L"例如：python / 压缩 / 浏览器 / vlc / 微信");
            out.canClean = false;
            return;
        }
        ctx.Report(0, L"正在探测 winget…");
        if (!EnsureWinget(ctx, out)) return;

        ctx.Report(30, L"正在搜索「" + keyword_ + L"」…");
        wstring o;
        int code = RunWingetCapture(CmdSearch(keyword_), kWingetTimeoutMs, &o);
        items_ = ParseWingetTable(o);
        if (code != 0) {
            ctx.Log(L"⚠ winget search 退出码 " + std::to_wstring(code));
        }
        out.rows.clear();
        for (auto& r : items_) {
            ModuleRow row;
            row.c1 = r.name.empty() ? r.id : r.name;
            row.c2 = r.version.empty() ? L"未知" : r.version;
            row.c3 = r.source.empty() ? L"winget" : r.source;
            row.c4 = r.id;
            row.tag = (int)out.rows.size();
            out.rows.push_back(row);
        }
        if (out.rows.empty()) {
            out.summary = L"未找到与「" + keyword_ + L"」匹配的软件";
            out.hint = T(L"换个关键词试试，如 python / 压缩 / 浏览器 / vlc / 微信");
            out.canClean = false;
            return;
        }
        out.summary = L"找到 " + std::to_wstring(out.rows.size()) + L" 个结果（关键词：" +
                      keyword_ + L"）";
        out.hint = T(L"勾选后点「安装选中」安装，点「卸载选中」卸载；未勾选 = 全部安装");
        out.canClean = true;
    }

    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        vector<SoftRef> targets = PickTargets();
        if (targets.empty()) {
            out.summary = L"没有需要安装的软件";
            return;
        }
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdInstall, L"安装", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"安装完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();
            out.hint = T(L"失败项：") + failed;
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
    }

    bool SupportsUndo() const override { return true; }
    void Undo(ModuleContext& ctx, ModuleResult& out) override {
        if (selection_.empty()) {
            out.summary = L"请先勾选要卸载的软件";
            out.hint = T(L"在搜索结果中勾选后点击「卸载选中」");
            return;
        }
        vector<SoftRef> targets = PickTargets();
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdUninstall, L"卸载", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"卸载完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();
            out.hint = T(L"失败项：") + failed;
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
    }

private:
    // 未勾选 = 全部搜索结果；勾选 = 按 tag 回溯
    vector<SoftRef> PickTargets() const {
        vector<SoftRef> targets;
        if (selection_.empty()) {
            for (auto& r : items_) targets.push_back({ r.name.empty() ? r.id : r.name, r.id });
        } else {
            for (int t : selection_) {
                if (t < 0 || t >= (int)items_.size()) continue;
                const WgRow& r = items_[t];
                targets.push_back({ r.name.empty() ? r.id : r.name, r.id });
            }
        }
        return targets;
    }

    wstring path_, target_, keyword_;
    vector<WgRow> items_;
    vector<int> selection_;
};

// ---------------------------------------------------------------- 已安装软件

class InstalledMod : public IModule {
public:
    const wchar_t* Id() const override { return L"soft.installed"; }
    const wchar_t* Group() const override { return L"软件管家"; }
    const wchar_t* Title() const override { return L"已安装软件"; }
    const wchar_t* Description() const override { return L"winget 管理的已安装软件清单，可勾选卸载"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    const wchar_t* ApplyLabel() const override { return L"卸载选中"; }
    const wchar_t* ApplyWarning() const override {
        return L"将卸载勾选的软件（winget uninstall），软件配置与数据可能丢失。\n\n是否继续？";
    }
    bool ApplyIsDanger() const override { return true; }   // 卸载 = 危险操作，红色按钮

    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在探测 winget…");
        if (!EnsureWinget(ctx, out)) return;

        ctx.Report(30, L"正在读取已安装软件列表…");
        wstring o;
        int code = RunWingetCapture(CmdList(), kWingetTimeoutMs, &o);
        if (code != 0) {
            out.summary = L"读取已安装列表失败（winget 退出码 " + std::to_wstring(code) + L"）";
            out.hint = T(L"请确认 winget 正常：命令行执行 winget list 查看");
            out.canClean = false;
            ctx.Log(L"✗ winget list 失败（退出码 " + std::to_wstring(code) + L"）");
            return;
        }
        items_ = ParseWingetTable(o);
        out.rows.clear();
        int nManaged = 0;
        for (auto& r : items_) {
            ModuleRow row;
            row.c1 = r.name.empty() ? r.id : r.name;
            row.c2 = r.version.empty() ? L"未知" : r.version;
            row.c3 = r.source.empty() ? L"未知" : r.source;
            row.c4 = r.id;   // MSIX/ARP 行的 ID 原样展示（系统标识，属正常）
            row.tag = (int)out.rows.size();
            if (r.source == L"winget") ++nManaged;
            out.rows.push_back(row);
        }
        out.summary = L"已安装 " + std::to_wstring(items_.size()) + L" 个（winget 可管理 " +
                      std::to_wstring(nManaged) + L" 个）";
        out.hint = T(L"双击行可查看详情；勾选后点「卸载选中」卸载");
        out.canClean = !items_.empty();
    }

    // 卸载勾选项（未勾选 = 拒绝并提示，防止误删）
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        if (selection_.empty()) {
            out.summary = L"请先勾选要卸载的软件";
            out.hint = T(L"在列表中勾选后点击「卸载选中」");
            return;
        }
        vector<SoftRef> targets;
        for (int t : selection_) {
            if (t < 0 || t >= (int)items_.size()) continue;
            const WgRow& r = items_[t];
            targets.push_back({ r.name.empty() ? r.id : r.name, r.id });
        }
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdUninstall, L"卸载", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"卸载完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();
            out.hint = T(L"失败项：") + failed + T(L"（可重试，或以管理员身份运行）");
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
    }

    bool SupportsUndo() const override { return false; }

private:
    vector<WgRow> items_;
    vector<int> selection_;
};

// ---------------------------------------------------------------- 软件升级

class UpgradeMod : public IModule {
public:
    const wchar_t* Id() const override { return L"soft.upgrade"; }
    const wchar_t* Group() const override { return L"软件管家"; }
    const wchar_t* Title() const override { return L"软件升级"; }
    const wchar_t* Description() const override { return L"检测可升级软件，勾选一键更新到新版"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    const wchar_t* ApplyLabel() const override { return L"升级选中"; }
    const wchar_t* ApplyWarning() const override {
        return L"将通过 winget 升级所选软件（联网下载，大包可能耗时数分钟）。\n\n是否继续？";
    }

    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在探测 winget…");
        if (!EnsureWinget(ctx, out)) return;

        ctx.Report(30, L"正在检查可升级软件…");
        wstring o;
        int code = RunWingetCapture(CmdUpgrade(), kWingetTimeoutMs, &o);
        if (code != 0) {
            out.summary = L"检查升级失败（winget 退出码 " + std::to_wstring(code) + L"）";
            out.hint = T(L"请确认 winget 正常：命令行执行 winget upgrade 查看");
            out.canClean = false;
            ctx.Log(L"✗ winget upgrade 失败（退出码 " + std::to_wstring(code) + L"）");
            return;
        }
        items_ = ParseWingetTable(o);
        out.rows.clear();
        for (auto& r : items_) {
            ModuleRow row;
            row.c1 = r.name.empty() ? r.id : r.name;
            row.c2 = r.version.empty() ? L"未知" : r.version;
            row.c3 = r.source.empty() ? L"winget" : r.source;
            row.c4 = r.available.empty() ? r.id : (r.id + L" → " + r.available);
            row.tag = (int)out.rows.size();
            out.rows.push_back(row);
        }
        if (out.rows.empty()) {
            out.summary = L"已全部是最新版本，没有可升级的软件";
            out.hint = T(L"winget upgrade 未发现可更新项");
            out.canClean = false;
            return;
        }
        out.summary = std::to_wstring(out.rows.size()) + L" 个软件可升级";
        out.hint = T(L"勾选后点「升级选中」；不勾选 = 升级全部");
        out.canClean = true;
    }

    // 升级选中（未勾选 = 升级全部）
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        vector<SoftRef> targets;
        if (selection_.empty()) {
            for (auto& r : items_) targets.push_back({ r.name.empty() ? r.id : r.name, r.id });
        } else {
            for (int t : selection_) {
                if (t < 0 || t >= (int)items_.size()) continue;
                const WgRow& r = items_[t];
                targets.push_back({ r.name.empty() ? r.id : r.name, r.id });
            }
        }
        if (targets.empty()) {
            out.summary = L"没有需要升级的软件";
            return;
        }
        wstring failed;
        bool cancelled = false;
        int ok = WingetBatch(ctx, targets, CmdUpgradeOne, L"升级", &failed, &cancelled);
        wchar_t buf[256];
        swprintf_s(buf, L"升级完成：成功 %d，失败 %d，共 %zu 项", ok,
                   (int)targets.size() - ok, targets.size());
        out.summary = buf;
        if (!failed.empty()) {
            failed.pop_back();
            out.hint = T(L"失败项：") + failed + T(L"（可重试，或以管理员身份运行）");
        }
        if (cancelled) out.hint = T(L"已被用户中止（Esc）");
    }

    bool SupportsUndo() const override { return false; }

private:
    vector<WgRow> items_;
    vector<int> selection_;
};

// ---------------------------------------------------------------- 注册

// 装机必备 · 推荐总榜（全部分类）
class CatalogRecMod : public CatalogMod {
public:
    CatalogRecMod() : CatalogMod(L"soft.rec", L"装机必备 · 推荐总榜",
                                 L"63 款免费好软件，勾选一键安装 / 卸载", nullptr) {}
};
REGISTER_MODULE(CatalogRecMod);

class CatalogBrowserMod : public CatalogMod {
public:
    CatalogBrowserMod() : CatalogMod(L"soft.rec.browser", L"浏览器上网",
                                     L"主流浏览器推荐，按需勾选一键安装", L"浏览器") {}
};
REGISTER_MODULE(CatalogBrowserMod);

class CatalogFileMod : public CatalogMod {
public:
    CatalogFileMod() : CatalogMod(L"soft.rec.file", L"压缩与文件管理",
                                  L"解压 / 搜索 / 对比 / 启动盘等文件利器", L"压缩文件") {}
};
REGISTER_MODULE(CatalogFileMod);

class CatalogOfficeMod : public CatalogMod {
public:
    CatalogOfficeMod() : CatalogMod(L"soft.rec.office", L"办公与效率",
                                    L"办公套件、笔记、密码管理等效率必备", L"办公效率") {}
};
REGISTER_MODULE(CatalogOfficeMod);

class CatalogDevMod : public CatalogMod {
public:
    CatalogDevMod() : CatalogMod(L"soft.rec.dev", L"开发与运维",
                                 L"Git / 编辑器 / 运行时 / 容器等开发环境", L"开发运维") {}
};
REGISTER_MODULE(CatalogDevMod);

class CatalogMediaMod : public CatalogMod {
public:
    CatalogMediaMod() : CatalogMod(L"soft.rec.media", L"影音与图像",
                                   L"播放 / 直播 / 截图 / 修图等多媒体工具", L"影音图像") {}
};
REGISTER_MODULE(CatalogMediaMod);

class CatalogChatMod : public CatalogMod {
public:
    CatalogChatMod() : CatalogMod(L"soft.rec.chat", L"通讯与社交",
                                  L"微信 / QQ / 钉钉 / 会议等通讯软件", L"通讯社交") {}
};
REGISTER_MODULE(CatalogChatMod);

class CatalogSysMod : public CatalogMod {
public:
    CatalogSysMod() : CatalogMod(L"soft.rec.sys", L"系统与安全",
                                 L"硬件检测 / 磁盘监控 / 加密 / 远程桌面", L"系统安全") {}
};
REGISTER_MODULE(CatalogSysMod);

REGISTER_MODULE(SearchMod);
REGISTER_MODULE(InstalledMod);
REGISTER_MODULE(UpgradeMod);

} // namespace
} // namespace mz
