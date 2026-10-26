# NetAdmin Toolbox v2.8 增量技术设计与开发计划

> 作者：架构师 高见远（Gao）· 2025-10
> 状态：供用户 review 后执行 ｜ 基线：v2.7（含 softmods.cpp 软件管家，QA 中）
> 方法：全部结论基于源码逐行勘察（文件+行号）+ 本机只读实测，未改动任何代码。

---

## 结论速览（给赶时间的读者）

| 需求 | 结论 | 关键发现 |
|---|---|---|
| R1 启动默认管理员 | 可行，改造点 5 处 | manifest 是 `asInvoker`（app.manifest:7），**不能**改 requireAdministrator（会连 CLI 一起强制提权）；防循环用环境变量继承 + 命令行标记双保险 |
| R2 结束进程 | 改造 `sys.process` | 现为纯只读 Inventory（sysmods.cpp:639-726），加 Apply 路径 + 关键进程黑名单即可 |
| R3 服务操作 | 改造 `sys.services` | 枚举只用了 `SC_MANAGER_ENUMERATE_SERVICE`（sysmods.cpp:852），需要扩展动作框架支持 >2 个操作 |
| R4 计划任务 | **选 COM API（ITaskService）** | MinGW 已带 `taskschd.h` + `libtaskschd.a`（实测确认）；schtasks 文本解析多语言下易碎 |
| R5 清理优化 | **发现 1 个高危缺陷 + 5 类"删不掉"根因** | `DriverStore\FileRepository` 被列为可清理目录（cleaner.cpp:401，非 reportOnly）→ 会被整目录清空；`CleanDirectoryContents` 忽略全部失败且返回"删除前大小"冒充释放量（cleaner.cpp:954-981） |
| R6 安全体检 | 需要新的流式进程执行器 | `RunHidden` 固定 120s 超时且不可取消（common.cpp:98），撑不住 sfc/DISM（5~30 分钟） |
| R7 常用管理 | 选定 6 个模块 | 还原点 / 用户账户 / 共享 / 事件日志 / 电源计划 / 更新检查，全部有原生 API 或 MinGW 头文件支撑 |

---

# 一、现状勘察报告

## 1.1 提权链路勘察（R1）

### 1.1.1 现状调用链（实证）

```
wWinMain (main.cpp:10)
 ├─ argc>=2 且非 --gui → mz::RunCli(argc, argv)        // main.cpp:15-36，CLI 分支，全程无提权逻辑
 └─ 否则 → mz::RunAppWindow(hInst, nCmdShow)           // main.cpp:40
```

提权相关代码共 4 处：

| # | 位置 | 现状 | 问题 |
|---|---|---|---|
| 1 | `src/app.manifest:7` | `<requestedExecutionLevel level="asInvoker"/>` | 默认标准用户启动；改 `requireAdministrator` 会让 CLI（脚本自动化场景）也每次弹 UAC，不可取 |
| 2 | `src/common.cpp:37-47` | `IsElevated()`：`AllocateAndInitializeSid` + `CheckTokenMembership` 检查 Administrators 组 | 实现正确，可直接复用 |
| 3 | `src/common.cpp:49-62` | `RelaunchAsAdmin()`：`ShellExecuteExW` + `lpVerb=L"runas"` | ① 不传参数（`sei.lpParameters` 未设置）② 不传工作目录 ③ 无返回细分：UAC 用户取消（`ERROR_CANCELLED=1223`）与其它失败混在一起 ④ 成败只看 `ShellExecuteExW` 返回值 |
| 4 | `src/ui/appwindow.cpp`（控件 ID `ID_BTN_ADMIN=1014`，appwindow.cpp:36） | 「以管理员重启」按钮：确认框 → `RelaunchAsAdmin()` → `DestroyWindow` | 手动提权路径，保留 |

另外 `IsElevated()` 的使用点：sysmods.cpp:723（进程模块提示权限不足）、cleanmods.cpp:502-505（优化失败提示）、appwindow.cpp 权限胶囊区。均只读展示，不受 R1 影响。

**本机实测佐证**：当前 shell 为 Medium 完整性级别（`Mandatory Label\Medium Mandatory Level S-1-16-8192`），即该程序日常以标准用户运行——这正是 R1 要解决的体验问题。

### 1.1.2 R1 改造点清单

| # | 文件 | 改造 |
|---|---|---|
| 1 | main.cpp:40（GUI 分支前） | 启动时 `!IsElevated() && !已尝试过` → 自动 `RelaunchAsAdmin` 并退出当前实例 |
| 2 | common.cpp:49-62 | `RelaunchAsAdmin` 增强：新增 `params`/`workDir` 参数、返回 `ElevateResult{Ok, Cancelled, Failed}` |
| 3 | common.h | 新增 `enum class ElevateResult`、`ElevationAttempted()` 环境变量标记函数 |
| 4 | appwindow.cpp「以管理员重启」按钮 | 复用新版 `RelaunchAsAdmin`；UAC 取消时 Toast 提示而非静默 |
| 5 | cli.cpp | **保持零改动**（CLI 永不自动提权），仅帮助文案加一行说明 |

**防提权死循环设计（关键）**：

- 机制 A（主）：`SetEnvironmentVariableW(L"MZ_ELEV_ATTEMPTED", L"1")` 后再 `ShellExecuteExW`。子进程**继承父进程环境块**，启动后检测到该变量即跳过自动提权 → 天然防循环，无具名对象泄漏。
- 机制 B（兜底）：`lpParameters` 追加 `--elev-attempted`。子进程 `wWinMain` 解析到后剥离该参数并置标记（注意：必须在 CLI/GUI 分流**之前**剥离，否则 `argc>=2` 会误入 CLI 分支，main.cpp:15）。
- UAC 取消：`ShellExecuteExW` 失败且 `GetLastError()==ERROR_CANCELLED(1223)` → 不退出，继续以标准用户运行 GUI，日志写「已按标准用户权限运行」。
- CLI 模式：main.cpp:15-36 分支完全不进入提权逻辑。
- 提权后保留原工作目录与参数：`sei.lpDirectory = 当前 GetCurrentDirectoryW()`；`lpParameters` 前向 `--gui` 之外的用户参数（当前产品 CLI 参数与 GUI 不共享，v2.8 仅前向空参数即可，接口留好）。

## 1.2 清理引擎诊断（R5，本报告核心）

### 1.2.1 扫描覆盖清单与缺口

**clean.basic（cleaner.cpp:252-388 ScanBasic）实际覆盖 21 项**：

| 类别 | 路径 | 行号 | 可清理? |
|---|---|---|---|
| 临时文件 | `%TEMP%` | :263 | ✔ |
| 系统临时 | `%WINDIR%\Temp` | :266-267 | ✔ |
| 预读取 | `%WINDIR%\Prefetch` | :270-271 | ✔ |
| 最近文档 | Roaming\...\Recent | :274-275 | ✔ |
| 回收站 | 各盘 `$Recycle.Bin` | :278-285 | ✔（但见 1.2.2 问题④） |
| 浏览器缓存 ×9 | Chrome/Edge/Code Cache/火狐/Brave/Opera/Vivaldi/WebView2 | :288-302 | ✔ **仅 Default profile** |
| Defender | ProgramData\...\Scans、Definition Updates | :304-310 | ✔ |
| 商店/通知 | Store LocalCache、Notifications | :312-319 | ✔ |
| 搜索索引 | ProgramData\...\Search\Data\Temp | :321-324 | ✔ |
| Installer | `$PatchCache$`；Installer 整目录 msi/msp | :326-336 | 前者✔，后者 reportOnly |
| 媒体/打印 | Media Player Cache、spool\PRINTERS、SystemTemp | :338-347 | ✔ |
| 崩溃/错误 | CrashDumps、WER ReportArchive/ReportQueue、系统 WER | :349-365 | ✔ |
| OneDrive | setup\logs、logs | :367-373 | ✔ |
| 下载文件夹 | Shell Folder {374DE290-...} | :380-385 | reportOnly ✔ |

**clean.system（cleaner.cpp:390-501 ScanSystem）实际覆盖**：

| 路径 | 行号 | 可清理? | 风险评估 |
|---|---|---|---|
| SoftwareDistribution\Download | :396 | ✔ | 正常，但见问题⑤ |
| SoftwareDistribution\DeliveryOptimization | :397 | ✔ | 正常 |
| Logs\CBS | :398 | ✔ | 与 :399 的 Windows\Logs **重复统计** |
| **Windows\Logs（整目录）** | :399 | ✔ | 含 CBS/DISM 子目录重复；日志被占用时删不掉（见 1.2.2） |
| WinSxS\ManifestCache | :400 | ✔ | 正常 |
| **System32\DriverStore\FileRepository** | :401 | ✔ ⚠️ | **高危缺陷：非 reportOnly，Apply 会清空整个驱动仓库！** 删除后设备驱动无法重装/回滚。必须降级 reportOnly |
| FontCache | :402 | ✔ | 文件常被占用 |
| MEMORY.DMP | :403 | ✔ | 正常 |
| 用户 WER 目录 | :404 | ✔ | 与 clean.basic :353-359 重复项 |
| Explorer 缩略图 | :405 | ✔ | iconcache 被 explorer 占用，必删不掉 |
| ServicePackFiles | :420 | ✔ | Win10/11 基本不存在 |
| LogFiles\CBS、Logs\DISM | :425,:428 | ✔ | 与 :398-399 三重重复统计 |
| Windows.old | :434-445 | reportOnly | 正常（DirSizeWithBudget 25s 预算，partial 标记） |
| $WINDOWS.~BT/$WINDOWS.~WS/$GetCurrent | :447-462 | reportOnly | 正常 |
| WinSxS\Temp、WinSxS\Backup | :465,:469 | ✔/reportOnly | 正常 |
| pagefile.sys/hiberfil.sys | :477-487 | reportOnly | 正常（FileSizeByHandle 三级兜底 :120-139） |
| WinSxS 整体 | :490-499 | reportOnly | 正常（25s 预算） |

**缺口清单（对照 Windows 常见垃圾位置逐一核对，本机只读实测）**：

| 垃圾位置 | 现状 | 本机实测 | 建议 |
|---|---|---|---|
| 浏览器多 Profile（Profile 1/2/…）与 GPUCache/Service Worker/CacheStorage | 缺（basic 仅 Default\Cache + Code Cache；GPUCache 等只在 Winapp2 规则里） | Edge Cache 513 文件/120MB | 扩展 basic 表 + 扩充内置 Winapp2 规则 |
| `%WINDIR%\LiveKernelReports` | 缺 | — | 系统清理新增 |
| `%WINDIR%\Panther`（安装日志）、`%WINDIR%\Logs\SIH` | 缺 | — | 系统清理新增 |
| `%WINDIR%\System32\LogFiles\Scm/SMSS/WMI` | 缺（仅 CBS :425） | — | 系统清理新增 |
| 缩略图/图标缓存逐文件 | 目录级 | Explorer 目录 33 文件/10.8MB | 深度扫描逐文件 + 提示需重启 explorer |
| 回收站清空 | 只统计（SHQueryRecycleBinW :178-187），删除走 `$Recycle.Bin` 子目录强删 | 回收站存在 | 改用 `SHEmptyRecycleBinW` |
| 报告项重复统计 | Windows\Logs ⊃ Logs\CBS ⊃ LogFiles\CBS | CBS 133.8MB | 目录级去重（父子包含关系剪枝） |
| `%TEMP%` 逐文件级清理 | 目录级一刀切 | TEMP 355 文件/58.1MB | 深度模式逐文件 + 失败跳过保留其余 |

> 本机取样命令均为只读（`Get-ChildItem` 统计）；本机 Installer 目录 1062.6MB（43 个 msi/msp，已正确 reportOnly）、DriverStore 3GB（**当前会被误清**，最高优先级修复）。

### 1.2.2 删除实现诊断与「删不掉」根因

**实现现状（cleaner.cpp:916-981）**：

- `RemoveTree`（:918-938）：递归 ListDir → 文件清只读属性 → `DeleteFileW`；**目录不清只读属性**；**所有返回值全部忽略**（:926,:929,:934 都不检查）。
- `DeleteSingleFile`（:940-952）：清只读 → `DeleteFileW`，失败有错误码（:950）。
- `CleanDirectoryContents`（:954-981）：先 `DirSize` 存 before（:960）→ 删除 → **返回 before 而非实际释放量**（:980）——即使全部失败也报"释放 XX GB"。
- 上层（cleanmods.cpp:99-115 BasicCleanMod::Apply）：不检查 `err`，无论成败一律 `++ok`（:107）；SystemCleanMod 同（cleanmods.cpp:133-153）。

→ **"删除删不掉、下次扫描还在"的直接根因：失败被吞掉，UI 永远显示成功。**

**「删不掉」根因假设表（按优先级，附只读验证方法）**：

| # | 根因 | 典型错误码 | 典型目标 | 只读验证方法 |
|---|---|---|---|---|
| ① | 文件被进程占用（浏览器在跑、explorer 占 iconcache、OneDrive 同步中） | 32 ERROR_SHARING_VIOLATION / 33 | 浏览器 Cache、FontCache、缩略图 | 只读探测：`CreateFileW(path, GENERIC_READ, 0/*不共享*/, ...)` 成功→无独占句柄；失败 32→被占。配合 `openfiles /query`（需管理员开启 maintain objects list）或 Process Explorer |
| ② | Defender 实时扫描瞬时锁 | 32 | 刚落地的大文件 | 本机实测 `Get-MpPreference.DisableRealtimeMonitoring=False`（实时保护开启）；策略矩阵加退避重试即可 |
| ③ | 权限不足（非管理员 / TrustedInstaller 文件） | 5 ERROR_ACCESS_DENIED | Windows\Temp 内 SYSTEM 文件、Windows\Logs | `icacls <path>`（只读）查看 ACL；R1 落地后自动消除大半 |
| ④ | **目录只读属性未清** | 5 | RemoveTree 目录分支（:924-930 不清属性） | `cmd /c dir /a <dir>` 查看属性位（只读命令） |
| ⑤ | 系统服务占用（wuauserv/cryptsvc 锁 Download；WerSvc 锁 WER） | 32 | SoftwareDistribution\Download | `sc query wuauserv` 只读查询状态；删除前需停服务 |
| ⑥ | 路径超长 >260 字符 | 3/206 ERROR_FILENAME_EXCED_RANGE | 浏览器 Cache\...\f_000001 深层嵌套 | `Get-ChildItem -LiteralPath` 递归测长；修复用 `\\?\` 前缀 |
| ⑦ | 删除挂起（DELETE_PENDING，重启后消失） | — | 刚删过的文件再次扫描仍在 | 注册表 `PendingFileRenameOperations`（本机实测存在 8 项 → `MoveFileEx DELAY_UNTIL_REBOOT` 机制在本机活跃可用，佐证策略 L4 可行） |
| ⑧ | 重解析点/移动硬盘掉线 | 2/21 | — | 代码已跳过 REPARSE_POINT（:925,:968）；移动盘用 `GetDriveTypeW` 已有（:164-175） |

### 1.2.3 修复策略设计

**扫描分层**：

```
快速扫描（默认，秒级）：现规则 + 缺口补齐 + 父子目录去重剪枝，目录级大小
深度扫描（目标框输入 1 触发）：逐文件收集（上限 10000，超限 partial），
          支持逐文件删除回报与「跳过失败文件继续删其余」
```

**删除策略矩阵（新引擎 DeleteFileSmart）**：

| 层级 | 动作 | 适用 |
|---|---|---|
| L1 | `DeleteFileW` 常规删 | 一切起点 |
| L2 | 清**文件+父目录**只读/隐藏/系统属性后重试 | 错误码 5 |
| L3 | 退避重试 ×3（100/300/600ms，`Sleep` 可被 CancelFn 打断） | 错误码 32/33（Defender/瞬时锁） |
| L4 | `MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT)` 占位删除 + 写日志「重启后自动删除」 | 系统进程占用（本机已验证该机制可用） |
| L5 | 服务联动：停 `wuauserv`+`cryptsvc` → 删 → 恢复原启动类型 | 仅 SoftwareDistribution\Download |
| L6 | 仅统计 + 中文指引（现 reportOnly 机制保留） | pagefile/hiberfil/WinSxS/Windows.old/Installer |

**逐文件结果回报**：

```cpp
struct CleanFileResult { wstring path; DWORD code; };        // code=0 成功
struct CleanReport {
    unsigned long long freedBytes = 0;                        // 真实释放
    int fileOk = 0, fileFail = 0, rebootPending = 0;
    vector<CleanFileResult> fails;                            // 上限 200 条进 UI
    static wstring ErrorTextZh(DWORD code);                   // 32→「被其他程序占用」…
};
```

错误码→中文映射表放 common 层（复用 sysmods.cpp:75-91 `Win32Err` 的思路并集中化，该函数目前是 sysmods.cpp 文件内静态，v2.8 提升到 common.cpp 供清理引擎共用）。失败项进日志逐条 + summary 汇总「成功 N 失败 M（原因分布）」。

**回收站**：改用 `SHEmptyRecycleBinW`（shell32，已链接），失败原因同样入 CleanReport。

## 1.3 进程/服务模块现状与可操作化改造点（R2/R3）

**sys.process（sysmods.cpp:639-726）**：`ModuleKind::Inventory`（:645）纯只读。枚举用 Toolhelp32 快照（:649）+ `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`（:674）读内存/时间/路径。行数据 `ModuleRow{c1..c4, sortKey, danger}` 已含全部展示信息，但**没保存 PID**（PID 拼进了 c3 文本 :704）——改造点：利用 `ModuleRow.tag`（module.h:43，模块私有索引，排序后仍可回溯）保存 PID。

- 结束进程：`OpenProcess(PROCESS_TERMINATE)` → `TerminateProcess(h, 1)` → `WaitForSingleObject` 短等待。权限不足时 fallback：`ShellExecuteEx runas taskkill /PID`（仅提示，不自动提权）。**必须内置保护黑名单**：System(4)/System Idle(0)/csrss/wininit/winlogon/smss/services/lsass——黑名单行 `danger=true` 且 Apply 时拒绝并给原因。
- 风险：强杀 svchost 组进程可能连带服务崩溃 → ApplyWarning 文案 + 黑名单拒绝。

**sys.services（sysmods.cpp:842-970）**：`Inventory`（:848）。`OpenSCManagerW(SC_MANAGER_ENUMERATE_SERVICE)`（:852），`OpenServiceW(SERVICE_QUERY_CONFIG)`（:902）。行数据 c1=显示名、c3=启动类型、c4=name+PID+账户（:926-937），tag 同样未用——改造点：tag 存服务名索引。

操作矩阵（全部走 SCM API，新增 `SC_MANAGER_CONNECT` 权限）：

| 操作 | API | 权限 |
|---|---|---|
| 启动 | `StartServiceW` | SERVICE_START |
| 停止 | `ControlService(SERVICE_CONTROL_STOP)` + 轮询状态（≤10s） | SERVICE_STOP |
| 暂停/继续 | `ControlService(PAUSE/CONTINUE)` | SERVICE_PAUSE_CONTINUE |
| 重启 | 停止→启动 组合 | 上述两者 |
| 禁用/启用/手动/自动/延迟 | `ChangeServiceConfigW(dwStartType)` + `ChangeServiceConfig2W(DELAYED_AUTO_START)` | SERVICE_CHANGE_CONFIG |

受保护服务（如 Winmgmt、RpcSs 等 MS 核心服务）失败是常态——错误码中文映射后展示即可，不做黑名单硬编码（SCM 自己会拒绝）。

## 1.4 计划任务两条技术路线对比（R4）

| 维度 | 路线 A：schtasks 子进程 | 路线 B：Task Scheduler COM（ITaskService） |
|---|---|---|
| 实现代价 | 低：复用 `RunHidden`（需放宽 120s）；但 `/query /fo csv /nh` 的 CSV 解析 + OEM 编码 + 多语言列名 | 中：CoInitializeEx + COM 样板 ~200 行；结构化取字段 |
| 数据完整性 | `/query` 缺"下次运行时间"（要 `/v` 冗长模式，且本地化易碎）；`/query /xml` 还要再解析 | IRegisteredTask 直接给 State/LastRunTime/NextRunTime/Definition |
| 操作 | `/run /end /enable /disable /delete /f` 退出码粗糙（不区分"没权限"与"任务不存在"） | `Run/Stop/Enabled/ITaskFolder::DeleteTask` 返回 HRESULT 可归类 |
| 依赖 | 外部 exe（沙箱实测：本开发机策略阻止 schtasks.exe 运行——产品用户机无此限制，但提示了外部依赖脆性） | 系统服务 Schedule（本机实测 Running/Automatic） |
| 链接 | 无 | MinGW 实测已带 `x86_64-w64-mingw32/include/taskschd.h` + `lib/libtaskschd.a`；build.bat 加 `-ltaskschd` 即可 |

**选型：路线 B（COM）**。理由：模块表格需要结构化字段（下次运行时间/状态/作者），schtasks 文本解析在非中文/非英文系统上必然碎；COM 初始化每个模块 Run 内做一次（`CoInitializeEx` + 失败降级提示），不改进程级架构。风险：Home 版 Insider 构建 COM 行为一致（本机即 Home Insider Preview），无兼容性问题。

---

# 二、增量设计

## 2.1 框架扩展点（先做，向后兼容）

**module.h 新增（全部带默认实现，存量 42 模块零改动）**：

```cpp
// 扩展动作（v2.8）：服务/计划任务等需要 >2 个操作按钮的模块
struct ModuleAction {
    const wchar_t* label;     // 按钮文案，如「停止选中服务」
    const wchar_t* warning;   // 确认框正文；nullptr=通用文案
    bool danger;              // 危险色渲染
    wchar_t glyph;            // MDL2 字形（0=默认）
};
virtual vector<ModuleAction> Actions() const { return {}; }
virtual void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) {}
virtual bool RefreshAfterAction() const { return false; }  // 动作后自动重扫（对齐 Tweaks 行为）
```

**appwindow.cpp 改点**：

1. 按钮位复用（当前 6 位：appwindow.cpp:32-39 的 ID_BTN_RUN/CLEAN/UNDO/STOP/ALL/NONE）：`Actions()` 非空的模块，动作 0 → 占用「执行清理」位（文案取 `Actions()[0].label`，走 ApplyLabel 通道），动作 1 → 占用「还原选中」位，动作 2+ → 新增第 7 个按钮位 `ID_BTN_MORE`（「更多 ▾」，`TrackPopupMenu` 弹出，项文案与 danger 灰显逻辑同源 `ModuleAction`）。
2. `StartWork`（现有 Run/Apply/Undo 三态）增加 `actionIndex = -1` 参数；工作线程 lambda 内按 `actionIndex>=0 → RunAction(ctx, idx)` 路由。
3. `kMsgDone` 回 UI 分支：`RefreshAfterAction()==true` 的模块动作完成后自动重扫（复用 Tweaks 自动重扫的现有代码路径）。
4. `PickIcon` kMap（模块 ID→MDL2 字形）补新模块条目。
5. 确认框走现有 `ConfirmDialog`（dialog.h:52-54，支持 danger 红色 okText），`ApplyWarning`/`ModuleAction.warning` 接入。

## 2.2 启动提权

见 1.1.2 改造点清单。新增/修改文件：`common.h`（ElevateResult + 标记函数声明）、`common.cpp`（RelaunchAsAdmin 增强 + MZ_ELEV_ATTEMPTED）、`main.cpp`（GUI 分支自动提权 + `--elev-attempted` 剥离）、`appwindow.cpp`（管理员按钮复用）。CLI（cli.cpp）零改动。

## 2.3 新模块清单表（v2.8 目标 +9 模块，总 51）

| Id | Group | Title | Kind | 按钮语义（ApplyLabel/UndoLabel 或 Actions[]） | 数据来源 API | NeedsElevation |
|---|---|---|---|---|---|---|
| `sys.process`（改造） | 系统管理 | 进程管理 | Tweaks(动作) | 「结束选中进程」/ 无 Undo | Toolhelp32 + TerminateProcess | 否（部分进程需） |
| `sys.services`（改造） | 系统管理 | 服务管理 | Tweaks(动作) | Actions: ①启动选中 ②停止选中 ③更多菜单（暂停/继续/重启/设为自动/自动延迟/手动/禁用） | OpenSCManager/StartService/ControlService/ChangeServiceConfig | 是 |
| `sys.tasks`（新） | 系统管理 | 计划任务 | Tweaks(动作) | Actions: ①立即运行 ②结束选中 ③更多菜单（启用/禁用/删除） | ITaskService/ITaskFolder/IRegisteredTask | 否（系统任务操作需） |
| `sec.scan`（新） | 安全体检 | 一键体检 | Action | 「开始体检」 | 聚合只读检查：Defender(MpPreference 注册表/WMI)、UAC 状态、防火墙 profile、更新滞后天数、sfc 需否（CBS 日志尾部） | 否 |
| `sec.repair`（新） | 安全体检 | 系统修复 | Tweaks(动作) | Actions: ①sfc /scannow ②DISM RestoreHealth ③chkdsk C: /scan ④netsh winsock reset ⑤重置网络栈 ⑥Defender 快速扫描/更新签名（更多菜单：DISM ScanHealth、chkdsk /f 预约） | 新 ProcessRunner 流式执行 | 是 |
| `sys.restore`（新） | 常用管理 | 系统还原点 | Action | 「创建还原点」 | RunHidden `powershell -NoProfile -Command "Checkpoint-Computer -Description ..."` | 是 |
| `sys.users`（新） | 常用管理 | 用户账户 | Tweaks(动作) | Actions: ①启用选中 ②禁用选中 ③更多（重置密码→弹输入框 v2.9） | NetUserEnum/NetUserGetInfo/NetUserSetInfo（`lm.h` 实测在） | 是 |
| `sys.shares`（新） | 常用管理 | 共享管理 | Tweaks(动作) | Actions: ①停止选中共享 | NetShareEnum/NetShareDel（`lmshare.h` 实测在） | 是 |
| `sys.events`（新） | 常用管理 | 事件日志 | Inventory | 只读：近 7 天 Error/Critical 聚合 Top 20 | EvtQuery/EvtRender（`libwevtapi.a` 实测在） | 否 |
| `sys.power`（新） | 常用管理 | 电源计划 | Tweaks | 「应用选中」=切换电源计划 | RunHidden `powercfg /list` 解析 + `powercfg /setactive GUID` | 否 |
| `sys.update`（新，P2 备选） | 常用管理 | Windows 更新 | Action | 「检查更新」 | WUAPI COM（`wuapi.h` 实测在；IUpdateSearcher） | 否 |

**R7 取舍理由**：入选 6 个（还原点/用户/共享/事件日志/电源计划/更新检查）全部满足「原生 API 或单行命令、网管装机/排障高频、失败可安全回退或不产生不可逆变更」。落选及理由→见 3.3 不做清单。

**模块归属调整**：`sys.process`/`sys.services`/`sys.tasks` 的 Group 从「系统信息」改为「系统管理」，与只读盘点区隔（侧栏语义更清晰）。

## 2.4 安全体检可视化设计（R6）

**新基础设施 `src/procrun.h/.cpp`（流式进程执行器）**：

```cpp
struct ProcRunOptions {
    int      timeoutMs = 1800000;      // 默认 30 分钟（sfc/DISM 量级）
    CancelFn cancel;                    // 取消 → TerminateProcess
    std::function<void(const wstring&)> onLine;  // 逐行回调 → LogView 实时
};
int RunProcess(const wstring& cmdline, const ProcRunOptions& opt, wstring* fullOut);
```

要点：管道双线程（读线程 + 等待线程）避免 `RunHidden`（common.cpp:93-98）「读完成才 Wait」的单线程模型在长任务上的不可取消问题；OEM→UTF16 转码复用现有逻辑（common.cpp:104-111）。

**sec.repair 命令矩阵**（全部经 ProcessRunner，逐行日志 + 进度不定态 + Esc 取消）：

| 修复项 | 命令 | 耗时预估 | 特殊处理 |
|---|---|---|---|
| 系统文件检查 | `sfc /scannow` | 5~20min | 结果解析「未发现完整性冲突/已修复/无法修复」三态 |
| 组件存储修复 | `DISM /Online /Cleanup-Image /RestoreHealth` | 10~30min | 前置 `/ScanHealth` 可选 |
| 磁盘检查 | `chkdsk C: /scan`（只读） | 2~10min | `/f` 需预约重启 → 提示并给出 `chkntfs` 查看方式 |
| 网络栈重置 | `netsh winsock reset` + `netsh int ip reset` | 秒级 | 提示需重启；列日志 |
| DNS/网络刷新 | `ipconfig /flushdns` + `ipconfig /release /renew`（可选） | 秒级 | — |
| Defender | `MpCmdRun -Scan -ScanType 1` / `-SignatureUpdate`（ProgramData 路径探测） | 分钟级 | 失败回退 `powershell Start-MpScan/Update-MpSignature` |

**sec.scan 一键体检**（只读聚合，Action 单按钮）：检查 UAC 是否启用（注册表 EnableLUA）、Defender 实时保护状态、防火墙各 profile 启用、系统盘剩余空间阈值、Windows 更新滞后（WUAPI 或 CBS 最后安装时间）、sfc 建议项（CBS.log 尾部 [SR] cannot repair 计数）——输出逐项「正常/警告」清单，警告项给「去修复」指引文案。

## 2.5 清理引擎重构方案（R5）

1. **新文件 `src/cleanresult.h/.cpp`**：`CleanReport` + `ErrorTextZh` + `DeleteFileSmart`（策略矩阵 L1~L4）+ `CleanDirectoryContentsEx`（新签名，旧函数保留转调，避免破坏 scan.* 模块）。
2. **cleaner.cpp 修复**：
   - :401 FileRepository → `reportOnly=true` + hint「驱动仓库，请用设备管理器/ pnputil 清理旧驱动」（**B1 最高优先级缺陷修复**）
   - :398-399/:425/:428 父子目录去重剪枝（路径前缀包含检查）
   - ScanBasic 浏览器表（:288-298）扩多 Profile 通配与 GPUCache
   - 缺口新增：LiveKernelReports、Panther、LogFiles\Scm/SMSS/WMI、Logs\SIH
   - 回收站项标记 `useEmptyRecycleBin`（ScanItem 新增枚举标记），Apply 走 `SHEmptyRecycleBinW`
3. **cleanmods.cpp**：BasicCleanMod/SystemCleanMod::Apply 改用 CleanDirectoryContentsEx，失败项进 out.rows（c4=「失败 N 个：原因」）与日志；summary 展示真实释放量 + 失败原因分布；Download 项清理前自动走服务联动（L5）。
4. **Winapp2 规则扩展**：`data/Winapp2.ini` 增补 GPUCache/Service Worker/CacheStorage/多 Profile（`User Data\*\` 已支持通配，实测规则格式见根文件 Chrome 节）；`tools/gen_winapp2.py` 重跑生成 `data/winapp2_builtin.h`（该头文件注明由脚本生成，勿手改）。
5. **UI 呈现**：失败原因进 c4 + 汇总；仅统计项提示保留现有 `[仅统计]` 前缀（cleanmods.cpp:22）；清理确认框文案追加「建议先创建系统还原点（常用管理 → 系统还原点）」。

---

# 三、开发计划

## 3.1 分批任务表（批次 × 任务，共 5 个任务 / 4 批次）

**T01 基础设施：提权启动 + 框架扩展点 + 流式执行器**（B1 上半，P0）

- 涉及文件：`src/main.cpp`、`src/common.h`、`src/common.cpp`、`src/module.h`、`src/ui/appwindow.cpp`、`src/app.manifest`（版本随 T05 统一）、新增 `src/procrun.h/.cpp`、`build.bat`（源清单自动收集 ui/modules，procrun 放 src/ 根需在 :39 FILES 手工追加或挪入新目录）
- 依赖：无（起点）
- 验收：标准用户启动 → UAC 弹出 → 同意后管理员运行；取消 UAC → 标准用户运行且不弹第二次；`--list`/`run` CLI 无 UAC；连续启动 3 次无提权循环；`netadmin run net.diag --target 127.0.0.1` 回归通过；gui_smoketest.ps1 通过

**T02 清理引擎重构**（B1 下半，P0，用户痛点）

- 涉及文件：`src/cleaner.cpp`、`src/cleaner.h`、新增 `src/cleanresult.h/.cpp`、`src/modules/cleanmods.cpp`、`data/Winapp2.ini`、`data/winapp2_builtin.h`（脚本再生成）、`src/winapp2.cpp`（如需通配增强）
- 依赖：T01（错误码集中化 + 提权后权限收益）；与 T03-T05 无耦合
- 验收：①FileRepository 扫描结果显示 [仅统计] ②对 `%TEMP%` 构造只读文件/深层超长路径/占用文件（自建测试沙箱目录）实测：成功删除/正确报中文原因/失败项不阻塞其余 ③系统清理父子目录无重复统计 ④清理 summary 展示真实释放量与失败分布 ⑤CLI `run clean.basic` 输出与 GUI 一致 ⑥对测试目录快照比对：清理后无任何非目标文件丢失

**T03 进程/服务/计划任务管理**（B2，P0）

- 涉及文件：`src/modules/sysmods.cpp`（改造两模块）、新增 `src/modules/taskmods.cpp`、`src/module.h`（若扩展动作签名微调）、`src/ui/appwindow.cpp`（第 7 按钮位 + 弹出菜单 + 路由）、`build.bat`（加 `-ltaskschd`）
- 依赖：T01（扩展动作框架在 T01 定型）
- 验收：①结束记事本进程成功且刷新列表；对 csrss 等黑名单项按钮灰/拒绝并说明 ②启动/停止 Spooler 类安全服务成功，状态列自动更新（RefreshAfterAction）③计划任务：列表含「下次运行时间」，对自建任务「禁用/启用/立即运行/删除」全通过，系统任务（\Microsoft\...）操作被 SCM/COM 拒绝时给中文原因 ④全部动作有确认框，Esc 可中止

**T04 安全体检可视化**（B3，P1）

- 涉及文件：新增 `src/modules/secmods.cpp`、`src/procrun.h/.cpp`（T01 交付，此处消费）、`src/ui/appwindow.cpp`（长任务日志/进度联动微调）
- 依赖：T01
- 验收：①体检清单 6 项逐项有结论（本机实测对照）②`DISM /ScanHealth`（只读诊断）全程日志实时滚动 + Esc 中止生效 ③sfc 扫描本机（管理员实例）完成并解析三态结论 ④chkdsk 只给 /scan 只读模式，/f 只提示不代做

**T05 常用管理 + 打磨发版**（B4，P1/P2）

- 涉及文件：新增 `src/modules/mgmtmods.cpp`（还原点/用户/共享/事件日志/电源计划/更新检查 6 模块）、`src/common.h`（k_AppVersionMinor 7→8）、`src/app.rc`（2,8,0,0）、`README.md`（模块数 42→51 与新章节）
- 依赖：T01；T05 内部模块两两独立
- 验收：①创建还原点成功且「系统属性→系统保护」可见 ②用户启用/禁用对自建测试账户生效 ③共享列表与 IPC$/ADMIN$ 特殊共享只读展示，删除仅对用户自建共享放行 ④电源计划切换即时生效 ⑤`build.bat clean` 全量构建零警告、`--list` 51 模块、版本三处一致 ⑥全模块 GUI 冒烟（gui_smoketest.ps1 扩展）

**每批回归方法（统一）**：`cmd /c build.bat` → `netadmin-toolbox.exe --list`（模块数比对）→ `run` 每个新模块冒烟 → gui_smoketest.ps1 → 针对性功能验证（如上）。QA 在独立测试机/虚拟机执行，破坏性操作（还原点外）一律先在快照环境验证。

## 3.2 风险登记册

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | 误删系统文件（FileRepository 类缺陷重演） | 高 | reportOnly 白名单化高危目录；清理引擎新增「顶层危险路径终检」（对齐 winapp2.cpp IsDangerousPattern 的做法）；QA 快照机验证 |
| 2 | 服务/进程操作失误致系统不稳 | 高 | 确认框 + 危险色；MS 核心服务与关键进程黑名单；所有操作单条日志可追溯；不做「全部应用」类一键批操作 |
| 3 | 提权死循环 / UAC 疲劳 | 中 | 双保险标记（环境变量 + 命令行参数）；UAC 取消静默降级；CLI 永不提权；QA 专项：连续启动×3、取消 UAC×2、低权账户登录 |
| 4 | 长任务（sfc/DISM 30min）超时/挂死 | 中 | ProcessRunner 可取消 + 无固定短超时；Esc → TerminateProcess；日志实时可见避免「假死」 |
| 5 | winget/WUAPI/TaskScheduler COM 在家庭版/Insider 上的兼容性 | 中 | 本机（Home Insider 26340）为最严苛样本先行验证；COM 失败一律降级为中文提示不崩溃（对齐 softmods EnsureWinget 的降级模式 softmods.cpp:517-518） |
| 6 | MoveFileEx 占位删除被滥用（删除了不该删的） | 中 | 仅对「L1-L3 失败且属已批准清理目录」启用；数量上限 + 日志明示「重启后删除」 |
| 7 | 回归范围扩大（42→51 模块） | 低 | 模块框架注册表自动去重（module.cpp:11-18）；每批 CLI+GUI 双冒烟 |

## 3.3 不做清单（Out of Scope）

- BitLocker 管理（manage-bde 误操作不可逆，价值/风险比低）
- 磁盘分区管理（diskmgmt.msc 已足够）
- 计划任务的新建/触发器编辑（只做 启用/禁用/运行/结束/删除；编辑走系统 taskschd.msc）
- 组策略编辑器（家庭版无组件）
- 防火墙规则细粒度管理（netsh advfirewall 解析复杂，P3 备选）
- 远程桌面/远程管理（第三方工具成熟）
- 注册表清理（registry cleaner 类功能业界公认风险大于收益）
- 系统还原点的删除与回滚执行（只创建，回滚走系统 rstrui）
- 修改 Windows 更新策略/强推更新（只读检查 + 提示）

## 3.4 关键设计图

类图与时序图见配套文件：

- `docs/class-diagram.mermaid` —— IModule 扩展动作 + CleanReport/ProcessRunner + 服务/任务管理类
- `docs/sequence-diagram.mermaid` —— ①自动提权启动 ②清理执行（策略矩阵）③服务扩展动作 三条主链路

（图内容同时内嵌于两个 .mermaid 文件，此处不重复粘贴。）

---

## 附：勘察证据清单

| 证据 | 来源 |
|---|---|
| 提权链路 4 处代码位 | main.cpp:10-41 / common.cpp:37-62 / app.manifest:7 / appwindow.cpp ID_BTN_ADMIN |
| 清理扫描规则全集 | cleaner.cpp:252-501（逐行核对）+ data/Winapp2.ini（Chrome/Edge/Firefox 等节） |
| 删除实现缺陷 | cleaner.cpp:918-981（RemoveTree 忽略返回值 / CleanDirectoryContents 返回 before） |
| 上层吞错 | cleanmods.cpp:99-115、133-153 |
| 本机环境 | Win11 家庭版 Insider 26340 / Medium 完整性 / Defender 实时保护开启 / PendingFileRenameOperations 8 项 |
| MinGW 库可用性 | taskschd.h + libtaskschd.a、lm.h/lmshare.h + libnetapi32.a、wuapi.h、libwevtapi.a 实测存在 |
| 垃圾目录取样 | TEMP 58.1MB/355 文件、CBS 日志 133.8MB、Installer 1062.6MB、FileRepository 3GB、Edge Cache 120MB（全部只读统计） |
| Schedule 服务 | Running / Automatic（COM 路线前提） |
