# 网管工具箱 NetAdmin Toolbox v2.8 增量 PRD

| 项 | 内容 |
| --- | --- |
| 文档 | incremental-prd-v28.md（v2.7 → v2.8 增量需求） |
| 作者 | 产品经理 Alice / 许清楚（研究模式，只产出文档不改代码） |
| 状态 | 待评审（供软件架构师与开发计划直接使用） |
| 上游输入 | 用户原始需求 R1~R7（见 §3）；仓库现状（v2.7，src/ 8628 行，模块框架 IModule） |
| 技术约束 | C++20 + MinGW-w64；单文件 exe（当前 ~1.7MB，v2.8 上限建议 ≤2.5MB）；纯 Win32 owner-draw 自绘 UI；**零第三方库**；模块=一个 .cpp + REGISTER_MODULE，GUI/CLI 自动生成 |
| 交互范式约束 | 扫描型模块（开始扫描→列表→可勾选→执行）；每模块 4 列（名称/数值/分类/详情）+ 日志控制台；按钮行最多 6 个；确认框与危险态红色渲染已有（ApplyWarning/ApplyIsDanger） |
| 语言 | 简体中文 |

---

## 1. 背景与目标

v2.7 已完成「软件管家」（winget 图形化）并确立了模块化插件框架，但工具当前的能力边界仍停留在「看得见、管不动」：进程只能看不能结束，服务只能看不能启停，计划任务完全缺失，系统安全修复仍依赖用户手打命令，而作为产品根基的垃圾清理又存在「C 盘扫不全、删不掉、下次还在」的口碑风险。v2.8 的核心命题是把工具箱从**盘点型工具**升级为**可操作的网管控制台**：默认管理员身份进入，进程/服务/计划任务可视化管理，安全命令全部图形化，垃圾清理做到「扫得全、删得掉、说得清」，并参照 7 个主流开源项目补足高频管理功能。所有能力必须继续收敛在「单文件、零依赖、免安装」的交付约束内。

**成功标准（可度量）**：

| # | 指标 | 目标值 | 验证方式 |
| --- | --- | --- | --- |
| G1 | 启动提权 | Win11 + 默认 UAC：双击后（含 UAC 确认）≤3s 进入管理员模式；UAC 拒绝/无权限时 ≤1s 降级为标准用户模式且功能可用不崩溃 | QA 装机实测 3 场景（同意/拒绝/标准账户） |
| G2 | 进程/服务/任务操作 | 非关键对象操作成功率 ≥99%；系统关键进程拦截率 100%（PID 0/4、csrss/wininit/services/smss） | 功能测试 + 恶意操作测试 |
| G3 | 清理扫描覆盖率 | 基准测试机构造 ≥20 类已知垃圾，扫描命中 ≥18 类（≥90%），且同一目录不重复计入 | 构造测试目录实测 |
| G4 | 清理删除成功率 | ≥95%；失败项 100% 给出原因与建议；「释放空间」为删除前后实测值，误差 <5% | 构造 100 文件（含 10 个被占用）实测 |
| G5 | 清理后残留 | 成功项二次扫描不再出现（被运行程序重新生成的需在结果中标注原因） | 清理后复扫 |
| G6 | 安全命令 | §5 清单中 P0 项 100% 可执行，退出码与关键输出可见；体检报告 ≤30s 出结果 | 真机执行 9 个 P0 命令 |
| G7 | 无回归 | v2.7 已有全部模块功能、主题、缩放、CLI 无回归 | QA 全量回归（tools/regression.py） |
| G8 | 交付物 | dist/netadmin-toolbox.exe 单文件 ≤2.5MB，无新增第三方库 | 构建产物检查 |

---

## 2. v2.8 功能总览（新增/变更模块一览）

> 命名遵循现有风格（`分类.名称`，snake 分组名）。此表供架构师快速建立全景，详细需求见 §3。

| 模块 ID | 分组 | 类型 | 变更 | 对应需求 |
| --- | --- | --- | --- | --- |
| app 启动流程 | — | — | 默认管理员启动，失败降级 | R1 |
| sys.process | 系统信息 | Inventory → **Manageable（可操作）** | 结束进程、父进程列、打开所在位置 | R2 |
| sys.services | 系统信息 | Inventory → **Manageable** | 启动/停止/重启/暂停恢复、改启动类型 | R3 |
| sch.task | 计划任务（新分组） | Manageable | 枚举（含子目录/隐藏）、运行/结束/启用/禁用/删除 | R4 |
| clean.basic / clean.system / clean.appcache / clean.cdrive | 磁盘清理 | Cleaner | 覆盖面扩充、删除强化、结果表（R5a/b/c） | R5 |
| sys.checkup | 系统体检（新分组） | Inventory（报告型） | 一键体检报告（只读行项聚合） | R6a |
| fix.sfc | 系统修复（新分组） | Action | sfc /scannow | R6b |
| fix.dism | 系统修复 | Action | RestoreHealth + StartComponentCleanup | R6b |
| fix.chkdsk | 系统修复 | Action | chkdsk /scan（+ /spotfix 选项） | R6b |
| fix.netreset | 系统修复 | Action | netsh winsock/ip/ipv6/防火墙重置（可勾选） | R6b |
| fix.wuupdate | 系统修复 | Action | usoclient 触发 + WuReset 修复流程 | R6b |
| fix.defender | 系统修复 | Action | MpCmdRun 定义更新/快扫/全扫/自定义路径 | R6b |
| fix.license | 系统修复 | Action | slmgr /xpr、/dlv（只读诊断） | R6b |
| sys.restorepoint | 系统管理（新分组） | Action | 创建系统还原点 | R7-1 |
| sys.users | 系统管理 | Manageable | 本地账户枚举/启禁用/改密/新建/删除 | R7-2 |
| sys.shares | 系统管理 | Manageable | 共享文件夹枚举/权限/新建/删除/会话 | R7-3 |
| sys.eventlog | 系统管理 | Inventory | 事件日志查看（系统/应用/安全，过滤+详情） | R7-4 |
| net.firewall | 网络管理 | Manageable | 防火墙规则查看/启禁用/增删 | R7-5 |
| sys.windowsupdate | 系统管理 | Manageable | 更新扫描/安装/历史 | R7-6 |
| sys.smart | 系统管理 | Inventory | 磁盘 SMART 健康 | R7-7 |
| sys.powerplan | 系统管理 | Manageable | 电源计划查看/切换 | R7-8 |
| sys.shutdown | 系统管理 | Action | 定时关机/取消 | R7-9 |

**框架影响（提示架构师，不限定实现）**：现有 `ModuleKind` 只有 Inventory/Cleaner/Action/Tweaks/Console。R2/R3/R4 需要「扫描→勾选→多操作」形态，建议新增 `Manageable` 类型（复用 `SetSelection` 与 `ModuleRow.tag` 回溯，按钮由模块声明最多 6 个操作），或由架构师定等价方案；GUI/CLI 其余不动，保持「加模块不改框架」的扩展性承诺。

---

## 3. 需求池（P0 / P1 / P2）

> P0 = 本批必做；P1 = 应做尽量；P2 = 候选观望。每条含：编号、用户故事、功能描述、验收标准（可测试）、优先级理由。

### 3.1 R1 启动默认管理员模式（P0）

**R1.1 自动提权启动**

- 用户故事：作为网管，我希望双击 exe 就自动进入管理员模式，不用每次手动右键「以管理员身份运行」。
- 功能描述：
  1. `wWinMain` 启动后先判断 `IsElevated()`；未提权时通过 `ShellExecuteExW`（动词 `runas`）静默重启自身，原样透传命令行参数；
  2. 必须带防循环标记（如内部参数 `--elevated-retry` 或环境变量），保证提权重启只发生一次；
  3. 提权失败（`ERROR_CANCELLED` 用户拒绝 UAC、无管理员账户、UAC 被域策略禁用等）→ 立即以标准用户模式继续运行，权限胶囊显示「标准用户」，日志给出一次降级说明；
  4. manifest **保持 `asInvoker`**（不可改 `requireAdministrator`，否则无降级路径）；`--gui`/CLI 模式语义不变（CLI 建议不自动提权，由用户自行决定，文档注明）；
  5. 已有「以管理员重启」按钮与权限胶囊复用为降级状态展示，不新增 UI 元素。
- 验收标准：
  - AC1 管理员账户 + 默认 UAC：双击 → UAC 弹窗 → 同意 → 管理员模式，胶囊「管理员模式」，全程一次重启；
  - AC2 UAC 选「否」：降级启动，胶囊「标准用户」，功能可用（各模块按现有 `NeedsElevation` 逻辑降级提示）；
  - AC3 标准用户账户（无管理员凭据）：直接降级可用，无崩溃、无死循环；
  - AC4 `--list`/CLI 行为与 v2.7 一致，不因提权逻辑变化。
- 优先级理由：R2/R3/R5/R6 大量功能依赖管理员权限，这是整批需求的公共前置；不升级权限则后续模块全部半残。

### 3.2 R2 进程管理增强（P0）

**R2.1 结束进程**

- 用户故事：作为网管，我希望在进程列表勾选无响应/可疑进程一键结束，不必切去任务管理器或记 taskkill。
- 功能描述：
  1. `sys.process` 升级为 Manageable：勾选行 →「结束进程」按钮 → 确认框（列进程名+PID，危险态红色渲染）→ `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess`；
  2. **关键进程硬拦截**：PID 0、PID 4（System）、`csrss.exe`、`wininit.exe`、`services.exe`、`smss.exe`、`winlogon.exe`、自身进程——勾选即禁用或点击即提示「系统关键进程，已禁止操作」；
  3. 失败逐条报告：`ERROR_ACCESS_DENIED`（受保护进程/权限不足）、`ERROR_INVALID_HANDLE`（进程已退出）等，转中文原因写日志；
  4. 操作完成后自动刷新列表（或提示手动刷新，二者择一，默认自动刷新）；
  5. 列增强：显示父进程名（PROCESSENTRY32 的 `th32ParentProcessID`，低成本高价值，解决「谁拉起的它」）。
- 验收标准：
  - AC1 结束一个测试用 notepad.exe：成功，刷新后列表中消失，日志有记录；
  - AC2 尝试结束 System（PID 4）/csrss：被拦截并有明确文案，不产生任何系统调用副作用；
  - AC3 结束一个受保护进程（如以 SYSTEM 运行的服务进程）在标准用户下：给出「拒绝访问（需要管理员权限）」；
  - AC4 对已退出的进程操作：提示「进程已不存在」而非报错码。
- 优先级理由：用户点名需求；与 R5b「占用文件进程处理」共享同一底层能力（Restart Manager/进程查询），一次建设两处受益。

**R2.2 打开文件所在位置（P1）**

- 功能：对选中进程执行 `explorer.exe /select,"<路径>"`；无路径（权限不足）时按钮禁用。
- 验收：点击后资源管理器打开并选中目标 exe。
- 理由：定位可疑文件是网管最高频的后续动作；实现仅一句 ShellExecute。

### 3.3 R3 服务管理增强（P0）

**R3.1 服务启动/停止/重启/暂停恢复 + 启动类型**

- 用户故事：作为网管，我希望直接启停服务、改启动类型，不用开 services.msc 或记 `sc config`。
- 功能描述：
  1. `sys.services` 升级为 Manageable，按当前状态动态启用按钮：「启动」「停止」「重启」「暂停/恢复」（暂停仅对支持的服务显示）；
  2. `StartService` 为异步操作：轮询 `QueryServiceStatus` 直至 RUNNING/超时（默认 30s，读取 `dwWaitHint` 提示），日志实时回显阶段文案；
  3. 失败原因中文化：`ERROR_DEPENDENT_SERVICE_RUNNING`（依赖服务在跑）、`ERROR_SERVICE_ALREADY_RUNNING`、`ERROR_ACCESS_DENIED` 等；
  4. 修改启动类型（自动/手动/禁用）独立操作入口（勾选后弹出选择或第六按钮），需 `SERVICE_CHANGE_CONFIG` 权限；禁用与启用均二次确认，禁用为危险态渲染；
  5. 停止非微软核心服务才允许一键；停止前确认框列出服务显示名+PID；对「停止即可能断网/蓝屏」的高风险服务清单（如 Dhcp、Dnscache、MpsSvc、BITS、wuauserv、Lsa、SamSs）停止前额外红字警示。
- 验收标准：
  - AC1 对「已停止」服务点「启动」：按钮禁用或提示「服务未运行」；对运行中服务点「停止」同理；
  - AC2 启动一个停止的非关键服务：状态列更新为「正在运行」，日志含耗时；
  - AC3 停止一个被依赖的服务：提示「有服务依赖它，无法停止」并列出依赖（能拿到则列）；
  - AC4 标准用户下操作：明确「拒绝访问（需要管理员权限）」；
  - AC5 改启动类型为「禁用」：二次确认 + 危险红渲染，成功后列表启动类型列更新。
- 优先级理由：用户点名需求；与 R2 共用「勾选+确认+操作+刷新」范式，框架一次投入两模块复用。

### 3.4 R4 计划任务管理（P0）

**R4.1 计划任务枚举与操作**

- 用户故事：作为网管，我希望查看和管理计划任务（含隐藏任务、子目录），因为大量自启动和「删了又回来」的故障藏在任务计划程序里。
- 功能描述：
  1. 新模块 `sch.task`，分组「计划任务」；Task Scheduler 2.0 COM（`taskschd.lib`，零依赖）：`ITaskService::Connect` → `GetFolder(L"\\")` → 递归 `GetFolders` + `GetTasks(TASK_ENUM_HIDDEN)`；
  2. 列表 4 列：名称 / 状态（就绪/正在运行/已禁用/失败…映射 `TASK_STATE`）/ 分类=所在文件夹+作者 / 详情=下次运行时间·上次运行时间·上次结果（`LastRunTime`/`LastTaskResult`）；
  3. 操作（按状态动态启用）：「立即运行」(`Run`)、「结束」(`Stop`，仅运行时)、「启用」(`Enable`)、「禁用」(`Disable`)、「删除」(`Delete`，危险态二次确认)；
  4. 删除/禁用微软官方路径任务（`\Microsoft\` 开头）前确认框红字警示「可能影响系统功能」；
  5. 非管理员：枚举可读，操作给出权限提示（与 R1 降级模式联动）。
- 验收标准：
  - AC1 枚举根目录+子文件夹任务（典型机器 100+），summary 给出总数/运行/禁用分布；
  - AC2 禁用一个用户任务：状态变「已禁用」；再启用恢复；
  - AC3 删除用户任务：二次确认，成功后不再出现在列表；
  - AC4 「立即运行」后详情列「上次运行时间/结果」更新；
  - AC5 对 `\Microsoft\Windows\` 下任务点删除：出现红字警示确认框。
- 优先级理由：用户点名需求；Dism++ 启动管理、Sophia 的定时清理、Sysinternals Autoruns 都把计划任务列为自启动核心位置，缺失它是「看得见启动项、管不到真凶」。

### 3.5 R5 垃圾文件清理深化（P0，拆三条）

#### R5a 扫描覆盖面（P0）

- 用户故事：作为网管，我希望一次扫描把所有常见垃圾找全——包括**其他用户**的垃圾、浏览器多 Profile 缓存、更新残留、旧驱动、错误报告、内存转储，而不是只扫当前用户的一半。
- 功能描述（在现有 ScanBasic/ScanSystem 基础上扩充）：
  1. **多用户扫描**：遍历 `C:\Users\*`（需管理员；跳过 `Default`、`Default User`、`Public`、`All Users`），对每个用户扫 `AppData\Local\Temp`、浏览器缓存、回收站；无管理员时降级仅当前用户并在 summary 注明；
  2. **浏览器多 Profile**：Chrome/Edge/Brave/Vivaldi/Opera 扫 `User Data\Default` + `Profile 1..N`（目录通配），新增 `GPUCache`、`Service Worker\CacheStorage`、`GrShaderCache`、`Media Cache`、`Code Cache`、`DawnGraphCache`/`DawnScopedCache`（较新 Chromium 目录，实测存在则收）；
  3. **补充类别**（详见 §6 清单）：`$WinREAgent`（隐藏目录，reportOnly）、`Windows.old`（reportOnly+引导）、DirectX Shader Cache、`C:\NVIDIA\CacheDriver`（安装缓存，常 1~2GB）、缩略图 `thumbcache_*.db`、内存转储 `MEMORY.DMP`+`Minidump`、系统 WER 队列/存档、传递优化、`Installer\$PatchCache$`（已有）与 `Windows\Installer` 的 .msi/.msp（维持 reportOnly）、驱动仓库旧版本（`DriverStore\FileRepository` 中无对应 INF 的孤立目录，reportOnly+建议走驱动管理）；
  4. **按驱动器可选**：默认仅系统盘；提供「全盘」勾选项（其他固定盘只扫浏览器缓存/回收站/临时，避免把用户数据盘翻个底朝天）；
  5. 与既有 `clean.appcache`（Winapp2 规则模块）分工：硬编码项管系统级，Winapp2 规则管第三方应用级；扩充内置规则集条目（对标 BleachBit 的 winapp2 生态，做法见 §4 兼容性结论）。
- 验收标准：
  - AC1 覆盖率：构造 ≥20 类垃圾（每类已知大小），命中 ≥18 类；
  - AC2 同类目不重复：同一目录不在两个清理项中重复计入（扫描后自检去重）；
  - AC3 性能：SSD 全类扫描 ≤90s、HDD ≤180s，全程可取消（`ctx.Cancelled()`），取消后 summary 标注「已中止」；
  - AC4 summary 明确分项：「共 N 项　可清理 X 项 / Y　仅统计 Z 项 / W」；
  - AC5 多用户机器：管理员下能扫到至少 1 个非当前用户 Temp/缓存；标准用户下降级不报错。
- 优先级理由：用户原话「C 盘瘦身扫描不全」是 R5 第一痛点；扫不到就删不掉，覆盖面是删除成功率的前提。

#### R5b 删除成功率与失败归因（P0）

- 用户故事：作为网管，我希望勾选清理后真的删掉，不再「下次扫描还在」；删不掉的也要逐条告诉我为什么、怎么办。
- 功能描述（根因：现 `CleanDirectoryContents` 忽略 `DeleteFileW`/`RemoveDirectoryW` 返回值、按 before 估算释放量、对占用/权限失败无对策）：
  1. **逐项删除 + 结果回收**：清理改为 per-item 结果结构（成功/失败 + 错误码）；「释放空间」= 删除前后 `DirSize` 实测差值，不再用估值；
  2. **占用文件处理**（Restart Manager，`rstrtmgr.dll`，零依赖）：删除遇 `ERROR_SHARING_VIOLATION` → `RmStartSession/RmRegisterResources/RmGetList` 找出占用进程 →
     - 默认策略：跳过并记录「被 `chrome.exe` 占用，请关闭后重试」；
     - 勾选「结束占用进程后重试」时：对占用进程结束再删（ explorer.exe/浏览器等可安全重启进程允许；其余需确认），删完可提示重启资源管理器；
  3. **重启后删除**：仍失败项提供「重启后删除」选项（`MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT)`），重启生效，下轮扫描验证消失（对标 Sysinternals MoveFile 思路）；
  4. **权限失败**：`ERROR_ACCESS_DENIED` → 提示「需要管理员权限」或「系统保护项，已改为仅统计」，不静默跳过；
  5. **浏览器/微信运行中**：检测到目标进程运行时，对应缓存项跳过并记录「程序正在运行，关闭后清理更彻底」；
  6. **失败重试**：每项删除失败后原地重试 1 次（应对瞬时锁）；
  7. 删除后逐项复测：成功 = 该项 size 归零；未归零计入失败。
- 验收标准：
  - AC1 构造 100 个待清理文件（含 10 个被进程占用）：默认策略下删除成功率 ≥95%（占用项计入失败）；启用「结束占用进程后重试」后 ≥99%；
  - AC2 失败项 100% 有原因文案（错误码 + 中文 + 建议动作）；
  - AC3 「释放空间」与实测差值误差 <5%；
  - AC4 「重启后删除」项在重启后二次扫描不再出现；
  - AC5 全程可取消，取消时已删项保留、报告半程状态。
- 优先级理由：用户原话「删除时删不掉（下次扫描还在）」是 R5 第二痛点，也是清理类工具口碑的生命线；失败归因是「可视化」的底线要求。

#### R5c 清理结果可视化（P0）

- 用户故事：作为网管，我希望清理结束看到一张明白账：哪些成功、哪些失败、为什么失败、实际释放多少，并能对失败项一键重试。
- 功能描述：
  1. 清理完成后结果表（4 列）：项目 / 释放大小 / 状态（✓ 成功、✗ 失败、– 跳过（仅统计/占用）/ ⏳ 待重启删除）/ 失败原因与建议；
  2. summary 行：「清理完成：成功 X 项 / 失败 Y 项 / 跳过 Z 项，实际释放 N」；
  3. 失败项支持勾选「重试选中」；「待重启删除」项注明重启后自动完成；
  4. 操作前可选「创建还原点」（联动 R7-1，默认勾选，需管理员；创建失败不阻断清理，仅提示）。
- 验收标准：
  - AC1 混合场景（成功/失败/跳过/待重启）清理后四态在结果表正确呈现；
  - AC2 点「重试选中」对失败项重新执行，状态更新；
  - AC3 非管理员 + 勾选还原点：提示「无法创建还原点（需管理员）」后仍可继续清理。
- 优先级理由：与 R5b 是同一验收闭环的两面（做得成 + 说得清）；成本主要是结果数据结构与表格渲染，框架已具备。

### 3.6 R6 系统安全命令可视化（P0 拆分）

> 命令级明细见 §5。此处只立需求条目。

**R6a 一键体检（P0）**

- 用户故事：作为网管，我希望点一次「开始体检」，30 秒内拿到一份系统健康报告，不用背任何命令。
- 功能描述：新模块 `sys.checkup`（分组「系统体检」，Inventory 报告型），聚合 §5 表「体检行」项：Defender 状态与病毒库日期、防火墙各配置文件状态、关键服务状态（wuauserv/BITS/MsMpSvc/Schedule/WlanSvc）、Windows 更新最近安装日期/是否待重启、许可证激活状态（slmgr /xpr 解析）、还原点存在性与最近日期、系统版本、各盘空间概览、hosts 文件异常行数。每行输出「检查项 / 结果 / 建议」，异常行按 `danger` 红色渲染。
- 验收：AC1 全部体检行 ≤30s 完成（并行/快查实现）；AC2 每行有明确结果（正常/异常/无法检测），无空行；AC3 异常项给出下一步动作入口（如「去修复」链接到对应 fix 模块或提示命令）；AC4 标准用户下可运行，权限敏感项标注「需管理员」。
- 优先级理由：R6「做成可视化界面，不用背命令」的聚合出口；火绒「安全分析工具」、winutil「System Corruption Scan」验证该形态。

**R6b 安全修复命令独立模块（P0）**

- 用户故事：作为网管，我希望 sfc、DISM、chkdsk、netsh 重置、Windows Update 修复、Defender 扫描都有图形入口，执行过程实时看到输出，而不是打开 cmd 背命令。
- 功能描述：6 个 Action 模块（`fix.sfc`/`fix.dism`/`fix.chkdsk`/`fix.netreset`/`fix.wuupdate`/`fix.defender`，分组「系统修复」），统一执行形态：
  1. `CreateProcess` 重定向 stdout/stderr 管道 → `ctx.Log` 逐行实时输出（保留命令原文回显）；
  2. 不定进度（`ctx.Report(-1, 阶段文案)`）+ 完成行汇总退出码与关键结论；
  3. 可取消（长任务取消前确认框警示「可能导致修复未完成」；MpCmdRun 用 `-Cancel` 优雅取消）；
  4. 非管理员运行：明确拒绝并提示提权（联动 R1 的提权入口）；
  5. 每个模块的确认框文案自拟（`ApplyWarning`），DISM/chkdsk 修复类标危险态（`ApplyIsDanger`）。
- 验收：AC1 §5 表中 P0 命令逐条真机执行，输出可见、退出码可见；AC2 长任务取消后状态提示「已取消，修复可能未完成」；AC3 标准用户下执行给出提权提示而非原始错误码。
- 优先级理由：用户原话「系统安全相关扫描和修复类命令全部做成可视化界面，不用背命令」；这是本批与「清理」并列的两大价值主张。

### 3.7 R7 其他常用管理功能（按调研短名单立项）

> 短名单依据见 §4 调研结论。每项含用户故事摘要、功能、验收、理由；P0/P1/P2 与代价标注。

**R7-1 系统还原点创建（P0，代价低）**

- 用户故事：作为网管，我希望在批量优化/清理前一键创建还原点，出问题能回滚。
- 功能：`sys.restorepoint`（Action）：动态加载 `srclient.dll` → `SRSetRestorePointW`（BEGIN/END_SYSTEM_CHANGE，需 `CoInitializeSecurity`）；描述文案可输入（默认「网管工具箱：清理/优化前快照」）；失败原因中文化（`ERROR_SERVICE_DISABLED`=系统还原被关闭 → 提示开启方法）。
- 验收：AC1 管理员下创建成功并返回序列号，日志可见；AC2 系统还原关闭时给出开启指引；AC3 24h 内重复创建按系统规则跳过并说明（Win8+ 默认频率限制，读 `SystemRestorePointCreationFrequency`）。
- 理由：winutil 应用 tweak 前自动建还原点、Sophia `CreateRestorePoint` 均将此作为安全基座；同时是 R5c「操作前还原点」的公共能力。**不做还原点列表/回滚**（枚举 API 不公开，PowerShell 化违背零依赖，列 P2）。

**R7-2 本地用户账户管理（P1，代价中）**

- 用户故事：作为网管，我希望新建/禁用/改密本地账户，不用进 lusrmgr 或记 `net user`。
- 功能：`sys.users`（Manageable）：`NetUserEnum` 列账户（名称/状态（启用/禁用）/组/上次登录/密码永不过期标记）；操作：启用/禁用（`NetUserSetInfo` level 1008 `UF_ACCOUNTDISABLE`）、重置密码（level 1003）、新建（`NetUserAdd`）、删除（危险态）。内置 Administrator/Guest/DefaultAccount 保护（删除需红字确认）。
- 验收：AC1 枚举本机全部本地账户；AC2 禁用测试账户后状态更新，启用恢复；AC3 新建标准账户+设密码后用该账户可登录（QA 实测）；AC4 删除内置账户被红字确认拦截。
- 理由：装机/交机高频；netapi32 为系统库零依赖。

**R7-3 共享文件夹管理（P1，代价中）**

- 用户故事：作为网管，我希望查看/新建/停止共享并看谁在连，不用计算机管理台。
- 功能：`sys.shares`（Manageable）：`NetShareEnum` 列共享（名/路径/备注/连接数）+ `NetConnectionEnum` 当前会话；操作：新建共享（`NetShareAdd`，可设备注）、删除（危险态）、断开指定会话。
- 验收：AC1 列出默认管理共享（C$/ADMIN$）+ 自定义共享；AC2 新建共享后另一台机器可访问；AC3 删除自定义共享成功，删除 C$ 被拦截提示「管理共享不可删除」。
- 理由：网管日常运维高频（文件服务、打印机共享场景）。

**R7-4 事件日志查看（P1，代价中）**

- 用户故事：作为网管，我希望直接看系统/应用程序日志、按级别过滤、看详情，不用事件查看器。
- 功能：`sys.eventlog`（Inventory）：`EvtQuery`（wevtapi）XPath 查询最近 N 条（默认 200，可调），列：时间/级别（错误红色、警告橙色）/来源/事件 ID/消息摘要；双击（或选中+按钮）`EvtRender`+`EvtFormatMessage` 出详情弹窗；过滤器：通道（System/Application/Security）+ 级别 + 关键词；Security 通道无权限时提示。
- 验收：AC1 列出 System 通道最近 200 条并按时间倒序；AC2 按「仅错误」过滤后无警告行；AC3 详情弹窗含格式化消息；AC4 标准用户访问 Security 通道给出权限提示。
- 理由：排障核心入口，Dism++/火绒/Sysinternals 用户的基础习惯。

**R7-5 防火墙规则查看与管理（P1，代价中）**

- 用户故事：作为网管，我希望看防火墙规则、启用/禁用、必要时增删，不用 netsh 背参数。
- 功能：`net.firewall`（Manageable，并入网络管理组）：COM `INetFwPolicy2`（FirewallAPI.h）列规则（名称/方向（入站·出站）/启用/动作（允许·阻止）/程序或端口/配置文件）；操作：启用/禁用（`Enabled` 属性）、新建（程序或端口规则，选择方向+动作+协议+端口）、删除（危险态）；另可只读展示各配置文件状态（与 R6a 体检共享查询代码）。
- 验收：AC1 列出全部规则（典型 500+）并支持按方向过滤；AC2 禁用某条自定义规则后状态更新；AC3 新建「阻止 9999 端口入站」规则后实测被拦；AC4 删除系统内置规则红字确认。
- 理由：火绒网络防御、winutil Reset Network 的周边；规则可视化是「不用背命令」的自然延伸。

**R7-6 Windows 更新管理（P1，代价中高）**

- 用户故事：作为网管，我希望主动扫更新、装勾选的更新、看历史，不用设置面板。
- 功能：`sys.windowsupdate`（Manageable）：WU API COM（wuapi）`IUpdateSession::CreateUpdateSearcher` 扫「IsInstalled=0」更新（标题/分类/大小/是否需重启/MsrcSeverity）；勾选 → `IUpdateInstaller` 下载安装（实时进度+日志）；历史：`QueryHistory` 最近 N 条（日期/KB/结果）；另提供「立即扫描」触发（usoclient StartScan，与 R6b 共享）。暂停/策略类（winutil 的 Default/Security/Disable 三档）不做（策略改注册表风险与定位不符，列 P2）。
- 验收：AC1 扫描出可用更新列表；AC2 安装一个勾选的小更新（QA 实测或虚拟机）；AC3 历史列表与设置面板「更新历史记录」一致；AC4 无可用更新时明确「已是最新」。
- 理由：winutil 独立 Updates 标签验证需求强度；但 COM 交互复杂，控制范围只做「扫描+安装+历史」。

**R7-7 磁盘 SMART 健康（P2，代价中）**

- 用户故事：作为网管，我希望体检时看硬盘健康度（温度/通电时间/重分配扇区），提前发现快坏的盘。
- 功能：`sys.smart`（Inventory）：`IOCTL_STORAGE_QUERY_PROPERTY` 确认 SMART 支持 → `SMART_RCV_DRIVE_DATA_CONTROL` 读属性表，解析关键属性（05 重分配扇区数、09 通电小时、194 温度、C5 待映射、C6 脱机不可修正）→ 状态列（良好/警告/危险，超出阈值按 danger 红渲染）；解析失败/不支持时明确降级文案。
- 验收：AC1 每块物理盘一行，关键属性可见；AC2 对一块坏道盘（测试样本）标记「危险」红色；AC3 不支持 SMART 的设备（部分 USB 盘/RAID）给出「不支持」而非崩溃。
- 理由：网管预防性维护高价值；实现门槛中等且无系统 API 依赖。

**R7-8 电源计划管理（P2，代价低）**

- 用户故事：作为网管，我希望快速切换电源计划（如临时开「卓越性能」压测后切回平衡）。
- 功能：`sys.powerplan`（Manageable）：`PowerEnumerate` 列计划 + `PowerSetActiveScheme` 切换；识别「卓越性能」计划（隐藏 GUID `e9a42b02-d5df-448d-aa83-0b785bddbc56`，不存在则提供一键生成 `powercfg -duplicatescheme`）。
- 验收：AC1 列出当前全部计划及激活项；AC2 切换后 `powercfg /getactivescheme` 一致；AC3 一键生成卓越性能计划成功。
- 理由：winutil/Sophia 均有；成本极低，适合填充批次。

**R7-9 定时关机（P2，代价低）**

- 用户故事：作为网管，我希望挂机下载/部署完自动关机，并能取消。
- 功能：`sys.shutdown`（Action）：输入分钟数 → `InitiateSystemShutdownExW`（带原因与超时，可强制关闭应用开关）；「取消关机」=`AbortSystemShutdownW`；倒计时与警告提示（保存工作）。
- 验收：AC1 设定 1 分钟关机，倒计时正常并可取消；AC2 关机前弹倒计时警告（系统原生行为）；AC3 取消后不再关机。
- 理由：小工具补位；风险点仅在误操作，用确认框+倒计时缓冲。

**R7 明确不做（P2 观望，理由记录在案）**

| 候选 | 不做/观望理由 |
| --- | --- |
| 文件占用查询（File Locksmith 式独立模块） | Restart Manager 能力已在 R5b 建设；独立模块价值边际低，后续可作为 R5b 失败项的「定位」动作开放 |
| 创建计划任务向导 | 任务定义（触发器/动作/条件）COM 建模工作量大，v2.8 只做管理不做创建 |
| 注册表编辑器 | regengine 已服务 Tweaks；通用注册表编辑器与「工具箱」定位有偏差，风险高 |
| 右键菜单管理 | Dism++/火绒有，但增删注册表风险与收益比一般，排 P2 |
| 系统精简/ISO 定制（winutil Win11 Creator/Dism++ 镜像） | 超出网管日常工具箱定位，工作量巨大，明确不做 |
| 弹窗拦截、流量监控、专杀工具（火绒式） | 需 HOOK/驱动级实现，违背单文件零依赖与安全定位，不做 |

---

## 4. 开源项目调研表

> 调研方式：WebSearch/WebFetch 公开资料（2026-10）。结论用于 R7 取舍。引用见文末来源。

| 项目 | 概况 | 功能面清单（公开资料） | 可借鉴点（落到本项目） | 与「单文件零依赖」兼容性判断 |
| --- | --- | --- | --- | --- |
| **hellzerg/optimizer** | C#/.NET 4.8，GPL-3.0，约 1.8 万 Star；2026-01 仓库已归档，由 optimizerNXT 接棒，最后版本 16.7（2024-08） | 模块化分组（通用/Win10/Win11/UWP/启动项/垃圾清理）；40+  tweaks；禁用服务/遥测/Cortana/自动更新；UWP 卸载；DNS 切换预置列表；hosts 编辑；启动项管理；文件锁句柄查找+结束进程；硬件检视；注册表修复；ping 延迟；静默模板部署 | ①一功能一模块按类别组织（已采纳）；②「文件锁+结束进程」直接支撑 R5b 失败项处理与 R2；③危险功能（禁 Defender/更新）明确风险警示的做法；④ DNS 预置列表（P2 候选） | **高（理念级）**。.NET 依赖不适用；其功能清单已被本项目框架结构消化，无可移植代码 |
| **BleachBit** | 2008 年起，Python+GTK，GPL；Windows 6.0.2 | 规则化清理（CleanerML + winapp2.ini 社区规则 2400+）；清理前预览；白名单/排除；Cookie 管理；安全粉碎（多次覆写）；空闲空间擦除；计划清理；CLI；便携模式 | ①「预览→确认→删除」两段式（已有）；②排除/白名单机制 → R5 风险控制建议做「永不清理此项」持久化；③ winapp2 规则生态 → 本项目已有 winapp2.cpp+内置规则，R5a 按此扩充；④**不借鉴**：粉碎/空闲擦除（SSD 无意义且伤盘、耗时，与快清理定位冲突） | **中**。规则格式已吸收（clean.appcache）；粉碎/擦除明确排除 |
| **Dism++（初雨团队）** | C++ 原生，MIT，单文件 3.5/4.2MB，Vista~11；基于 CBS 内部 API，不依赖 dism.exe | 空间回收（更新清理/WinSxS/临时/DirectX 着色器缓存/Defender 缓存/程序缓存，宣称可释 10~30GB）；CompactOS 压缩；启动项管理（原生+计划任务+服务+Explorer 扩展）；驱动管理（备份/导出/清理，解决 NVIDIA 历史驱动占几十 GB）；系统修复（CBS RestoreHealth）；更新管理（选择性安装/卸载）；Appx 管理；hosts；右键菜单；ESD 备份；镜像处理 | ①「空间回收」大盘视角与清理项分类命名 → R5a 类别清单参照（DirectX 着色器缓存、驱动仓库等已纳入）；②旧驱动清理思路（R5a reportOnly+引导）；③Windows.old/$WinREAgent 处理入口；④ CompactOS 压缩提示（P2）；⑤ 启动项=计划任务+服务+Explorer 扩展的全位置观（R4 立项依据之一） | **高（同为 C++ 原生 API 路线）**。CBS 内部 API 不公开，对应能力用 DISM.exe 子进程替代（R6b fix.dism）；镜像/ESD 功能不做 |
| **ChrisTitusTech/winutil** | PowerShell 脚本+WPF 界面，约 5.7 万 Star，MIT | Install（winget 批量装/升/卸，本项目 v2.7 已有）；Tweaks 66 项分 Essential/Advanced；**应用前自动创建还原点+可逐项还原**；Config：Windows Features 勾选（.NET/Hyper-V/WSL/Sandbox）、修复（Autologin/Reset Windows Update/Reset Network(netsh)/System Corruption Scan(sfc+DISM)/WinGet 重装）、legacy 面板直达、OpenSSH；Updates：Default/Security/Disable 三档策略；Win11 定制 ISO | ①Essential/Advanced 分级+一键还原（本项目 Tweaks 已有还原，建议后续补分级文案）；②**应用前自动还原点** → R5c 默认勾选项；③ Reset Network（netsh）→ R6b fix.netreset；④ System Corruption Scan（sfc+DISM 组合）→ R6a/R6b 编排顺序；⑤ legacy 面板直达（P2） | **高（模式级）**。PowerShell 运行时不可引入；功能用原生 API 重写 |
| **Sophia Script（farag2）** | PowerShell 模块，150+ 函数，MIT | CreateRestorePoint；**计划任务创建**（Windows Cleanup 定时清 SoftwareDistribution\Download+%TEMP%，toast 通知可选）；Defender 排除项管理；Controlled folder access；资源管理器/右键深度 tweaks；存储感知；UWP 卸载；电源计划；WSL 安装 | ①还原点先行（R7-1）；②「定时清理任务」形态 → 可作为 R4 的延伸用例（P2「定期自动清理」）；③toast 式结果反馈（本项目用日志面板+summary 替代，不引 toast COM） | **高（模式级）**。无可移植实现 |
| **Microsoft PowerToys** | C++/C# 混合，MIT，30+ 实用工具 | Hosts File Editor（编辑+自动备份+过滤+提权）；Environment Variables（可视化编辑+profile）；File Locksmith（查占用进程并可结束）；Registry Preview；PowerRename；ZoomIt 等 | ①hosts 只读→可编辑+自动备份（P1，本项目 sys.hosts 现为只读 Console）；②环境变量只读→可编辑（P1/P2，sys.env 扩展）；③文件占用查询（R5b 已用 RM 实现，不单独立模块） | **高**。均为标准 Win32/COM 能力 |
| **Sysinternals Suite** | Mark Russinovich，70+ 工具，Microsoft 免费许可 | Process Explorer（进程树/DLL/句柄/**Find Handle**/VirusTotal 列）；Process Monitor（文件+注册表+进程实时监控）；Autoruns（全量自启动位置+签名验证）；TCPView；RAMMap；ProcDump；PsTools；**MoveFile（重启删除）**；BgInfo | ①Find Handle → R5b 占用定位（RM 实现）；②进程树 → R2.1 父进程列；③**重启删除**（MoveFileEx DELAY_UNTIL_REBOOT）→ R5b 已规划；④Autoruns「全位置+签名」→ 启动项扩展（P2，当前 sys.startup 覆盖 Run+启动文件夹，可加计划任务位置） | **高（诊断理念）**。不打包其二进制（保持自包含）；理念已转化为自研实现 |
| **火绒安全软件 6.0** | 国产免费（个人版），自有引擎 | 病毒查杀；防护中心（入侵/钓鱼/勒索/无文件）；访问控制；**安全工具 14 个**：漏洞修复、系统修复、弹窗拦截、垃圾清理、启动项管理、文件粉碎、右键管理、**断网修复**、流量监控、修改 HOSTS、ARP 防护、DHCP 检测、安全分析工具、专杀工具 | ①「断网修复」一键化 → R6b fix.netreset 的产品化参照；②「系统修复」聚合 → R6a/R6b 体检+修复编排；③「安全分析工具」报告形态 → R6a 体检报告行设计；④垃圾清理分类 → R5a 类别参照；⑤**不借鉴**：实时防护引擎（驱动/HOOK 级，超定位） | **高（产品形态级）**。引擎自研不现实也无必要；其「工具矩阵」形态是 v2.8 分组扩张的直接参照 |

### R7 推荐功能短名单（按调研结论取舍）

| # | 功能 | 立项模块 | 优先级 | 实现代价 | 依据（调研来源） |
| --- | --- | --- | --- | --- | --- |
| 1 | 系统还原点创建 | sys.restorepoint | **P0** | 低 | winutil/Sophia 均将还原点作为安全基座；R5c 联动 |
| 2 | 本地用户账户管理 | sys.users | P1 | 中 | 装机高频；netapi32 |
| 3 | 共享文件夹管理 | sys.shares | P1 | 中 | 网管高频；netapi32 |
| 4 | 事件日志查看 | sys.eventlog | P1 | 中 | 排障核心；wevtapi |
| 5 | 防火墙规则查看/管理 | net.firewall | P1 | 中 | 火绒网络防御周边；FirewallAPI COM |
| 6 | Windows 更新管理（扫描/安装/历史） | sys.windowsupdate | P1 | 中高 | winutil Updates 标签 |
| 7 | 磁盘 SMART 健康 | sys.smart | P2 | 中 | 预防性维护；IOCTL/SMART 自解析 |
| 8 | 电源计划管理 | sys.powerplan | P2 | 低 | winutil/Sophia；powrprof |
| 9 | 定时关机 | sys.shutdown | P2 | 低 | 小工具补位；advapi32 |
| 10 | hosts 可编辑+自动备份 | sys.hosts 升级 | P1 | 低 | PowerToys Hosts File Editor |
| 11 | 环境变量可编辑 | sys.env 升级 | P2 | 低 | PowerToys Environment Variables |
| 12 | 文件占用定位（作为 R5b 失败项动作） | 挂在 R5b 内 | P1（随 R5b） | 中 | PowerToys File Locksmith / PE Find Handle；RM 已在 R5b 建设 |
| 13 | 启动项扩展（计划任务位置+签名） | sys.startup 增强 | P2 | 中 | Sysinternals Autoruns / Dism++ |
| 14 | DNS 切换预置列表 | net.dns 增强 | P2 | 低 | optimizer/winutil |

---

## 5. 安全命令可视化清单（R6 细化）

> 形态说明：「体检行」= sys.checkup 内一行只读检查（快、无副作用）；「独立模块」= 系统修复组 Action 模块（可执行、有日志控制台、可取消）。耗时为预估（以 256GB SSD + 千兆网络为基准）。

### 5.1 系统完整性 / 磁盘（独立模块为主）

| 命令/动作 | 作用 | 需管理员 | 可视化形态 | 预计耗时 | 风险与注意事项 | 归属 |
| --- | --- | --- | --- | --- | --- | --- |
| `sfc /scannow` | 校验并修复受保护系统文件 | 是 | 独立模块 fix.sfc；实时日志；不定进度 | 5~15 min | 中低。无法修复时提示转 DISM；部分修复需重启完成；取消=杀进程，需确认框警示 | 独立 |
| `DISM /Online /Cleanup-Image /RestoreHealth` | 用 Windows Update 源修复组件存储（CBS） | 是 | 独立模块 fix.dism | 10~30 min（取决网速） | 中。需联网；与 sfc 组合时先 DISM 后 sfc（与 winutil 编排一致）；取消同上 | 独立 |
| `DISM /Online /Cleanup-Image /StartComponentCleanup` | 清理组件存储旧版本（WinSxS 瘦身，常 1~3GB） | 是 | fix.dism 内第二个动作（同模块多按钮） | 2~5 min | 低。卸载更新后不可回退旧版本组件；与 R5a「WinSxS 仅统计」联动引导用户来此执行 | 独立 |
| `chkdsk C: /scan` | 在线只读磁盘错误扫描（不修复） | 是 | 独立模块 fix.chkdsk | 10~30 min（HDD 更久） | 低（只读）。发现问题提示改用 `/offlinescanandfix`；不能与碎片整理同时进行 | 独立 |
| `chkdsk C: /offlinescanandfix`（或 `/spotfix` 快速修） | 重启进入离线阶段修复文件系统错误 | 是 | fix.chkdsk 内选项（安排重启） | 重启耗时 5~30 min | 中。强制安排重启；修前明确告知不可中断断电风险 | 独立 |
| 系统版本/激活信息 `slmgr /xpr`、`slmgr /dlv` | 激活状态与许可证详情 | 否 | 体检行（解析 slmgr 输出，30 秒超时） | 秒级 | 无 | 体检行 |
| 系统还原点存在性与最近日期 | 判断回滚保险是否还在 | 否 | 体检行 | 秒级 | 无（只读检测，不列全部还原点） | 体检行 |

### 5.2 网络栈修复（独立模块，可勾选组合）

| 命令/动作 | 作用 | 需管理员 | 可视化形态 | 预计耗时 | 风险与注意事项 | 归属 |
| --- | --- | --- | --- | --- | --- | --- |
| `netsh winsock reset` | 重置 Winsock 目录（socket 层损坏、断网修复） | 是 | fix.netreset：4 个勾选项 + 「执行重置」按钮；逐项日志 | 秒级 | 中。**需重启生效**；重置后 VPN/加速器类软件可能需重装 | 独立 |
| `netsh int ip reset` | 重置 TCP/IP 协议栈 | 是 | 同上 | 秒级 | 中。需重启；自定义静态 IP/DNS 丢失需重配 | 独立 |
| `netsh int ipv6 reset` | 重置 IPv6 栈 | 是 | 同上 | 秒级 | 低~中。需重启 | 独立 |
| `netsh advfirewall reset` | 恢复防火墙默认规则 | 是 | 同上（**默认不勾选**，红字警示） | 秒级 | 中高。**全部自定义入站规则丢失**，远程桌面/共享访问可能断；单独勾选+红字+确认框 | 独立 |
| 各配置文件防火墙状态 | 域/专用/公用 是否启用 | 否 | 体检行（COM INetFwPolicy2 或 netsh 解析） | 秒级 | 无 | 体检行 |

### 5.3 Windows Update（体检行 + 独立模块）

| 命令/动作 | 作用 | 需管理员 | 可视化形态 | 预计耗时 | 风险与注意事项 | 归属 |
| --- | --- | --- | --- | --- | --- | --- |
| `usoclient StartScan` / `StartDownload` / `StartInstall` | 触发更新扫描/下载/安装 | 否（效果受限） | 独立模块 fix.wuupdate（按钮三分）+ 体检行「最近安装日期/是否待重启」 | 1~10 min | 低。安装更新可能需重启；无详细进度（usoclient 黑盒），需说明「进度请见设置面板」 | 独立+体检行 |
| WuReset 流程（停 wuauserv/BITS/appidsvc/cryptsvc → 重注册 wuaueng.dll 等 DLL → 清 SoftwareDistribution\Download → 重启服务） | 修复更新组件（0x8007xxxx 类故障） | 是 | fix.wuupdate 内「深度修复」动作，流程化日志 | 1~3 min | 中。清 Download 后待装更新需重下；建议先建还原点（R5c 联动）；操作不可逆 | 独立 |
| 最近安装更新日期 / 待重启标记 | 体检更新维度 | 否 | 体检行（注册表/WU API） | 秒级 | 无 | 体检行 |

### 5.4 Defender（独立模块）

| 命令/动作 | 作用 | 需管理员 | 可视化形态 | 预计耗时 | 风险与注意事项 | 归属 |
| --- | --- | --- | --- | --- | --- | --- |
| `MpCmdRun -SignatureUpdate` | 更新病毒定义 | 是 | 独立模块 fix.defender | 1~3 min | 低。需联网；MpCmdRun 位于 `%ProgramData%\Microsoft\Windows Defender\Platform\<版本>`，实现须动态解析最新版本目录 | 独立 |
| `MpCmdRun -Scan -ScanType 1` | 快速扫描 | 是 | fix.defender | 5~20 min | 低。检测到威胁按策略自动处置；`-Cancel` 可优雅取消 | 独立 |
| `MpCmdRun -Scan -ScanType 2` | 完整扫描 | 是 | fix.defender | 30 min~3 h | 低。CPU 默认 50%，可加 `-CpuThrottling`；本机使用会变慢属正常 | 独立 |
| `MpCmdRun -Scan -ScanType 3 -File <路径>` | 自定义路径扫描（U 盘/下载目录） | 是 | fix.defender（NeedsPath） | 视路径 | 低。`-BootSectorScan` 仅自定义扫描可带 | 独立 |
| Defender 状态 + 病毒库日期 | 是否在保护、定义是否过期 | 否 | 体检行（WMI `root\Microsoft\Windows\Defender` `MSFT_MpComputerStatus`；第三方杀软用 `root\SecurityCenter2` `AntiVirusProduct`） | 秒级 | 无。第三方 AV 场景需注明「非 Defender 主防」 | 体检行 |
| 关键服务状态（wuauserv/BITS/MsMpSvc/Schedule/WlanSvc） | 更新/防护/任务基础设施是否正常 | 否 | 体检行（EnumServicesStatusEx 复用现有代码） | 秒级 | 无 | 体检行 |

### 5.5 权限/策略（检测优先，修复 P2）

| 命令/动作 | 作用 | 需管理员 | 可视化形态 | 预计耗时 | 风险与注意事项 | 归属 |
| --- | --- | --- | --- | --- | --- | --- |
| `secedit /analyze`（对比 defltbase.inf） | 安全策略/权限漂移检测 | 是 | P2：fix.secop「检测」模式，输出差异行 | 1~2 min | 低（只读分析）。**不自动 /configure 修复** | P2 |
| `secedit /configure /cfg defltbase.inf` | 恢复默认安全策略 | 是 | P2：确认框红字 + 独立执行 | 1~2 min | 高。会覆盖自定义用户权限/ACL，仅高级用户显式选择 | P2 |
| `icacls <路径> /reset` 示例（重置 ACL） | 修复文件/目录权限错误 | 是 | P2： NeedsPath + 红确认 | 秒级 | 高。误用可导致系统目录不可访问；默认只对用户目录开放 | P2 |
| `slmgr /rearm` | 重置许可证状态（延长评估期） | 是 | **不做** | — | 高。消耗 rear 计数、不可逆、低频，纳入仅增加风险 | 不做 |
| hosts 文件异常检测 | 行数/可疑重定向计数 | 否 | 体检行（复用 sys.hosts 解析） | 秒级 | 无 | 体检行 |

### 5.6 编排规则（实现约束）

1. **顺序**：体检只做只读与秒级项；「System Corruption Scan」式编排（先 DISM 后 sfc）仅在用户分别执行时于文案中引导，不做强制串联；
2. **进度**：长任务一律不定进度 + 阶段文案（「正在校验系统文件…」），不用假进度条；
3. **取消**：允许取消但必须确认框明示「可能导致修复未完成」；MpCmdRun 用 `-Cancel`；
4. **提权**：非管理员执行给出「需要管理员权限」与一键提权入口（R1）；
5. **日志**：命令原文 + 逐行输出 + 退出码 + 结论行，全部入日志控制台（现有 Console 能力）。

---

## 6. 清理功能深化需求（R5 产品视角）

### 6.1 「全覆盖清理」类别清单

> 与 §3.5 R5a 对应；「默认清理/仅统计/按需」为产品分级。现有实现已覆盖的标注（已），需扩充的标注（新），危险项一律仅统计。

| 类别 | 子项 | 分级 | 现状/来源 |
| --- | --- | --- | --- |
| 临时文件 | 当前用户 %TEMP%（已）、`Windows\Temp`（已）、**其他用户 Temp（新，需管理员）**、Prefetch（已） | 默认清理 | ScanBasic |
| 回收站 | 各固定盘 `$Recycle.Bin`（已，用 RecycleBinSize 精确统计） | 默认清理（确认框列各盘大小） | ScanBasic |
| 浏览器缓存 | Chrome/Edge/Brave/Vivaldi/Opera/Opera GX/Chromium 的 Cache、Code Cache、**GPUCache、Service Worker\CacheStorage、GrShaderCache、Media Cache、DawnGraphCache/DawnScopedCache**；**多 Profile（Default+Profile N，新）**；Firefox 本地+配置缓存（已）；WebView2（已） | 默认清理（运行时跳过并提示） | ScanBasic 扩充 |
| 应用缓存 | 微信/QQ（已有专用模块）；Winapp2 规则第三方应用缓存（已有 clean.appcache，**扩充内置规则条目**） | 默认清理 | cleanmods/winapp2 |
| 系统日志 | `Windows\Logs\CBS`（已）、`Windows\Logs`（已，注意与 DISM 日志去重已处理）、DISM 日志（已）、`System32\LogFiles`（新细化：HTTPERR/SCM 等子目录）、WER 存档/队列/系统队列（已） | 默认清理 | ScanSystem 扩充 |
| 更新残留 | `SoftwareDistribution\Download`（已）、`DeliveryOptimization`（已）、**Windows.old（仅统计+引导 DISM/磁盘清理）**、**$WinREAgent（仅统计，隐藏目录需重取权限，引导用 fix.chkdsk/DISM 前手动处理）** | 混合 | ScanSystem 扩充 |
| 旧驱动 | `DriverStore\FileRepository` 孤立 oem 目录（**仅统计+建议**：无对应 INF 的旧驱动，提示走「设备管理器/驱动管理」）；NVIDIA 安装缓存 `C:\NVIDIA\CacheDriver`（新，默认清理，常 1~2GB） | 混合 | 新（Dism++ 思路） |
| 错误报告/转储 | `CrashDumps`（已）、`MEMORY.DMP`（已）、**Minidump（新）**、ReportQueue/ReportArchive（已） | 默认清理 | ScanBasic/System |
| 缩略图 | `Explorer\thumbcache_*.db`（已按目录统计，**改为按文件删（新）**：删 db 文件而非清空目录，避免误删 icon 缓存索引） | 默认清理 | ScanSystem 修正 |
| 系统缓存 | Defender 扫描/定义缓存（已）、FontCache（已）、搜索索引 Temp（已）、Media Player（已）、`Installer\$PatchCache$`（已）、WinSxS\ManifestCache（已，仅统计）、**DirectX Shader Cache（新，常 100~300MB，Dism++ 数据）**、传递优化（已） | 混合 | 扩充 |
| 系统关键项 | `Windows\Installer\*.msi/.msp`（已，仅统计）、`hiberfil.sys`/`pagefile.sys`/`WinSxS`/`System Volume Information`（已，仅统计+正确处置指引：powercfg/磁盘清理/DISM） | 仅统计 | 维持 |
| 用户目录 | 下载文件夹（已，仅统计）；**文档/桌面/图片等永不纳入自动清理** | 仅统计 | 维持红线 |
| OneDrive/其他 | OneDrive 日志（已）；Office 上传缓存（P2）；npm/pip/NuGet/VS 缓存（P2，开发者向，默认关闭） | 混合 | 维持+扩充 |

### 6.2 用户可感知的验收（产品指标）

1. **扫描覆盖率**：基准测试机构造 ≥20 类垃圾（每类已知大小），扫描命中 ≥18 类（≥90%）；同类目不重复计入；
2. **删除成功率**：默认策略 ≥95%；启用「结束占用进程后重试」≥99%；
3. **失败可解释**：失败项 100% 给出「错误码 + 中文原因 + 建议动作」三要素；
4. **释放量真实**：清理前后 `DirSize` 实测差值，误差 <5%；不再出现「显示释放 2GB、实际 800MB」；
5. **二次扫描干净**：成功项不再出现；被运行程序重新生成的项，结果中标注「程序运行中重新生成」；
6. **C 盘瘦身总控**：聚合全类别一键扫描（新增 `clean.cdrive`「C 盘瘦身」入口或现有模块的全选聚合），SSD ≤90s / HDD ≤180s，可取消；
7. **还原点保险**：清理/优化前默认创建还原点，失败不阻断但有提示。

### 6.3 风险控制（产品规则）

1. **仅统计红线**：系统关键项（hiberfil/pagefile/WinSxS/Installer msi/Windows.old/$WinREAgent/SVI）永不直接删除，只展示可释放空间与正确处置方式（对标现有 reportOnly 机制并扩充）；
2. **用户目录红线**：下载/文档/桌面/图片等用户数据区只统计不删；浏览器「密码/Cookie/登录数据」类规则一律排除（Winapp2 规则侧显式禁用，防 BleachBit 社区曾出现的误删登录数据问题）；
3. **白名单**：用户可对任一项标记「永不清理」（持久化到 exe 同目录 ini），扫描阶段即跳过（借鉴 BleachBit 排除机制；P1，低成本高信任价值）；
4. **删除方式**：默认硬删（与 Dism++ 一致，空间立释）；「移至回收站」作为选项（P1，仅对用户区类目生效，系统目录硬删）；**不做**多次覆写粉碎与空闲空间擦除（SSD 无意义且伤盘）；
5. **进程占用**：默认只报告不杀；「结束占用进程后重试」为用户显式勾选项，且只对可安全重启的进程（explorer/浏览器/办公软件）免确认，其余需确认框列名；
6. **还原点优先**：批量清理与所有 R6b 修复类操作前，默认尝试创建还原点（需 SR 服务开启+管理员），失败仅提示不阻断。

---

## 7. 待确认问题（≤5，含建议默认答案）

| # | 问题 | 建议默认答案（用户不回复即按此执行） |
| --- | --- | --- |
| Q1 | R1 提权失败降级时是否每次弹窗告知？ | **只提示一次**：首次降级时日志+胶囊常驻「标准用户」状态，之后静默降级，不反复弹窗打扰。 |
| Q2 | 结束进程/停止服务是否允许对「微软核心」对象操作？ | **核心清单硬拦截**（PID 0/4、csrss/wininit/services/smss/winlogon + 内置账户/管理共享/官方任务删除）；其余允许操作但确认框列名+危险红渲染。 |
| Q3 | 清理默认硬删还是进回收站？ | **默认硬删**（空间立释，与 Dism++ 一致）；「移至回收站」作为 P1 选项仅对用户区类目生效；系统目录类目永远硬删+仅统计红线兜底。 |
| Q4 | sfc/DISM/chkdsk 等长任务是否允许中途取消？ | **允许取消**，但取消前确认框明示「可能导致修复未完成，下次需重新执行」；MpCmdRun 扫描用 `-Cancel` 优雅取消；日志记录取消时点。 |
| Q5 | 事件日志/Windows 更新涉及较多 COM/WMI，人力不足时保哪个？ | **事件日志保 P1**（wevtapi 轻量、排障刚需）；**Windows 更新管理收缩为「扫描+历史+触发」P1**（安装下载执行留 P2）；防火墙规则「查看+启禁用」保 P1，「增删」降 P2。 |

---

## 8. 附：框架与实现提示（供架构师，非需求）

1. **新模块类型**：建议 `ModuleKind::Manageable`（扫描→勾选→多操作，按钮 ≤6，动态启用）；复用 `SetSelection` + `ModuleRow.tag` 行回溯；危险操作统一走 `ApplyWarning`/`ApplyIsDanger`。若不设新 Kind，用「Tweaks 型按钮扩展」等价实现亦可，由架构师定。
2. **新库依赖（全部为系统库，不违零依赖）**：taskschd（计划任务）、netapi32（账户/共享）、wevtapi（事件日志）、FirewallAPI/ole32（防火墙）、wuapi（Windows 更新）、powrprof（电源）、rstrtmgr（Restart Manager）、srclient（还原点，动态加载）、evr? 无。需在构建脚本核对 MinGW 导入库齐全性（taskschd/wuapi/rstrtmgr 需确认）。
3. **COM 初始化**：计划任务/防火墙/更新/还原点均需 COM；还原点需 `CoInitializeSecurity`（允许 LS/NS/SY/BA 回调，照 MSDN 示例）。
4. **子进程执行**：R6b 统一封装「建进程+管道重定向+逐行日志+取消」执行器（sfc/DISM/chkdsk/netsh/MpCmdRun/usoclient/WuReset 流程共用），是系统修复组的公共基础设施。
5. **MpCmdRun 路径**：`%ProgramData%\Microsoft\Windows Defender\Platform\<版本号>` 随更新变化，实现须枚举取最新版本目录；找不到时降级 `%ProgramFiles%\Windows Defender\MpCmdRun.exe`。
6. **清理引擎改造点**（R5b）：`CleanDirectoryContents`/`DeleteSingleFile` 增加逐项结果与错误码回传；新增占用检测（RM）与延迟删除（MoveFileEx）；「释放量」改实测；扫描侧扩充见 §6.1。
7. **分组命名**：新增「计划任务」「系统体检」「系统修复」「系统管理」四个分组，侧栏自动生成，无需改 UI 代码。
8. **体积预算**：新增 20+ 模块预计 +300~500KB 源码级增量，需守住 exe ≤2.5MB（G1~G8 的 G8）。

---

## 9. 参考来源

- hellzerg/optimizer：https://github.com/hellzerg/optimizer ；OptimizerNXT：https://github.com/hellzerg/optimizerNXT
- BleachBit：https://www.bleachbit.org/features ；winapp2.ini：https://github.com/MoscaDotTo/Winapp2
- Dism++：https://github.com/Chuyu-Team/Dism++ ；官网 https://www.dismplus.com/
- ChrisTitusTech/winutil：https://github.com/ChrisTitusTech/winutil ；功能介绍 https://christitus.com/windows-tool/
- Sophia Script：https://github.com/farag2/Sophia-Script-for-Windows
- Microsoft PowerToys：https://learn.microsoft.com/windows/powertoys/ ；Hosts File Editor：https://learn.microsoft.com/windows/powertoys/hosts-file-editor
- Sysinternals：https://www.sysinternals.com/ ；Process Utilities：https://docs.microsoft.com/sysinternals/downloads/process-utilities/
- 火绒安全软件 6.0 用户手册：https://www.huorong.cn/ （产品页与手册 PDF）
- MpCmdRun 命令行参数：https://code.msdn.microsoft.com/defender-endpoint/command-line-arguments-microsoft-defender-antivirus
- Task Scheduler 2.0（枚举任务示例）：https://learn.microsoft.com/windows/win32/taskschd/displaying-task-names-and-state--c--
- SRSetRestorePoint：https://learn.microsoft.com/windows/win32/sr/calling-srsetrestorepoint
- Windows 11 精简工具横评（optimizer 归档信息）：https://trendingreview.com/best-windows-11-debloater-tools

> 未找到公开资料的项目/功能：BleachBit 6.0.2 之后的路线图细节、Dism++ 各版本完整清理规则清单、火绒「安全分析工具」报告字段明细——文中相关表述已按公开可得信息收敛，未臆造。

（完）
