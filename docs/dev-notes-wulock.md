# 开发笔记 · purg.wulock 系统更新锁定（v2.15 需求 1）

> 调研目标：还原 ghost / ESD 封装镜像中「锁定 Windows 更新」的完整手法，
> 并给出本工具箱的封锁 / 还原对照实现。调研时间：v2.15 开发期。

## 一、封装镜像锁更新的完整手法（四层）

### ① 组策略注册表封锁（核心层）

键：`HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate`
及其 AU 子键：`HKLM\...\WindowsUpdate\AU`

| 值 | 含义 | 锁定值 |
| --- | --- | --- |
| `NoAutoUpdate` (DWORD) | 关闭自动更新 | 1 |
| `DisableWindowsUpdateAccess` (DWORD) | 禁用 WU 访问（UX 与 API 双封） | 1 |
| `DoNotConnectToWindowsUpdateInternetLocations` (DWORD) | 禁止连接 WU 互联网位置（内网 WSUS 语义，公网同样生效） | 1 |
| `SetDisableUXWUAccess` (DWORD) | 设置页隐藏「检查更新」按钮（AU 子键） | 1 |
| `ExcludeWUDriversInQualityUpdate` (DWORD) | 质量更新不携带驱动 | 1 |

封装镜像通常还会顺手写 `WUServer` / `WUStatusServer` 指向 127.0.0.1 的假 WSUS，
本模块扫描时一并列报，锁定时不写假地址（留空即可，`DoNotConnect...=1` 已足够）。

### ② 服务禁用（自愈对抗层）

| 服务 | 默认启动类型 | 说明 |
| --- | --- | --- |
| `wuauserv` | 手动(demand) | Windows Update 主服务 |
| `UsoSvc` | 自动(auto) | 更新会话编排器 |
| `WaaSMedicSvc` | 手动(demand) | **Windows 更新医助——被 ACL 保护，ChangeServiceConfig 会被拒绝**，需直写注册表 `HKLM\SYSTEM\CurrentControlSet\Services\WaaSMedicSvc` 的 `Start=4`（禁用） |
| `DoSvc` | 自动(延迟) | 传递优化 |

WaaSMedicSvc 自愈对抗：其 `FailureActions`（REG_BINARY）中含 `01 00 00 00`
（SC_ACTION_RESTART），服务被停后会自动拉起。封锁时把该动作字节改为
`00 00 00 00`（SC_ACTION_NONE），防止「禁用了又被医活」。
（该键被 TrustedInstaller 保护，需先取得所有权再改，或以 SYSTEM 权限写入；
非提权时如实报拒绝并计数。）

### ③ 计划任务禁用

两个目录的全部任务置 Enabled=false（taskschd COM `put_Enabled`）：

* `\Microsoft\Windows\WindowsUpdate\*`（Automatic App Update、Scheduled Start、SIH 等）
* `\Microsoft\Windows\UpdateOrchestrator\*`（Universal Orchestrator Start、
  Schedule Scan、Workstation 等十几个编排任务）

个别任务受保护拒绝时记录跳过（不中断、不伪造成功）。

### ④ hosts 域名拦截（可选开关，默认关）

向 `C:\Windows\System32\drivers\etc\hosts` 追加带标记注释块：

```
# >>> NetAdmin Toolbox WU Lock (v2.15) >>>
127.0.0.1 download.windowsupdate.com
127.0.0.1 update.microsoft.com
...（少量常用更新域名，不堆砌长名单以免误伤/杀软误报）
# <<< NetAdmin Toolbox WU Lock <<<
```

还原 = 删除两标记行之间的整块。注意：仅 `DoNotConnectToWindowsUpdateInternetLocations`
类策略封锁已足够时不必启用此项（hosts 拦截对使用 CDN 的直连域名时效性差，作为兜底）。

### ⑤ 主页锁定手法（引用 v2.14，不在本模块重复实现）

ghost 封装同源的「主页锁定」三件套——SetupComplete.cmd 写 Start Page、
Default User 的 NTUSER.DAT、浏览器策略 Homepage——已由 v2.14 `purg.homepage`
覆盖（扫描+修复+策略锁/解锁），本模块不重复实现，见 purgmods2.cpp。

## 二、本模块实现对照（purgmods3.cpp / purg.wulock）

| 封装手法 | 扫描（Run） | 锁定（Actions[0]） | 还原（Actions[1]） |
| --- | --- | --- | --- |
| 策略封锁 | 枚举 WindowsUpdate 与 AU 键全部值 | 写 5 个封锁值 | 按备份 TSV 写回原值/删值 |
| 服务禁用 | OpenSCManager + QueryServiceConfig + QueryServiceStatus（不用 sc.exe，沙箱黑名单） | ChangeServiceConfig→DISABLED，被拒（WaaSMedicSvc）则直写 Start=4 + FailureActions 01→00，尝试 ControlService STOP | Start 恢复：wuauserv=demand / UsoSvc=auto / WaaSMedicSvc=demand / DoSvc=auto（备份有原值时按原值） |
| 计划任务 | taskschd COM 枚举两目录任务状态（MinGW get_Item 坑：Task/FolderCollection 用 VARIANT 下标，见 taskmods.cpp 范式） | 全部 put_Enabled(false)，个别拒绝记录跳过 | put_Enabled(true) |
| hosts 拦截 | 检查标记块是否存在 | Field 勾选（填 yes/开，默认关）才追加 | 移除标记块 |

* 备份：exe 目录 `wulock-backup.tsv`，行格式
  `SET \t HKLM \t 子键 \t 值名 \t 类型 \t 数据` / `SVC \t 名称 \t 原Start` /
  `TASK \t 任务路径 \t 原Enabled`（复用 purgcore `EscapeField` 转义）。
* 权限：CLI 永不提权；HKLM/服务/任务写失败如实计数并用 ErrorTextZh 中文归因
  （与 purg.homepage 同语义）。GUI 用户可用右上角「以管理员重启」后重试。
