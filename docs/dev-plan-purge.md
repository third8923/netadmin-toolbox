# 开发计划 · 流氓软件净化器（Rogue Purger）

**项目**：网管工具箱 NetAdmin Toolbox —「净化中心」新分组
**文档版本**：v1.1（2026-10-26 · M1 已交付）
**性质**：调研 + 增量开发规划
**状态**：✅ M1 已随 v2.13 交付（用户确认四项决策：①集成进工具箱新分组 ②M1 先做
purg.scan ③特征库 SoftCnKiller + 自有补充 ④**在线更新提前至 M1**——原计划 M4，
要求双源更新：SoftCnKiller 数据仓库 + 本项目 GitHub 自有规则仓库）

---

## 一、参考文章分析：8 款工具能力矩阵

| 工具 | 核心定位 | 可借鉴点 | 不可借鉴/短板 |
|---|---|---|---|
| Ashampoo UnInstaller 14（商业） | **安装监控快照**：装前后系统 diff，精确记录安装产生的文件/注册表 → 卸载零残留 | 安装快照 diff 思想、浏览器插件检测、批量卸载 | 闭源收费，不能抄实现 |
| Geek Uninstaller | 6MB 绿色单文件，原生卸载 + 残留扫描 | 单文件绿色定位（与我们产品哲学一致） | 功能面窄 |
| HiBit Uninstaller | 单文件便携、功能最全（商店应用/扩展/注册表/右键菜单/计划任务管理） | 功能清单是很好的"验收对照表" | 闭源 |
| Uninstall Tool | 隐藏安装项显示、强制卸载、HTML 报告 | 隐藏项枚举、报告导出 | 商业 |
| **SoftCnKiller（重点对标）** | **特征库查杀**：读取启动项/服务/驱动/计划任务/右键/输入法/文件关联的数字签名与目录名，比对 sign.txt/folder.txt 黑名单一键处置；adwview 弹窗定位；规则在线更新 | 整体查杀思路 + 分级处置策略 + 特征库格式 + 弹窗定位交互 | MFC 闭源；对 360/2345 等有驱动保护的无效；不删程序本体 |
| Revo Uninstaller | 先进算法全量扫描注册表/文件残留 | 残留深度扫描算法思想 | 商业 |
| Wise Force Deleter | 句柄解锁 + 强制删除 | 强删交互（拖拽/解锁确认） | 单一功能 |
| Wise Care 365 | 实时保护注册表、隐私擦除、进程监控 | 实时防护层思路（注册表 Run 键监视） | 全家桶式，风格不符 |

**结论**：文章 8 款覆盖了三层能力——**检测**（全量枚举安装/自启点）、**识别**（签名/目录特征黑名单 + 行为启发式）、**处置**（原生卸载→静默卸载→强删→残留清理→复活防护）。市面工具没有一个同时做全三层且开源；我们的机会 = SoftCnKiller 的本土特征库 + BCUninstaller 的批量卸载框架 + 自研弹窗定位与复活监控，全部纳入工具箱单文件形态。

---

## 二、可借鉴的开源项目与规则库（已逐一核实）

### 2.1 工具类开源项目

| 项目 | 协议 | 语言 | 借鉴价值 |
|---|---|---|---|
| **BCUninstaller**<br>github.com/BCUninstaller/Bulk-Crap-Uninstaller | Apache 2.0 | C# | ★★★★★ 批量卸载框架的教科书：UninstallerType 十四类枚举（Msiexec/NSIS/InnoSetup/InstallShield/StoreApp/Steam/Chocolatey/WindowsFeature…）、**静默卸载参数自动生成**、残留扫描、**卸载器数字证书校验**（信任分色）、正在找 maintainer，可直接贡献/移植思路 |
| **System Informer**（原 Process Hacker）<br>github.com/winsiderss/systeminformer | MIT | C/C++ | ★★★★★ 底层能力宝库：句柄枚举与关闭（文件占用解锁）、隐藏进程检测（DKOM）、服务/驱动管理、内核调用栈、KSystemInformer 驱动源码、进程树归因 |
| Kill-Malware / Rogueware-Killer | 开源（脚本） | PowerShell/BAT | ★★★ 证书拉黑（cer 目录 → 禁用流氓签名）、安装目录屏蔽、hosts 屏蔽、火绒拦截规则 JSON——"预防层"策略参考 |
| Dism++（已停更） | MIT | C/C++ | ★★ 清理引擎与装机思路（其架构文档仍有价值） |

### 2.2 规则库（直接可用）

| 规则库 | 地址 | 格式 | 用途 |
|---|---|---|---|
| **SoftCnKiller 数据仓库**（四海一叶秋，持续更新中，2026-10 仍在更新） | gitee.com/softcnkiller/data<br>github.com/SiHaiYiYeQiu/SoftCnKiller | sign.txt（数字签名黑名单）<br>folder.txt（目录特征）<br>whitepath.txt（白名单）<br>3000+ 特征 | **查杀特征库主数据源**，格式极简可直接适配解析 |
| anti-AD | github.com/privacy-protection-tools/anti-AD | hosts / easylist | 广告/弹窗回连域名屏蔽（写入 hosts 或防火墙） |
| yhosts | github.com/vokins/yhosts | hosts | 国内站/APP 广告域名 |
| 大圣净化 | github.com/jdlingyu/ad-wars | hosts | 国内视频网站广告域名 |
| neoHosts | hosts.nfz.moe | hosts | 挖矿/统计/劫持域名 |
| StevenBlack/hosts | github.com/StevenBlack/hosts | hosts（多源聚合） | 国际广告+恶意域名聚合 |
| Neo23x0/signature-base | github.com/Neo23x0/signature-base | YARA + IOC（**DRL 1.1 许可，商用需审阅条款**） | PUP/adware 文件级 YARA 扫描规则（进阶） |
| 火绒全家桶拦截规则 | bbs.huorong.cn（社区规则 JSON） | 火绒自定义防护 JSON | 拦截规则语义参考（转译为我们自己的规则格式） |

### 2.3 闭源但方法论可学的对标

- **Malwarebytes AdwCleaner**（免费闭源）：**隔离区（Quarantine）+ 可还原**模型、浏览器策略重置、命令行 `/scan /clean`——安全工具的工程范式标准。
- **Sysinternals MoveFile/PendMoves**：`PendingFileRenameOperations` 重启删除（微软官方安全强删通道，我们已掌握其原理）。

---

## 三、产品定义：净化中心（拟 6 模块）

定位：**单文件、绿色、可回滚、以国内流氓软件为主要打击对象**的检测-识别-处置一体化工具。

| # | 模块 ID | 名称 | 功能 |
|---|---|---|---|
| 1 | `purg.scan` | **全盘查杀** | 枚举进程/服务/驱动/启动项/计划任务/右键菜单(ShellEx)/输入法/文件关联/浏览器扩展/hosts/代理，按签名+目录特征库匹配 → 分类列出 → 分级处置（对齐 SoftCnKiller 处置语义：服务禁用、驱动停+手动、任务删除、启动项删、ShellEx 移除、关联恢复） |
| 2 | `purg.uninst` | **卸载中心** | 安装清单（注册表含隐藏项 + MSI API + UWP + 商店应用）→ 原生卸载/静默卸载（参数库自动生成）→ 残留扫描（文件+注册表）→ 一键清残留 |
| 3 | `purg.popup` | **弹窗定位** | 拖拽准星取窗口（WindowFromPoint + WinEventHook 双通道）→ 归属进程/路径/签名 → 关联自启项扫描 → 一键清理（复用查杀引擎） |
| 4 | `purg.unlock` | **强删/解锁** | 文件占用句柄定位 → 四阶梯强删：直删 → ACL/属性修复 → 句柄关闭 → `PendingFileRenameOperations` 重启删除 |
| 5 | `purg.net` | **劫持修复** | 主页劫持/搜索引擎劫持/快捷方式篡改检测修复、hosts 篡改检测、代理劫持检测、可选写入屏蔽域名（anti-AD 等规则可选订阅） |
| 6 | `purg.guard` | **复活监控**（可选进阶） | RegNotifyChangeKeyValue 监视 Run 键/服务键、安装快照 diff（装前装后对比，Ashampoo 思想轻量版）、复活告警 |

**规则引擎（贯穿）**：统一 JSON 规则包，四类规则——签名（含证书指纹）、路径/目录、文件名/资源、注册表键；内置 SoftCnKiller 特征库（含来源致谢）；支持白名单；支持从 gitee/GitHub raw 在线更新（WinHTTP，校验签名/哈希）。
**安全工程（贯穿）**：所有处置先写**操作日志 + 注册表项导出备份**（可一键还原）；可选隔离区（移入受控目录）；高危动作红色确认；绝不删除程序本体以外的个人文件。

---

## 四、开发所需高级编程技能（研究结论）

| 领域 | 具体技术 | 用在哪个模块 | 我们现状 |
|---|---|---|---|
| Win32 深度 | WinVerifyTrust/Authenticode 签名验证、WinEventHook、WindowFromPoint、ShellExecuteEx | 查杀/弹窗定位 | 大部分熟悉，签名验证为新增 |
| COM 自动化 | ITaskService（已掌握）、IPackageManager（UWP）、MsiEnumProducts*（MSI）、IPersistFile（lnk 解析） | 卸载中心/查杀 | 计划任务 COM 已实战过 |
| ACL 与所有权 | SetNamedSecurityInfo/GetNamedSecurityInfo、TakeOwnership 阶梯 | 强删/解锁 | 清理引擎 DeleteFileSmart 已有 L1-L4 基础 |
| 句柄技术 | NtQuerySystemInformation(SystemHandleInformation) 枚举句柄、DuplicateHandle+关闭（System Informer 同款） | 强删/解锁 | **全新学习**（System Informer 源码可参考，MIT） |
| 重启删除 | PendingFileRenameOperations REG_MULTI_SZ 格式 | 强删/解锁 | 原理已研究透，实现简单 |
| 注册表监视 | RegNotifyChangeKeyValue 多线程监视树 | 复活监控 | 新增但门槛低 |
| 浏览器解剖 | Chromium `Secure Preferences`/`Preferences` JSON 解析、Edge/Firefox profiles | 查杀/劫持修复 | 新增，需处理 HMAC 校验 |
| 网络更新 | WinHTTP + 证书校验 + 哈希验证 | 规则更新 | 新增（简单） |
| 测试工程 | 虚拟机快照 + 真实样本回归集 | 全部 | 沿用 QA 严过关流程 |

**前沿技术研判**：
1. **不建议上内核驱动**（v1）——流氓软件净化属 PUP 范畴，用户态 API + 句柄技术 + 重启删除已覆盖 95% 场景；驱动会引入签名/误报/蓝屏风险（SoftCnKiller 无驱动即成事）。M5 若遇 360/2345 级驱动保护，再评估参考 KSystemInformer（MIT）的最小驱动方案。
2. **YARA 集成缓行**——yara.lib 体积与规则许可（DRL 1.1）需法务审阅；v1 用"签名+路径+资源串"三通道特征已够，M5 再评估。
3. **不驻后台主动防御**（v1）——与工具箱"按需扫描、退出即走"的产品哲学一致；复活监控做成可选轻量监视线程而非常驻服务。

---

## 五、里程碑（每版独立可交付、可回归）

| 里程碑 | 内容 | 依赖 |
|---|---|---|
| **M1（v2.13 ✅ 已交付）** `purg.scan` 全盘查杀 + 在线更新 | 全量自启点枚举 + SoftCnKiller 特征库适配（sign/folder/whitepath 解析器）+ 数字签名验证 + 分级处置 + 操作日志/还原 + **WinHTTP 双源在线更新（原 M4 内容，用户要求提前）** | 无（复用 IModule 框架、procrun、common） |
| **M2（拟 v2.14）** `purg.uninst` 卸载中心 + `purg.unlock` 强删 | 安装清单 + 静默参数库（NSIS /S、Inno /VERYSILENT、MSI /qn、InstallShield）+ 残留扫描 + 句柄解锁 + 重启删除 | M1 |
| **M3（拟 v2.15）** `purg.popup` 弹窗定位 + `purg.net` 劫持修复 | 准星取窗 + 归因 + Chromium 扩展解析 + 主页/关联/hosts/代理修复 | M1 |
| **M4（拟 v2.16）** `purg.guard` 复活监控 + 规则在线更新 | RegNotify 监视 + 快照 diff + WinHTTP 规则订阅 | M1-M3 |
| **M5（远期评估）**（✅ v2.14 已落地：YARA 子集引擎自研零依赖、驱动增强以「内核模块表+隐藏进程检测」用户态方案实现、主页锁定独立模块；完整内核驱动与 libyara 完整版维持评估） YARA 扫描 / 驱动增强 / 主页锁定 | 视 M1-M4 用户反馈决定 | M4 |

**M1 任务分解（T01-T06，全部完成 ✅）**：
- ✅ T01 特征库引擎：`ruleset.h/cpp` —— 三类特征文件解析（UTF-16LE/UTF-8/GBK 自适应）+ 匹配索引 + 白名单；特征数据 **base64 内嵌**（规避 Defender PUA:Win32/Softcnapp 内容误报），rules\ 目录覆盖层支持更新与自有补充（extra_*.txt）
- ✅ T02 枚举层：`purgcore.h/cpp` —— 11 类点全量枚举 + WinVerifyTrust 签名验证 + 证书主体提取（CryptQueryObject→CMSG_SIGNER_INFO→CertGetNameString）+ 进程内缓存
- ✅ T03 处置层：先备份（purg-backup.tsv 转义 TSV）后分级处置 + 「还原上次处置」倒序回滚
- ✅ T04 GUI：净化中心分组（安全体检后）+ purg.scan（Tweaks+Actions 组合：处置选中/还原/更多 ▾）+ RunLabel 框架修复（Tweaks 也尊重 RunLabel）
- ✅ T05 在线更新：WinHTTP 双源（gitee SoftCnKiller + GitHub 自有规则）+ 行数门槛校验 + SHA256 记录 + rules\update.ini 可配置
- ✅ T06 测试：tools/t213_gui_check.ps1（GUI 全断言 PASS）+ tools/gen_softcn.py 数据生成器 + 四步判定实测（微软 0 误报/签名·白名单·目录三通道受控命中验证）+ 版本四处同步 2.13

---

## 六、风险与对策

| 风险 | 对策 |
|---|---|
| 误杀系统组件 | 微软/WHQL 签名默认放行 + whitepath 白名单 + 处置前注册表备份可还原 |
| 特征库版权 | SoftCnKiller 数据仓库公开且作者持续维护，引用时**致谢并注明来源**，格式适配不改内容 |
| 杀软误报我们 | 代码签名（后续）、行为透明（全日志）、提交误报申诉 |
| 360/2345 级驱动保护 | v1 明确不支持并提示；留 M5 评估口 |
| 范围膨胀 | 严格按 M1-M4 里程碑，每个里程碑独立发版 |

---

## 七、待确认事项（开工前回答）

1. **集成方式**：作为工具箱 v2.13 新分组「净化中心」（推荐，复用全部基础设施）还是独立单文件产品？
2. **M1 范围确认**：是否同意先做 `purg.scan`（最快见效、对标 SoftCnKiller 核心）？
3. **特征库来源**：默认内置 SoftCnKiller 数据仓库特征 + 自有补充，还是全部自建？
4. **在线更新**：M1 暂不做，规则文件随 exe 目录分发（M4 再做在线订阅）——是否接受？
