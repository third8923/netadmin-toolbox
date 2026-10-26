// ============================================================================
//  清理与分析模块 —— 把原有引擎包装成标准模块
//  复用 cleaner.h / winapp2.h / debloat.h / simhash.h 的实现
// ============================================================================
#include "../module.h"
#include "../cleaner.h"
#include "../cleanresult.h"
#include "../winapp2.h"
#include "../debloat.h"
#include "../simhash.h"
#include "winapp2_builtin.h"   // 来自 data/ 目录（-I../data）
#include <shellapi.h>
#include <winsvc.h>
#include <algorithm>

namespace mz {
namespace {

// 公共：把 ScanItem 转成表格行
static ModuleRow ToRow(const ScanItem& it) {
    ModuleRow r;
    r.c1 = it.name;
    r.c2 = FormatSize(it.size);
    r.c3 = it.category;
    r.c4 = it.reportOnly ? (L"[仅统计] " + (it.hint.empty() ? it.path : it.hint)) : it.path;
    if (it.partial) r.c2 += L"+";
    r.sortKey = it.size;
    r.danger = it.reportOnly;
    return r;
}

static void FillCommon(ModuleResult& out, const ScanOutcome& o) {
    unsigned long long cleanable = 0, reportOnly = 0;
    int nClean = 0, nReport = 0;
    for (auto& it : o.items) {
        if (it.reportOnly) { reportOnly += it.size; ++nReport; }
        else { cleanable += it.size; ++nClean; }
    }
    for (auto& it : o.items) out.rows.push_back(ToRow(it));
    std::sort(out.rows.begin(), out.rows.end(),
              [](const ModuleRow& a, const ModuleRow& b) { return a.sortKey > b.sortKey; });

    wchar_t buf[512];
    swprintf_s(buf, L"共 %zu 项　可清理 %d 项 / %s", o.items.size(), nClean,
               FormatSize(cleanable).c_str());
    out.summary = buf;
    if (nReport > 0) {
        swprintf_s(buf, L"　仅统计 %d 项 / %s（系统关键项，本程序不会删除）", nReport,
                   FormatSize(reportOnly).c_str());
        out.summary += buf;
    }
    // 扫描附加说明（父子去重说明 / 多用户覆盖范围）
    if (!o.note.empty()) out.summary += L"（" + o.note + L"）";
    out.canClean = (nClean > 0);
}

// ------------------------------------------------------- 清理执行公共设施

// 清理动作统计（逐项层面）
struct CleanApplyOutcome {
    int ok = 0;                          // 已处理项数
    int skip = 0;                        // 仅统计跳过项数
    int fail = 0;                        // 处理失败项数
};

// 回收站项：走系统回收站语义清空（SHEmptyRecycleBinW）。
// 返回 0 = 成功（含回收站本就为空）；非 0 = HRESULT 错误。
static DWORD EmptyRecycleBinFor(const ScanItem& it) {
    // it.path 形如 "C:\$Recycle.Bin"，取盘根 "C:\" 作为卷标识
    wstring root = (it.path.size() >= 3) ? it.path.substr(0, 3) : it.path;
    HRESULT hr = SHEmptyRecycleBinW(nullptr, root.c_str(),
        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    if (SUCCEEDED(hr)) return 0;
    // 回收站已空时部分系统返回 E_UNEXPECTED / 文件不存在的 HRESULT，视为成功
    if (hr == E_UNEXPECTED || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return 0;
    return (DWORD)hr;
}

// 路径包含判断（大小写不敏感）
static bool PathContains(const wstring& p, const wchar_t* key) {
    wstring low = p;
    for (auto& c : low) c = towlower(c);
    wstring k = key;
    for (auto& c : k) c = towlower(c);
    return low.find(k) != wstring::npos;
}

// 统一「逐项清理」实现（v2.8）：
//   回收站项   → SHEmptyRecycleBinW（不再暴力删 $Recycle.Bin 目录内容）
//   仅统计项   → 跳过并写日志
//   其余目录项 → CleanDirectoryContentsEx（逐文件智能删除 L1~L4 + 危险路径终检，
//                真实释放量写入 report.freedBytes，失败原因逐条进 report）
static void ApplyCleanItems(ModuleContext& ctx, vector<ScanItem>& items,
                            CleanReport& report, int* rebootBudget,
                            CleanApplyOutcome* o) {
    for (auto& it : items) {
        if (ctx.Cancelled()) break;
        if (it.reportOnly) {
            ++o->skip;
            ctx.Log(L"  跳过（仅统计）: " + it.name +
                    (it.hint.empty() ? wstring() : L"  " + it.hint));
            continue;
        }
        if (it.useEmptyRecycleBin) {
            DWORD hr = EmptyRecycleBinFor(it);
            if (hr == 0) {
                ++o->ok;
                report.freedBytes += it.size;
                ctx.Log(L"  已清空回收站: " + it.name);
            } else {
                ++o->fail;
                report.AddFail(it.path, hr);
                ctx.Log(L"  清空回收站失败: " + it.name + L"  " + ErrorTextZh(hr));
            }
            continue;
        }
        CleanDirectoryContentsEx(it.path, ctx.cancel, report, rebootBudget);
        ++o->ok;
        ctx.Log(L"  已清理: " + it.name);
    }
}

// 统一「清理结果」组装：真实释放量 summary + 失败明细行（danger 标红）+ 原因分布 hint
static void BuildCleanResult(ModuleResult& out, const CleanReport& report,
                             const CleanApplyOutcome& o, const wchar_t* verb) {
    wchar_t buf[512];
    swprintf_s(buf, L"%s完成：成功 %d 项，跳过 %d 项，失败 %d 项，释放 %s",
               verb, o.ok, o.skip, o.fail, FormatSize(report.freedBytes).c_str());
    out.summary = buf;
    if (report.rebootPending > 0) {
        swprintf_s(buf, L"　其中 %d 个文件将在重启后自动删除（被占用，已占位删除）",
                   report.rebootPending);
        out.summary += buf;
    }
    for (auto& f : report.fails) {
        ModuleRow r;
        size_t slash = f.path.find_last_of(L'\\');
        r.c1 = (slash == wstring::npos) ? f.path : f.path.substr(slash + 1);
        r.c2 = L"失败";
        r.c3 = ErrorTextZh(f.code);
        r.c4 = f.path;
        r.sortKey = 0;
        r.danger = true;
        out.rows.push_back(r);
    }
    if (report.fileFail > 0) {
        out.hint = L"失败原因分布：" + report.FailSummary();
        if (!IsElevated())
            out.hint += L"（部分文件需要管理员权限，请以管理员身份运行后重试）";
    }
    out.canClean = false;   // 清理已执行，避免再次点「清理」重复跑
}

// ---- L5 服务联动：更新缓存清理前停服务、清完恢复 ----

static bool StopServiceByName(const wchar_t* name, bool* wasRunning) {
    *wasRunning = false;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS st{};
    if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
        *wasRunning = true;
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        // 最多等 10 秒进入 STOPPED
        for (int i = 0; i < 100 && st.dwCurrentState != SERVICE_STOPPED; ++i) {
            Sleep(100);
            if (!QueryServiceStatus(svc, &st)) break;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool StartServiceByName(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED)
            ok = StartServiceW(svc, 0, nullptr) != 0;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

// ---------------------------------------------------------------- 通用扫描模块

class ScanModuleBase : public IModule {
public:
    explicit ScanModuleBase(ScanKind k) : kind_(k) {}
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ScanOptions opt;
        if (NeedsPath()) {
            if (!path_.empty()) opt.rootPath = path_;
            if (!target_.empty()) opt.minSizeMB = _wtoi(target_.c_str());
        }
        ctx.Report(0, L"正在扫描：");
        ctx.Report(0, Title());
        ScanOutcome o = RunScan(kind_, opt, ctx.progress, ctx.cancel);
        lastItems_ = o.items;
        FillCommon(out, o);
    }
    void SetInput(const wstring& path, const wstring& target) override {
        path_ = path;
        target_ = target;
    }

    const wchar_t* PathHint() const override { return L"扫描路径，如 D:\\"; }
    const wchar_t* TargetHint() const override {
        return kind_ == ScanKind::LargeFiles ? L"最小 MB，默认 100" : nullptr;
    }

protected:
    ScanKind kind_;
    wstring path_, target_;
    vector<ScanItem> lastItems_;
};

class BasicCleanMod : public IModule {
public:
    const wchar_t* Id() const override { return L"clean.basic"; }
    const wchar_t* Group() const override { return L"磁盘清理"; }
    const wchar_t* Title() const override { return L"基础清理"; }
    const wchar_t* Description() const override { return L"临时文件、浏览器缓存、回收站、应用缓存等"; }
    ModuleKind Kind() const override { return ModuleKind::Cleaner; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在扫描基础清理项…");
        ScanOutcome o = RunScan(ScanKind::BasicClean, ScanOptions{}, ctx.progress, ctx.cancel);
        lastItems_ = o.items;
        FillCommon(out, o);
    }
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        CleanReport report;
        int rebootBudget = 50;
        CleanApplyOutcome o;
        ApplyCleanItems(ctx, lastItems_, report, &rebootBudget, &o);
        out.rows.clear();
        BuildCleanResult(out, report, o, L"清理");
        lastItems_.clear();
    }
private:
    vector<ScanItem> lastItems_;
};

class SystemCleanMod : public IModule {
public:
    const wchar_t* Id() const override { return L"clean.system"; }
    const wchar_t* Group() const override { return L"磁盘清理"; }
    const wchar_t* Title() const override { return L"系统清理"; }
    const wchar_t* Description() const override { return L"系统日志、更新缓存、WinSxS、休眠/虚拟内存等"; }
    ModuleKind Kind() const override { return ModuleKind::Cleaner; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在扫描系统清理项（较慢，请稍候）…");
        ScanOutcome o = RunScan(ScanKind::SystemClean, ScanOptions{}, ctx.progress, ctx.cancel);
        lastItems_ = o.items;
        FillCommon(out, o);
    }
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        CleanReport report;
        int rebootBudget = 50;
        CleanApplyOutcome o;
        // L5 服务联动：SoftwareDistribution\Download 下的文件常被 wuauserv /
        // cryptsvc 锁定，先停服务再清理，清完按原状态恢复
        bool needService = false;
        for (auto& it : lastItems_) {
            if (!it.reportOnly && PathContains(it.path, L"SoftwareDistribution")) {
                needService = true;
                break;
            }
        }
        bool wuauservWas = false, cryptsvcWas = false;
        if (needService) {
            ctx.Log(L"  停止服务 wuauserv / cryptsvc 以解锁更新缓存…");
            StopServiceByName(L"wuauserv", &wuauservWas);
            StopServiceByName(L"cryptsvc", &cryptsvcWas);
        }
        ApplyCleanItems(ctx, lastItems_, report, &rebootBudget, &o);
        if (needService) {
            ctx.Log(L"  恢复服务原状态…");
            if (cryptsvcWas && !StartServiceByName(L"cryptsvc"))
                ctx.Log(L"  cryptsvc 恢复启动失败（系统稍后会自行拉起）");
            if (wuauservWas && !StartServiceByName(L"wuauserv"))
                ctx.Log(L"  wuauserv 恢复启动失败（Windows 更新时会自行拉起）");
        }
        out.rows.clear();
        BuildCleanResult(out, report, o, L"清理");
        lastItems_.clear();
    }
private:
    vector<ScanItem> lastItems_;
};

class WeChatMod : public IModule {
public:
    const wchar_t* Id() const override { return L"clean.wechat"; }
    const wchar_t* Group() const override { return L"磁盘清理"; }
    const wchar_t* Title() const override { return L"微信清理"; }
    const wchar_t* Description() const override { return L"微信图片/视频/文件缓存（兼容 3.x 与 4.x）"; }
    ModuleKind Kind() const override { return ModuleKind::Cleaner; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在扫描微信缓存…");
        ScanOutcome o = RunScan(ScanKind::WeChatClean, ScanOptions{}, ctx.progress, ctx.cancel);
        lastItems_ = o.items;
        FillCommon(out, o);
    }
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        CleanReport report;
        int rebootBudget = 50;
        CleanApplyOutcome o;
        ApplyCleanItems(ctx, lastItems_, report, &rebootBudget, &o);
        out.rows.clear();
        BuildCleanResult(out, report, o, L"清理");
        lastItems_.clear();
    }
private:
    vector<ScanItem> lastItems_;
};

class QQMod : public IModule {
public:
    const wchar_t* Id() const override { return L"clean.qq"; }
    const wchar_t* Group() const override { return L"磁盘清理"; }
    const wchar_t* Title() const override { return L"QQ 清理"; }
    const wchar_t* Description() const override { return L"QQ / NTQQ 图片、视频、接收文件缓存"; }
    ModuleKind Kind() const override { return ModuleKind::Cleaner; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在扫描 QQ 缓存…");
        ScanOutcome o = RunScan(ScanKind::QQClean, ScanOptions{}, ctx.progress, ctx.cancel);
        lastItems_ = o.items;
        FillCommon(out, o);
    }
    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        CleanReport report;
        int rebootBudget = 50;
        CleanApplyOutcome o;
        ApplyCleanItems(ctx, lastItems_, report, &rebootBudget, &o);
        out.rows.clear();
        BuildCleanResult(out, report, o, L"清理");
        lastItems_.clear();
    }
private:
    vector<ScanItem> lastItems_;
};

// ---- 应用缓存（Winapp2 规则）----
class AppCacheMod : public IModule {
public:
    const wchar_t* Id() const override { return L"clean.appcache"; }
    const wchar_t* Group() const override { return L"磁盘清理"; }
    const wchar_t* Title() const override { return L"应用缓存"; }
    const wchar_t* Description() const override { return L"按 Winapp2 规则清理第三方软件缓存"; }
    ModuleKind Kind() const override { return ModuleKind::Cleaner; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        Wa2RuleSet rs;
        wstring src;
        ctx.Report(0, L"正在加载清理规则…");
        if (!LoadRules(&rs, &src)) {
            out.summary = L"规则集加载失败";
            return;
        }
        ctx.Log(L"规则来源: " + src + L"，共 " + std::to_wstring(rs.entries.size()) + L" 个规则节");
        vector<Wa2ScanItem> items = ScanWa2(rs, ctx.progress, ctx.cancel);
        std::sort(items.begin(), items.end(),
                  [](const Wa2ScanItem& a, const Wa2ScanItem& b) { return a.size > b.size; });

        unsigned long long total = 0;
        for (auto& it : items) {
            total += it.size;
            ModuleRow r;
            r.c1 = it.section;
            r.c2 = FormatSize(it.size);
            r.c3 = L"应用缓存";
            r.c4 = it.pattern;
            r.sortKey = it.size;
            out.rows.push_back(r);
        }
        lastItems_ = items;
        out.summary = L"共 " + std::to_wstring(items.size()) + L" 项，合计 " + FormatSize(total);
        out.canClean = !items.empty();
    }

    void Apply(ModuleContext& ctx, ModuleResult& out) override {
        CleanReport report;
        int rebootBudget = 50;
        unsigned long long freed = 0;
        int ok = 0;
        for (auto& item : lastItems_) {
            if (ctx.Cancelled()) break;
            for (auto& p : item.matchedPaths) {
                if (ctx.Cancelled()) break;
                // 先取大小（删除后无法统计），再走智能删除 L1~L4
                WIN32_FILE_ATTRIBUTE_DATA d{};
                unsigned long long sz = 0;
                if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
                    sz = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
                if (DeleteFileSmart(p, ctx.cancel, report, &rebootBudget)) {
                    ++ok;
                    freed += sz;
                }
            }
            ctx.Log(L"  已处理: " + item.section + L"  " + FormatSize(item.size));
        }
        wchar_t buf[256];
        swprintf_s(buf, L"清理完成：成功 %d 个文件，失败 %d，释放 %s", ok, report.fileFail,
                   FormatSize(freed).c_str());
        out.summary = buf;
        if (report.rebootPending > 0)
            out.summary += L"（含 " + std::to_wstring(report.rebootPending) +
                           L" 个占位删除，重启后生效）";
        out.rows.clear();
        for (auto& f : report.fails) {
            ModuleRow r;
            size_t slash = f.path.find_last_of(L'\\');
            r.c1 = (slash == wstring::npos) ? f.path : f.path.substr(slash + 1);
            r.c2 = L"失败";
            r.c3 = ErrorTextZh(f.code);
            r.c4 = f.path;
            r.sortKey = 0;
            r.danger = true;
            out.rows.push_back(r);
        }
        if (report.fileFail > 0) out.hint = L"失败原因分布：" + report.FailSummary();
        out.canClean = false;
        lastItems_.clear();
    }

private:
    static bool LoadRules(Wa2RuleSet* rs, wstring* src) {
        wstring ext = DefaultWinapp2Path();
        if (!ext.empty()) {
            DWORD a = GetFileAttributesW(ext.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                if (rs->LoadFromFile(ext)) { *src = L"外部规则 " + ext; return true; }
            }
        }
        if (rs->LoadFromMemory((const char*)WINAPP2_BUILTIN, WINAPP2_BUILTIN_SIZE)) {
            *src = L"内置精选规则集";
            return true;
        }
        return false;
    }
    vector<Wa2ScanItem> lastItems_;
};

// ---------------------------------------------------------------- 分析类

#define DEF_ANALYSIS(Cls, ID, GRP, TITLE, DESC, KIND, PATH)          \
    class Cls : public ScanModuleBase {                              \
    public:                                                          \
        Cls() : ScanModuleBase(KIND) {}                              \
        const wchar_t* Id() const override { return ID; }            \
        const wchar_t* Group() const override { return GRP; }        \
        const wchar_t* Title() const override { return TITLE; }      \
        const wchar_t* Description() const override { return DESC; } \
        bool NeedsPath() const override { return PATH; }             \
    };

DEF_ANALYSIS(LargeFileMod, L"scan.large", L"空间分析", L"大文件查找",
             L"按体积查找大文件（可设最小 MB）", ScanKind::LargeFiles, true)
DEF_ANALYSIS(DupFileMod, L"scan.dup", L"空间分析", L"重复文件",
             L"三级过滤：大小 → 首尾指纹 → 全量 MD5", ScanKind::DuplicateFiles, true)
DEF_ANALYSIS(SimilarFileMod, L"scan.similar", L"空间分析", L"相似文件",
             L"文件名归一化 + 同大小（启发式候选）", ScanKind::SimilarFiles, true)
DEF_ANALYSIS(DirSizeMod, L"scan.dirs", L"空间分析", L"目录大小",
             L"统计子目录体积排行", ScanKind::DirSizes, true)
DEF_ANALYSIS(AppUsageMod, L"scan.apps", L"空间分析", L"软件占用",
             L"枚举已安装软件并统计占用", ScanKind::SoftwareUsage, false)

// 相似图片：独立实现（用 dHash）
class SimilarImgMod : public IModule {
public:
    const wchar_t* Id() const override { return L"scan.similarimg"; }
    const wchar_t* Group() const override { return L"空间分析"; }
    const wchar_t* Title() const override { return L"相似图片"; }
    const wchar_t* Description() const override { return L"WIC + dHash 感知哈希，同画面不同尺寸也能找出"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    bool NeedsPath() const override { return true; }
    const wchar_t* PathHint() const override { return L"图片目录，留空 = C:\\Users"; }
    void SetInput(const wstring& path, const wstring&) { path_ = path; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        wstring root = path_.empty() ? L"C:\\Users" : path_;
        ctx.Report(0, L"正在收集图片: " + root);
        vector<wstring> imgs;
        vector<wstring> stack{ root };
        while (!stack.empty() && imgs.size() < 20000) {
            if (ctx.Cancelled()) break;
            wstring d = stack.back(); stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                wstring full = d + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (fd.cFileName[0] == L'$') continue;
                    stack.push_back(full);
                } else {
                    wstring n = fd.cFileName;
                    size_t dot = n.find_last_of(L'.');
                    if (dot == wstring::npos) continue;
                    wstring ext = n.substr(dot);
                    for (auto& c : ext) c = towlower(c);
                    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" ||
                        ext == L".bmp" || ext == L".gif")
                        imgs.push_back(full);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        ctx.Report(30, L"共 " + std::to_wstring(imgs.size()) + L" 张，计算感知哈希…");
        auto groups = GroupSimilarImages(imgs, 8, ctx.progress, ctx.cancel);
        for (size_t i = 0; i < groups.size(); ++i) {
            for (auto& p : groups[i]) {
                ModuleRow r;
                r.c1 = p.substr(p.find_last_of(L'\\') + 1);
                r.c2 = std::to_wstring(groups[i].size()) + L" 张同组";
                r.c3 = L"相似图片";
                r.c4 = p;
                r.sortKey = groups[i].size();
                out.rows.push_back(r);
            }
        }
        out.summary = L"扫描 " + std::to_wstring(imgs.size()) + L" 张图片，发现 " +
                      std::to_wstring(groups.size()) + L" 组相似";
    }

private:
    wstring path_;
};

// ---------------------------------------------------------------- 迁移类

class SoftwareMoveMod : public IModule {
public:
    const wchar_t* Id() const override { return L"tool.move"; }
    const wchar_t* Group() const override { return L"高级工具"; }
    const wchar_t* Title() const override { return L"软件搬家"; }
    const wchar_t* Description() const override { return L"列出可搬家的软件，迁移到其他磁盘（Junction）"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在扫描可搬家软件…");
        ScanOutcome o = RunScan(ScanKind::SoftwareMove, ScanOptions{}, ctx.progress, ctx.cancel);
        for (auto& it : o.items) {
            ModuleRow r;
            r.c1 = it.name;
            r.c2 = FormatSize(it.size);
            r.c3 = it.risk.empty() ? L"低风险" : (L"风险:" + it.risk);
            r.c4 = it.path;
            r.sortKey = it.size;
            r.danger = (!it.risk.empty() && it.risk != L"低");
            out.rows.push_back(r);
        }
        out.summary = L"可搬家软件 " + std::to_wstring(o.items.size()) + L" 个";
        out.hint = L"迁移使用 Junction，原路径保持可用；大型软件（SQL Server/Office）请勿搬";
    }
};

class UserDirMod : public IModule {
public:
    const wchar_t* Id() const override { return L"tool.usermigrate"; }
    const wchar_t* Group() const override { return L"高级工具"; }
    const wchar_t* Title() const override { return L"用户目录迁移"; }
    const wchar_t* Description() const override { return L"将桌面/文档/下载等迁移到其他分区"; }
    ModuleKind Kind() const override { return ModuleKind::Inventory; }
    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在统计用户目录…");
        ScanOutcome o = RunScan(ScanKind::UserDirMigrate, ScanOptions{}, ctx.progress, ctx.cancel);
        for (auto& it : o.items) {
            ModuleRow r;
            r.c1 = it.name;
            r.c2 = FormatSize(it.size);
            r.c3 = it.migratable ? L"可迁移" : L"已在其他盘";
            r.c4 = it.path;
            r.sortKey = it.size;
            out.rows.push_back(r);
        }
        out.summary = L"用户目录 " + std::to_wstring(o.items.size()) + L" 个";
    }
};

// ---------------------------------------------------------------- Debloat

class DebloatMod : public IModule {
public:
    const wchar_t* Id() const override { return L"tweak.winopt"; }
    const wchar_t* Group() const override { return L"系统优化"; }
    const wchar_t* Title() const override { return L"Windows 优化"; }
    const wchar_t* Description() const override { return L"40 项 Win11 隐私/外观/任务栏优化，可还原"; }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        ctx.Report(0, L"正在读取优化项状态…");
        feats_.clear();
        auto groups = FeaturesByCategory();
        for (auto& g : groups) {
            for (auto f : g.second) {
                bool applied = IsFeatureApplied(*f);
                ModuleRow r;
            r.c1 = f->label;
            r.c2 = applied ? L"已应用" : L"未应用";
            r.c3 = f->category;
            r.c4 = f->id;
            r.danger = false;
            r.tag = (int)feats_.size();   // 记录特性在 feats_ 中的下标，排序后仍可回溯
            out.rows.push_back(r);
            feats_.push_back(f);
            }
        }
        out.summary = L"共 " + std::to_wstring(feats_.size()) + L" 项优化";
        out.hint = L"选中要调整的项目后点「应用」；已应用项选中后点「还原」";
        out.canClean = true;
    }

    // 界面把列表选中行索引交给模块；与 Run() 输出行一一对应
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    // 由界面通过 ApplySelection 指定要处理的索引；为空时按全量处理
    void Apply(ModuleContext& ctx, ModuleResult& out) override { ApplyImpl(ctx, out, false); }
    bool SupportsUndo() const override { return true; }
    void Undo(ModuleContext& ctx, ModuleResult& out) override { ApplyImpl(ctx, out, true); }

    void ApplyImpl(ModuleContext& ctx, ModuleResult& out, bool undo) {
        LogSink log;
        log.onLine = [&ctx](const wstring& s) { ctx.Log(s); };
        vector<const DebloatFeature*> feats;
        if (selection_.empty()) {
            feats = feats_;   // 未勾选 = 全量（兼容旧行为）
        } else {
            for (int i : selection_) {
                if (i >= 0 && i < (int)feats_.size()) feats.push_back(feats_[i]);
            }
        }
        BatchOutcome r = ApplyFeatures(feats, undo, log, ctx.progress, ctx.cancel);
        wchar_t buf[256];
        swprintf_s(buf, L"%s完成：成功 %d，失败 %d", undo ? L"还原" : L"应用", r.ok, r.failed);
        out.summary = buf;
        if (feats.empty()) out.summary = L"未选中任何优化项";
        if (r.needExplorer) out.hint = L"部分设置需重启资源管理器才能生效";

        // 明确列出失败项，让用户知道哪些没成功、为何失败
        if (r.failed > 0) {
            wstring names;
            for (auto& id : r.failedIds) {
                for (auto& f : Features()) {
                    if (f.id == id) { names += f.label + L"、"; break; }
                }
            }
            if (!names.empty()) names.pop_back();  // 去掉末尾顿号
            out.hint += (out.hint.empty() ? L"" : L"  ·  ");
            out.hint += L"失败项：" + names;
            if (!IsElevated())
                out.hint += L"（写入 HKLM 或受保护键需要管理员权限，请以管理员身份运行）";
            else
                out.hint += L"（写入失败多为权限/键被锁定，可尝试手动注册表编辑器）";
        }
    }

private:
    vector<const DebloatFeature*> feats_;
    vector<int> selection_;
};

// ---------------------------------------------------------------- 注册

REGISTER_MODULE(BasicCleanMod);
REGISTER_MODULE(SystemCleanMod);
REGISTER_MODULE(WeChatMod);
REGISTER_MODULE(QQMod);
REGISTER_MODULE(AppCacheMod);
REGISTER_MODULE(LargeFileMod);
REGISTER_MODULE(DupFileMod);
REGISTER_MODULE(SimilarFileMod);
REGISTER_MODULE(SimilarImgMod);
REGISTER_MODULE(DirSizeMod);
REGISTER_MODULE(AppUsageMod);
REGISTER_MODULE(SoftwareMoveMod);
REGISTER_MODULE(UserDirMod);
REGISTER_MODULE(DebloatMod);

} // namespace
} // namespace mz
