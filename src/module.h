// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) - 可扩展模块框架
//  ---------------------------------------------------------------------------
//  设计目标：新增一个功能 = 新增一个 .cpp 文件 + REGISTER_MODULE，不碰 GUI/CLI。
//
//  借鉴项目：
//    * hellzerg/optimizer        —— 模块化分组 + 按类别组织界面
//    * Angry IP Scanner          —— 局域网主机发现
//    * Nmap                      —— 端口扫描
//    * TrafficMonitor            —— 网络状态展示
//    * BleachBit / Winapp2       —— 规则化清理
// ============================================================================
#pragma once

#include "common.h"
#include "lang.h"   // v2.18 i18n：模块层 hint/label/warning 文案经 T() 翻译

namespace mz {

// ---------------------------------------------------------------- 模块上下文

// 模块运行时能用的最小环境（由 GUI/CLI 注入，模块不直接依赖界面）
struct ModuleContext {
    HWND hwnd = nullptr;          // 可为空（CLI 模式）
    LogSink log;                  // 输出日志
    ProgressFn progress;          // 进度（p<0 表示不定进度）
    CancelFn cancel;              // 返回 true 表示用户要求中止

    void Log(const wstring& s) const { if (log.onLine) log.onLine(s); }
    void Report(int pct, const wstring& s) const { if (progress) progress(pct, s); }
    bool Cancelled() const { return cancel && cancel(); }
};

// ---------------------------------------------------------------- 结果模型

// 列表一行：四列（与界面表格列一一对应）
struct ModuleRow {
    wstring c1;                   // 名称
    wstring c2;                   // 数值（大小/端口/IP…）
    wstring c3;                   // 分类/类型
    wstring c4;                   // 详情/路径
    unsigned long long sortKey = 0;   // 排序用（越大越靠前）
    bool danger = false;          // 高风险行（红色标记）
    int tag = -1;                 // 模块私有索引（如 Tweaks 的特性下标），排序后仍可回溯
};

struct ModuleResult {
    vector<ModuleRow> rows;
    wstring summary;              // 一行汇总，显示在列表上方
    wstring hint;                 // 补充提示（可为空）
    bool canClean = false;        // 是否允许「执行清理/操作」按钮
};

// ---------------------------------------------------------------- 模块接口

// 扩展动作（v2.8）：服务/计划任务等需要 >2 个操作按钮的模块。
// 返回非空 Actions() 时界面复用按钮位：动作 0 → 「执行清理」位、
// 动作 1 → 「还原选中」位、动作 2+ → 「更多 ▾」弹出菜单。
struct ModuleAction {
    const wchar_t* label = nullptr;    // 按钮文案，如「停止选中服务」
    const wchar_t* warning = nullptr;  // 确认框正文；nullptr = 通用文案
    bool danger = false;               // 危险态（红色按钮/确认框/菜单项）
    wchar_t glyph = 0;                 // MDL2 字形（0 = 默认字形）
};

// 扩展输入字段（v2.12 R3）：需要 >2 个结构化输入的模块（如加域的
// 域管用户名/密码/计算机名前缀）通过 Fields() 声明，界面渲染为带标签的
// 输入行（最多 4 个；secret=true 的字段密码掩码显示，且持久化时经 DPAPI 加密）。
struct ModuleField {
    const wchar_t* label = nullptr;    // 字段标签，如「域管理员用户名」
    const wchar_t* hint = nullptr;     // 占位提示
    bool secret = false;               // 密码框 + 加密存储
};

// 模块类型决定界面上的按钮组合
enum class ModuleKind {
    Inventory,   // 只读盘点（扫描 → 看结果）      按钮：开始扫描
    Cleaner,     // 可清理（扫描 → 确认 → 删除）    按钮：开始扫描 / 执行清理
    Action,      // 一次性动作（如刷新 DNS）        按钮：执行
    Tweaks,      // 开关型（列表 + 勾选应用/还原）  按钮：应用选中 / 还原选中
    Console,     // 输出型（如 hosts 编辑）         按钮：执行
};

class IModule {
public:
    virtual ~IModule() = default;

    // ---- 元信息（界面/CLI 都从这里取）----
    virtual const wchar_t* Id() const = 0;            // 稳定 ID，如 L"net.ping"
    virtual const wchar_t* Group() const = 0;         // 分组，如 L"网络诊断"
    virtual const wchar_t* Title() const = 0;         // 显示名
    virtual const wchar_t* Description() const = 0;   // 副标题说明
    virtual ModuleKind Kind() const = 0;

    // 可选能力
    virtual bool NeedsPath() const { return false; }   // 需要路径输入框
    virtual bool NeedsTarget() const { return false; } // 需要目标(IP/域名/端口)输入框
    virtual bool NeedsElevation() const { return false; } // 需要管理员权限才完整

    // 输入框占位提示（可选）：返回nullptr用通用文案
    virtual const wchar_t* PathHint() const { return nullptr; }
    virtual const wchar_t* TargetHint() const { return nullptr; }

    // 可选：自定义主按钮与确认框（向后兼容，默认行为不变）----
    // 「开始扫描 / 执行」主按钮（btnRun）自定义文案；nullptr = 默认
    virtual const wchar_t* RunLabel() const { return nullptr; }
    // 「应用选中 / 执行清理」按钮自定义文案；nullptr = 默认
    // （Tweaks「应用选中」/ 其他「执行清理」）
    virtual const wchar_t* ApplyLabel() const { return nullptr; }
    // 「还原选中」按钮自定义文案；nullptr = 默认「还原选中」
    virtual const wchar_t* UndoLabel() const { return nullptr; }
    // 确认框正文（替换通用文案，需自行包含「是否继续」问句）；nullptr = 默认文案
    virtual const wchar_t* ApplyWarning() const { return nullptr; }
    // 确认框与主按钮按危险态（红）渲染
    virtual bool ApplyIsDanger() const { return false; }

    // ---- 扩展动作（v2.8，全部带默认实现，存量模块零改动）----
    // 声明本模块支持的扩展动作（>=1 个时启用；>=3 个时出现「更多 ▾」菜单）
    virtual vector<ModuleAction> Actions() const { return {}; }
    // 执行第 actionIndex 个扩展动作（在工作线程上调用；界面保证先弹确认框）
    virtual void RunAction(ModuleContext&, int actionIndex, ModuleResult&) {}
    // 动态确认框正文：根据动作下标与选中项 tag（选中行 ModuleRow.tag，
    // 排序安全）生成详细警告文本（如列出将结束的进程名）。
    // 返回空串时回落 ModuleAction.warning / 通用文案。
    virtual wstring ActionWarning(int actionIndex, const vector<int>& selectionTags) const {
        (void)actionIndex; (void)selectionTags;
        return L"";
    }
    // 扩展动作完成后自动重扫（对齐 Tweaks 应用/还原后的自动刷新行为）
    virtual bool RefreshAfterAction() const { return false; }

    // 输入注入：界面/CLI 在执行前调用，把用户输入的 path/target 交给模块。
    // 不需要输入的模块可忽略。
    virtual void SetInput(const wstring& path, const wstring& target) {};

    // 扩展字段注入（v2.12 R3）：与 Fields() 一一对应，按声明顺序给出用户输入。
    virtual vector<ModuleField> Fields() const { return {}; }
    virtual void SetFields(const vector<wstring>& values) { (void)values; }

    // 勾选注入（Tweaks 用）：界面把列表选中行索引交给模块；默认忽略
    virtual void SetSelection(const vector<int>& rows) {}

    // 执行：填充 out
    virtual void Run(ModuleContext& ctx, ModuleResult& out) = 0;

    // 清理/应用：仅 Cleaner / Tweaks 类需要实现
    virtual void Apply(ModuleContext& ctx, ModuleResult& out) {}

    // 还原（Tweaks 用）：勾选「应用选中」的逆操作。默认不支持。
    virtual bool SupportsUndo() const { return false; }
    virtual void Undo(ModuleContext& ctx, ModuleResult& out) {}
};

// ---------------------------------------------------------------- 模块注册表

class ModuleRegistry {
public:
    static ModuleRegistry& Instance();

    void Register(IModule* m);
    const vector<IModule*>& All() const { return modules_; }
    IModule* Find(const wstring& id) const;

    // 按 Group() 分组，保持注册顺序
    struct GroupEntry { wstring group; vector<IModule*> mods; };
    vector<GroupEntry> Grouped() const;

private:
    vector<IModule*> modules_;
};

// 静态注册辅助：在 .cpp 里写一行 REGISTER_MODULE(YourClass) 即可
struct ModuleAutoReg {
    explicit ModuleAutoReg(IModule* m) { ModuleRegistry::Instance().Register(m); }
};

#define REGISTER_MODULE(Cls)                       \
    static Cls g_mod_##Cls;                        \
    static ::mz::ModuleAutoReg g_reg_##Cls(&g_mod_##Cls)

} // namespace mz
