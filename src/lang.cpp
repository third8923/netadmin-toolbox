// ============================================================================
//  NetAdmin Toolbox — 多语言层实现（v2.12 R4 / v2.15 i18n 补全）
// ---------------------------------------------------------------------------
//  设计：
//    * 简体中文是「源语言」——全部界面/模块字符串本来就以简体写成，ZhCn 原样返回。
//    * 繁体中文：LCMapStringW(LCMAP_TRADITIONAL_CHINESE) 由操作系统逐字转换，
//      覆盖所有经过本层翻译入口的字符串（OS 内置映射表，零维护成本）。
//    * 英文：内置查表（v2.15 起覆盖界面主干 + 全部分组名 + 全部模块标题 +
//      动作按钮/字段标签与占位提示/确认框正文/常用状态词）；
//      表中未登记的字符串回退显示简体原文（运行日志动态拼接暂随中文）。
//  查找性能：kEn 为线性表，v2.15 起首次查找时构建排序索引并二分查找
//  （O(log n)，300+ 条目下较逐条 wcscmp 快一个数量级）。
//  翻译入口只有两个：
//    lang::T(const wchar_t* zh)  -> const wchar_t*（持久指针，UI 直接绘制）
//    lang::Tx(const wstring& zh) -> wstring（拼接场景）
//  语言选择持久化在 <exe>-theme.ini 的 [ui] lang（zh-CN / zh-TW / en-US）。
//  注意：kEn 键必须与调用点源串逐字节一致，否则查不到（回退中文）。
// ============================================================================
#include "lang.h"

#include <unordered_map>
#include <algorithm>

namespace mz {
namespace lang {

namespace {

Lang g_lang = Lang::ZhCn;

// 翻译缓存：zh -> 目标语言串。unordered_map 节点地址稳定，
// T() 返回缓存内指针供绘制层直接使用。
std::unordered_map<std::wstring, std::wstring>& Cache() {
    static std::unordered_map<std::wstring, std::wstring> c;
    return c;
}

// 英文表：键 = 简体原文（必须与调用点字符串逐字节一致）。
// 覆盖范围（v2.15）：界面主干 / 15 个分组名 / 全部模块标题 / 动作按钮 /
// 字段标签与占位提示 / 确认框正文 / 表格常用状态词。
struct Pair { const wchar_t* zh; const wchar_t* en; };
const Pair kEn[] = {
    // ---- 界面主干 ----
    { L"开始扫描",        L"Start Scan" },
    { L"执行清理",        L"Run Clean" },
    { L"还原选中",        L"Restore Selected" },
    { L"停止",            L"Stop" },
    { L"全选",            L"Select All" },
    { L"取消全选",        L"Clear All" },
    { L"更多 ▾",          L"More ▾" },
    { L"以管理员重启",    L"Restart as Admin" },
    { L"关于",            L"About" },
    { L"主题",            L"Theme" },
    { L"全部展开",        L"Expand All" },
    { L"全部收起",        L"Collapse All" },
    { L"搜索功能…  (Ctrl+F)", L"Search…  (Ctrl+F)" },
    { L"没有匹配的功能",  L"No matching tools" },
    { L"路径（留空 = 默认）", L"Path (blank = default)" },
    { L"目标 / 参数",     L"Target / params" },
    { L"执行",            L"Run" },
    { L"应用选中",        L"Apply Selected" },
    { L"卸载选中",        L"Uninstall Selected" },
    { L"安装选中",        L"Install Selected" },
    { L"升级选中",        L"Update Selected" },
    { L"确认执行",        L"Confirm" },
    { L"确认执行操作",    L"Confirm Action" },
    { L"确认执行清理",    L"Confirm Clean" },
    { L"取消",            L"Cancel" },
    { L"确定",            L"OK" },
    { L"完成",            L"Done" },
    { L"管理员模式",      L"Administrator" },
    { L"标准用户",        L"Standard user" },
    { L"扫描结果",        L"Results" },
    { L"运行日志",        L"Log" },
    { L"暂无数据",        L"No data" },
    { L"%d 条记录",       L"%d items" },
    { L"就绪",            L"Ready" },
    { L"运行中 · Esc 可停止", L"Running · Esc to stop" },
    { L"还没有数据",      L"Nothing here yet" },
    { L"点击上方「执行」运行该功能", L"Click Run above to start this tool" },
    { L"点击上方「开始扫描」获取当前模块的数据", L"Click Start Scan above to load data" },
    { L"名称",            L"Name" },
    { L"数值",            L"Value" },
    { L"分类",            L"Category" },
    { L"详情",            L"Details" },
    { L"界面主题",        L"Theme" },
    { L"选择界面主题   ·   Ctrl+T 快速循环切换，选择自动保存",
      L"Pick a theme  ·  Ctrl+T cycles, selection is saved" },
    { L"当前",            L"Active" },
    { L"biohacker.fun 旗舰产品", L"biohacker.fun flagship" },
    { L"单文件免安装，无服务、无驱动、不写系统目录",
      L"Single file, portable; no services, drivers or system files" },
    { L"全部本地执行，不上传任何数据",
      L"Runs fully offline; nothing is uploaded" },
    { L"%d 个功能模块，覆盖清理 / 网络 / 系统信息",
      L"%d tools covering cleanup / network / system info" },
    { L"%d 个模块    界面 %d%%", L"%d tools    UI %d%%" },
    { L"简体中文",        L"简体中文" },
    { L"繁體中文",        L"繁體中文" },
    { L"English",         L"English" },
    { L"保存(&S)",        L"&Save" },
    { L"取消(&C)",        L"&Cancel" },
    { L"动作",            L"Action" },
    { L"编辑 hosts（保存前自动备份为 hosts.bak）",
      L"Edit hosts (auto backup to hosts.bak before saving)" },
    // ---- VI-D2 v2.17（空态/错误态/四态叙事/主题名/字体脚注）----
    { L"主题：",          L"Theme: " },
    { L"当前主题：",      L"Active theme: " },
    { L"信号没到，线就不画完",
      L"No signal yet — the trace stays open" },
    { L"信号在中途断了",  L"The signal broke midway" },
    { L"检查网络、权限或参数后重试",
      L"Check network, permissions or parameters, then retry" },
    { L"蜕变四态",        L"Four Stages of Metamorphosis" },
    { L"卵 v0.1",         L"Egg · v0.1" },
    { L"幼虫 v0.5",       L"Larva · v0.5" },
    { L"蛹 v0.9",         L"Pupa · v0.9" },
    { L"蝶 v1.0",         L"Butterfly · v1.0" },
    { L"当前版本",        L"Current version" },
    { L"纸亮",            L"Paper Light" },
    { L"墨暗",            L"Ink Dark" },
    { L"纸底 + 墨字 + 藤紫 + 金信号",
      L"Paper canvas, ink text, wing purple, gold signal" },
    { L"墨底 + 纸字 + 金信号 + 提亮紫",
      L"Ink canvas, paper text, gold signal, lifted purple" },
    { L"Baloo 2 / JetBrains Mono 以 SIL Open Font License 1.1 子集形式内嵌",
      L"Baloo 2 / JetBrains Mono embedded as subsets (SIL OFL 1.1)" },
    { L"暂无日志",        L"No log yet" },
    // ---- 分组名 ----
    { L"磁盘清理",        L"Disk Cleanup" },
    { L"空间分析",        L"Space Analysis" },
    { L"网络管理",        L"Network Mgmt" },
    { L"远程终端",        L"Remote Terminal" },
    { L"网络诊断",        L"Diagnostics" },
    { L"系统管理",        L"System Admin" },
    { L"系统信息",        L"System Info" },
    { L"常用管理",        L"Common Tools" },
    { L"软件管家",        L"Software Hub" },
    { L"安全体检",        L"Security Check" },
    { L"净化中心",        L"Rogue Purger" },
    { L"密码工具",        L"Password Tools" },
    { L"高级工具",        L"Advanced" },
    { L"系统优化",        L"System Tweaks" },
    { L"系统维护",        L"Maintenance" },
    // ---- 模块标题 ----
    { L"系统概览",        L"System Overview" },
    { L"磁盘信息",        L"Disks" },
    { L"进程管理",        L"Processes" },
    { L"启动项",          L"Startup Items" },
    { L"服务管理",        L"Services" },
    { L"计划任务",        L"Scheduled Tasks" },
    { L"已安装补丁",      L"Hotfixes" },
    { L"环境变量",        L"Environment Variables" },
    { L"hosts 文件",      L"hosts File" },
    { L"网络适配器",      L"Adapters" },
    { L"网络连接",        L"Connections" },
    { L"端口扫描",        L"Port Scan" },
    { L"局域网扫描",      L"LAN Scan" },
    { L"路由表",          L"Route Table" },
    { L"ARP 表",          L"ARP Table" },
    { L"刷新 DNS 缓存",   L"Flush DNS" },
    { L"无线网络",        L"Wi-Fi" },
    { L"DNS 优化",        L"DNS Optimizer" },
    { L"组网助手",        L"Mesh Assistant" },
    { L"远程会话",        L"Remote Sessions" },
    { L"端口转发隧道",    L"Port Forward Tunnels" },
    { L"串口终端",        L"Serial Terminal" },
    { L"Windows 优化",    L"Windows Tweaks" },
    { L"软件搬家",        L"Move Software" },
    { L"用户目录迁移",    L"Migrate User Folders" },
    { L"一键体检",        L"Quick Checkup" },
    { L"系统修复",        L"System Repair" },
    { L"本地用户",        L"Local Users" },
    { L"共享目录",        L"Shares" },
    { L"事件日志分析",    L"Event Log" },
    { L"电源计划",        L"Power Plans" },
    { L"缺失更新检查",    L"Missing Updates" },
    { L"装机必备 · 推荐总榜", L"Essentials · Top Picks" },
    { L"浏览器上网",      L"Browsers" },
    { L"压缩与文件管理",  L"Archivers & Files" },
    { L"办公与效率",      L"Office & Productivity" },
    { L"开发与运维",      L"Dev & Ops" },
    { L"影音与图像",      L"Media & Imaging" },
    { L"通讯与社交",      L"Communication" },
    { L"系统与安全",      L"System & Security" },
    { L"系统还原点",      L"Restore Points" },
    { L"软件搜索",        L"Software Search" },
    { L"已安装软件",      L"Installed Apps" },
    { L"软件升级",        L"App Updates" },
    { L"账户部署",        L"Account Setup" },
    { L"计算机名与域",    L"Computer Name & Domain" },
    { L"共享部署",        L"Share Hosting" },
    { L"共享扫描与网盘映射", L"Share Scan & Map" },
    { L"星号密码查看器",  L"Password Reveal" },
    { L"ZIP 密码恢复",    L"ZIP Password Recovery" },
    { L"凭据管理器",      L"Credential Manager" },
    { L"WiFi 密码查看",   L"Wi-Fi Passwords" },
    { L"全盘查杀",        L"Rogue Scan" },
    { L"规则库生成",      L"Custom Rules" },
    { L"YARA 扫描",       L"YARA Scan" },
    { L"主页锁定",        L"Homepage Lock" },
    { L"系统更新锁定",    L"Windows Update Lock" },
    { L"大文件扫描",      L"Large Files" },
    { L"重复文件",        L"Duplicates" },
    { L"系统清理",        L"System Cleanup" },
    { L"基础清理",        L"Basic Cleanup" },
    { L"QQ 清理",         L"QQ Cleanup" },
    { L"微信清理",        L"WeChat Cleanup" },
    { L"应用缓存",        L"App Caches" },
    { L"相似图片",        L"Similar Images" },
    // ---- 主按钮 / RunLabel ----
    { L"开始查杀",        L"Start Scan" },
    { L"处置选中项",      L"Handle Selected" },
    { L"还原上次处置",    L"Undo Last Action" },
    { L"在线更新特征库",  L"Update Rules Online" },
    { L"刷新规则列表",    L"Refresh Rules" },
    { L"添加规则",        L"Add Rule" },
    { L"删除选中规则",    L"Delete Selected Rules" },
    { L"打开规则目录",    L"Open Rules Folder" },
    { L"导出规则包",      L"Export Rule Pack" },
    { L"格式说明",        L"Format Help" },
    { L"仅清理快捷方式劫持", L"Clean Shortcut Hijacks Only" },
    { L"特征库信息",      L"Signature DB Info" },
    { L"打开特征库目录",  L"Open Signature DB Folder" },
    { L"扫描劫持",        L"Scan Hijacks" },
    { L"修复并设置主页",  L"Fix & Set Homepage" },
    { L"还原上次修改",    L"Restore Last Change" },
    { L"锁定主页",        L"Lock Homepage" },
    { L"解除主页锁定",    L"Unlock Homepage" },
    { L"扫描更新状态",    L"Scan Update Status" },
    { L"锁定系统更新",    L"Lock Windows Update" },
    { L"还原更新锁定",    L"Restore Update Lock" },
    { L"开始测速",        L"Run Speed Test" },
    { L"自动优选",        L"Auto Pick Fastest" },
    { L"加入网络",        L"Join Network" },
    { L"退出网络",        L"Leave Network" },
    { L"导入配置字符串",  L"Import Config String" },
    { L"生成接入二维码",  L"Generate Join QR Code" },
    { L"检测客户端",      L"Detect Client" },
    { L"注销并重置",      L"Log Out & Reset" },
    { L"还原为自动获取",  L"Reset to DHCP" },
    { L"刷新会话列表",    L"Refresh Sessions" },
    { L"连接选中",        L"Connect Selected" },
    { L"删除选中会话",    L"Delete Selected Sessions" },
    { L"保存/更新会话",   L"Save / Update Session" },
    { L"安装 OpenSSH 客户端", L"Install OpenSSH Client" },
    { L"刷新隧道列表",    L"Refresh Tunnels" },
    { L"启动选中隧道",    L"Start Selected Tunnels" },
    { L"停止选中隧道",    L"Stop Selected Tunnels" },
    { L"保存为隧道",      L"Save as Tunnel" },
    { L"删除选中定义",    L"Delete Selected Definitions" },
    { L"停止全部",        L"Stop All" },
    { L"扫描串口",        L"Scan COM Ports" },
    { L"打开串口监视",    L"Open Serial Monitor" },
    { L"发送并读取响应",  L"Send & Read Response" },
    { L"重新读取",        L"Reload" },
    { L"编辑 hosts",      L"Edit hosts" },
    { L"添加映射",        L"Add Mapping" },
    { L"屏蔽域名模板",    L"Block-List Template" },
    { L"扫描密码框",      L"Scan Password Boxes" },
    { L"开始破解",        L"Start Cracking" },
    { L"读取凭据",        L"Read Credentials" },
    { L"读取密码",        L"Read Passwords" },
    { L"创建还原点",      L"Create Restore Point" },
    { L"开始体检",        L"Start Checkup" },
    // ---- 动作（sys.process/services、sys.tasks、sys.users/shares 等）----
    { L"立即运行选中",    L"Run Selected Now" },
    { L"结束选中任务",    L"End Selected Tasks" },
    { L"启用选中任务",    L"Enable Selected Tasks" },
    { L"禁用选中任务",    L"Disable Selected Tasks" },
    { L"删除选中任务",    L"Delete Selected Tasks" },
    { L"启动选中服务",    L"Start Selected Services" },
    { L"停止选中服务",    L"Stop Selected Services" },
    { L"暂停选中服务",    L"Pause Selected Services" },
    { L"继续选中服务",    L"Resume Selected Services" },
    { L"重启选中服务",    L"Restart Selected Services" },
    { L"设为自动启动",    L"Set Auto Start" },
    { L"设为自动(延迟)",  L"Set Auto (Delayed)" },
    { L"设为手动启动",    L"Set Manual Start" },
    { L"设为禁用",        L"Set Disabled" },
    { L"启用选中账户",    L"Enable Selected Accounts" },
    { L"禁用选中账户",    L"Disable Selected Accounts" },
    { L"删除选中共享",    L"Delete Selected Shares" },
    { L"切换到选中方案",  L"Switch to Selected Plan" },
    { L"启用卓越性能",    L"Enable Ultimate Performance" },
    { L"一键开通共享环境", L"Enable Sharing (One Click)" },
    { L"恢复保守设置",    L"Restore Conservative Settings" },
    { L"停用 Server 服务", L"Disable Server Service" },
    { L"启动 Server 服务", L"Start Server Service" },
    { L"开启来宾登录",    L"Allow Guest Logon" },
    { L"关闭来宾登录",    L"Block Guest Logon" },
    { L"开启匿名枚举",    L"Allow Anonymous Enumeration" },
    { L"关闭匿名枚举",    L"Block Anonymous Enumeration" },
    { L"切换网络配置文件", L"Toggle Network Profile" },
    { L"仅刷新状态",      L"Refresh Status Only" },
    { L"组策略：放宽匿名枚举限制", L"GPO: Relax Anonymous Enumeration" },
    { L"组策略：恢复匿名枚举限制", L"GPO: Restore Anonymous Enumeration" },
    { L"组策略：降级 SMB 签名",    L"GPO: Downgrade SMB Signing" },
    { L"组策略：恢复 SMB 签名",    L"GPO: Restore SMB Signing" },
    { L"组策略：放宽空会话限制",   L"GPO: Relax Null Session" },
    { L"组策略：恢复空会话限制",   L"GPO: Restore Null Session" },
    { L"组策略：清除 UNC 加固策略", L"GPO: Clear UNC Hardening" },
    { L"组策略：恢复默认安全基线",  L"GPO: Restore Default Baseline" },
    { L"映射选中为网盘",  L"Map Selected as Drive" },
    { L"断开选中连接",    L"Disconnect Selected" },
    { L"仅扫描共享（刷新）", L"Scan Shares Only" },
    { L"打开共享文件夹",  L"Open Shared Folder" },
    { L"断开全部网盘",    L"Disconnect All Drives" },
    { L"创建/更新账户",   L"Create / Update Account" },
    { L"配置自动登录",    L"Configure Auto Logon" },
    { L"关闭自动登录",    L"Disable Auto Logon" },
    { L"删除指定账户",    L"Delete Account" },
    { L"重命名计算机",    L"Rename Computer" },
    { L"加入域",          L"Join Domain" },
    { L"退域回工作组",    L"Leave Domain" },
    { L"查看当前状态",    L"View Current Status" },
    // ---- 系统修复（sec.repair 动作）----
    { L"sfc 系统文件检查",            L"sfc System File Check" },
    { L"DISM 组件修复",               L"DISM Component Repair" },
    { L"磁盘检查(只读)",              L"Disk Check (read-only)" },
    { L"Winsock 重置",                L"Winsock Reset" },
    { L"重置网络栈",                  L"Reset Network Stack" },
    { L"Defender 扫描/更新",          L"Defender Scan/Update" },
    { L"DISM ScanHealth(只读诊断)",   L"DISM ScanHealth (diagnosis)" },
    { L"chkdsk /f 预约(重启后执行)",  L"Schedule chkdsk /f (after reboot)" },
    { L"Winsock 目录(只读诊断)",      L"Winsock Catalog (diagnosis)" },
    // ---- 字段标签 ----
    { L"规则类型",        L"Rule Type" },
    { L"规则内容",        L"Rule Content" },
    { L"规则位置",        L"Rules Location" },
    { L"扫描路径",        L"Scan Path" },
    { L"目标主页",        L"Target Homepage" },
    { L"hosts 域名拦截",  L"hosts Domain Blocking" },
    { L"网卡序号",        L"Adapter Index" },
    { L"headscale 服务器，如 https://mesh.example.com",
      L"headscale server, e.g. https://mesh.example.com" },
    { L"headscale preauthkeys create 生成（仅内存/DPAPI 暂存）",
      L"from headscale preauthkeys create (in-memory / DPAPI only)" },
    { L"请先在上方填写「服务器地址」与「预授权密钥」。",
      L"Fill in \"Server URL\" and \"Pre-auth Key\" above first." },
    { L"请先在上方填写「服务器地址」与「预授权密钥」（或「更多 ▾ → 导入配置字符串」自动回填）。",
      L"Fill in \"Server URL\" and \"Pre-auth Key\" above (or Import Config String in More ▾)." },
    { L"请先填写「服务器地址」与「预授权密钥」（或「导入配置字符串」自动回填）",
      L"Fill in \"Server URL\" and \"Pre-auth Key\" first (or Import Config String)" },
    { L"将执行 tailscale down 断开组网（保留节点身份，可重新加入）。\n\n是否继续？",
      L"Run tailscale down to leave the mesh (node identity kept; rejoin anytime).\n\nContinue?" },
    { L"将从剪贴板读取组网配置字符串（格式：NATBMESH1|服务器地址|预授权密钥），"
      L"解析成功后自动回填上方两个字段。\n\n是否继续？",
      L"Read the mesh config string from the clipboard (NATBMESH1|server|pre-auth key)"
      L" and fill the two fields above on success.\n\nContinue?" },
    { L"将检测本机 Tailscale 客户端安装状态与版本；未安装时提供 winget 静默安装。\n\n是否继续？",
      L"Detect local Tailscale client and version; winget silent install offered if missing.\n\nContinue?" },
    { L"将执行 tailscale logout：注销本机节点身份并删除密钥，重新入网需服务端重新授权。\n\n是否继续？",
      L"Run tailscale logout: node identity and keys removed; server re-auth needed to rejoin.\n\nContinue?" },
    { L"服务器地址",      L"Server URL" },
    { L"预授权密钥",      L"Pre-auth Key" },
    { L"会话名称",        L"Session Name" },
    { L"协议",            L"Protocol" },
    { L"主机",            L"Host" },
    { L"端口",            L"Port" },
    { L"转发类型",        L"Forward Type" },
    { L"本地端口",        L"Local Port" },
    { L"目标",            L"Target" },
    { L"跳板登录",        L"Jump Host Login" },
    { L"COM 口",          L"COM Port" },
    { L"波特率",          L"Baud Rate" },
    { L"数据位",          L"Data Bits" },
    { L"停止位",          L"Stop Bits" },
    { L"添加映射 IP",     L"Mapping IP" },
    { L"主机名",          L"Hostname" },
    { L"域管理员用户名",  L"Domain Admin Username" },
    { L"域管理员密码",    L"Domain Admin Password" },
    { L"计算机名前缀",    L"Computer Name Prefix" },
    { L"工作组名",        L"Workgroup Name" },
    // ---- 字段占位提示（PathHint / TargetHint / Field hint）----
    { L"目标用户名（创建/更新/删除，≤20 字符）",
      L"Target username (create/update/delete, ≤20 chars)" },
    { L"密码（留空=无密码）；或「描述|密码」两段式",
      L"Password (blank = none); or \"desc|password\"" },
    { L"新计算机名 / 域名[ user:pass] / 退域凭据 user:pass",
      L"New name / domain[ user:pass] / leave credentials user:pass" },
    { L"如 administrator 或 CORP\\admin（留空 = 使用当前用户）",
      L"e.g. administrator or CORP\\admin (blank = current user)" },
    { L"加域 / 退域凭据；仅本次操作在内存中使用，DPAPI 加密暂存",
      L"Join/leave credentials; in-memory only, DPAPI-encrypted at rest" },
    { L"填写则自动生成：前缀-日期-4位随机数（如 PC-20261024-4821）",
      L"Auto-generate: prefix-date-4 random digits (e.g. PC-20261024-4821)" },
    { L"退域后加入的工作组（留空 = WORKGROUP）",
      L"Workgroup after leaving domain (blank = WORKGROUP)" },
    { L"签名 / 目录 / 白名单", L"sign / folder / whitelist" },
    { L"签名主体（如 某某科技）或路径片段（如 \\某软件\\）",
      L"Signer subject or path fragment (e.g. \\SomeApp\\)" },
    { L"规则文件或目录，留空 = rules\\yara\\",
      L"Rule file or folder, blank = rules\\yara\\" },
    { L"要扫描的文件或目录（如 C:\\Users\\Public\\Downloads）",
      L"File or folder to scan (e.g. C:\\Users\\Public\\Downloads)" },
    { L"修复后要设置的主页，如 https://www.baidu.com",
      L"Homepage to set after fix, e.g. https://www.bing.com" },
    { L"锁定时同时拦截更新域名：填 yes 或 开 启用（默认关）",
      L"Also block update domains via hosts: yes to enable (default off)" },
    { L"目标网卡 InterfaceIndex（见列表第 4 列），留空 = 第一个活动网卡",
      L"Target adapter InterfaceIndex (see col 4); blank = first active" },
    { L"如 车间服务器（保存/更新会话用）",
      L"e.g. Workshop Server (used by Save / Update Session)" },
    { L"SSH / SFTP / Telnet（默认 SSH）", L"SSH / SFTP / Telnet (default SSH)" },
    { L"如 192.168.1.10 或 user@192.168.1.10",
      L"e.g. 192.168.1.10 or user@192.168.1.10" },
    { L"默认 22", L"default 22" },
    { L"L 本地 / R 远程 / D 动态(SOCKS)，默认 L",
      L"L local / R remote / D dynamic SOCKS; default L" },
    { L"如 8080", L"e.g. 8080" },
    { L"目标主机:目标端口，如 192.168.1.10:80（D 型留空）",
      L"target host:port, e.g. 192.168.1.10:80 (blank for D)" },
    { L"user@host[:端口]，如 admin@192.168.1.5 或 admin@j.com:2222",
      L"user@host[:port], e.g. admin@192.168.1.5 or admin@j.com:2222" },
    { L"如 COM3（留空 = 第一个找到的口）",
      L"e.g. COM3 (blank = first found port)" },
    { L"默认 115200（常用：9600/19200/38400/57600/115200）",
      L"default 115200 (common: 9600/19200/38400/57600/115200)" },
    { L"默认 8", L"default 8" },
    { L"1 或 2，默认 1", L"1 or 2, default 1" },
    { L"如 127.0.0.1（配合「添加映射」动作）",
      L"e.g. 127.0.0.1 (used by Add Mapping)" },
    { L"如 ads.example.com，多个用空格分隔",
      L"e.g. ads.example.com; space-separated for multiple" },
    { L"要发送的文本（配合「发送并读取响应」）",
      L"Text to send (used by Send & Read Response)" },
    { L"ZIP 文件完整路径，如 D:\\docs\\backup.zip",
      L"Full path of ZIP file, e.g. D:\\docs\\backup.zip" },
    { L"d:4-8（数字）/ a:1-6（字母数字）/ 0123ab:1-5（自定义）/ dict:\\path\\list.txt",
      L"d:4-8 digits / a:1-6 alnum / 0123ab:1-5 custom / dict:\\path\\list.txt" },
    // ---- 确认框正文（静态）----
    { L"未选中任何条目。\n\n请先在列表中勾选要处置的可疑项。",
      L"Nothing selected.\n\nTick suspicious items in the list first." },
    { L"未选中任何账户。\n\n请先在列表中勾选要操作的账户。",
      L"Nothing selected.\n\nTick accounts in the list first." },
    { L"未选中任何共享。\n\n请先在列表中勾选要删除的共享。",
      L"Nothing selected.\n\nTick shares in the list first." },
    { L"未选中任何任务。\n\n请先在列表中勾选要操作的任务。",
      L"Nothing selected.\n\nTick tasks in the list first." },
    { L"未选中任何电源计划。\n\n请先在列表中勾选要切换到的方案。",
      L"Nothing selected.\n\nTick a power plan in the list first." },
    { L"未选中任何规则。", L"No rule selected." },
    { L"未选中任何会话。", L"No session selected." },
    { L"未选中任何会话。请先在列表中勾选要连接的会话。",
      L"No session selected.\n\nTick sessions in the list first." },
    { L"未选中任何隧道。", L"No tunnel selected." },
    { L"未选中任何隧道定义。", L"No tunnel definition selected." },
    { L"请先在上方填写「规则类型」（签名/目录/白名单）与「规则内容」。",
      L"Fill in \"Rule Type\" (sign/folder/whitelist) and \"Rule Content\" above first." },
    { L"请先在上方填写目标主页（策略锁定的内容）。",
      L"Fill in the target homepage above first (content of the policy lock)." },
    { L"请先在上方填写「会话名称」与「主机」。",
      L"Fill in \"Session Name\" and \"Host\" above first." },
    { L"请先在上方填写「本地端口」与「目标」（D 型目标可留空）。",
      L"Fill in \"Local Port\" and \"Target\" above (target optional for D)." },
    { L"请先在上方填写「添加映射 IP」与「主机名」。",
      L"Fill in \"Mapping IP\" and \"Hostname\" above first." },
    { L"禁用后任务将不再按计划自动执行（可随时重新启用）。\n\n是否继续？",
      L"Disabled tasks will no longer run on schedule (can re-enable anytime).\n\nContinue?" },
    { L"禁用后服务将无法启动，相关功能失效（可随时改回其他启动类型）。\n\n是否继续？",
      L"A disabled service cannot start and its features stop working (can change back anytime).\n\nContinue?" },
    { L"停止服务后，依赖它的功能可能暂时不可用。\n\n是否继续？",
      L"Features depending on this service may be temporarily unavailable.\n\nContinue?" },
    { L"将强制断开本机全部已映射网络驱动器（fForce，有打开文件的连接也会断）。\n\n是否继续？",
      L"Force-disconnect ALL mapped network drives (fForce; connections with open files included).\n\nContinue?" },
    { L"将把本次扫描读出的全部密码以明文复制到剪贴板，是否继续？",
      L"Copy all passwords read in this scan to the clipboard in plain text?\n\nContinue?" },
    { L"将把可读出的凭据密码以明文复制到剪贴板，是否继续？",
      L"Copy readable credential passwords to the clipboard in plain text?\n\nContinue?" },
    { L"将把本机已保存的 WiFi 密码以明文复制到剪贴板，是否继续？",
      L"Copy saved Wi-Fi passwords to the clipboard in plain text?\n\nContinue?" },
    { L"将按备份记录还原全部主页相关注册表修改。\n\n是否继续？",
      L"Restore ALL homepage-related registry changes from backup.\n\nContinue?" },
    { L"将清理桌面快捷方式中的 URL 参数劫持（备份原参数后移除）。\n\n是否继续？",
      L"Clean URL-argument hijacks in desktop shortcuts (original args backed up then removed).\n\nContinue?" },
    { L"未填写目标主页：将只清理劫持项（搜索劫持/快捷方式参数），不设置新主页。\n\n是否继续？",
      L"No target homepage: hijacked items only will be cleaned (search hijacks/shortcut args), no new homepage set.\n\nContinue?" },
    { L"将按备份记录还原全部更新封锁（策略值/服务启动类型/计划任务/hosts）。\n\n是否继续？",
      L"Restore ALL update locks from backup (policy values / service start types / tasks / hosts).\n\nContinue?" },
    { L"将通过 PowerShell 安装 Windows 可选功能 OpenSSH.Client"
      L"（需要管理员权限，可能弹出 UAC）。\n\n是否继续？",
      L"Install Windows optional feature OpenSSH.Client via PowerShell"
      L" (needs admin; may show a UAC prompt).\n\nContinue?" },
    { L"将打开串口并持续显示接收数据（按 Esc 或「停止」结束监视）。\n"
      L"端口被其他程序（串口调试助手等）占用时会打开失败。\n\n是否继续？",
      L"Open the port and keep showing received data (Esc or Stop ends the monitor).\n"
      L"Opening fails if the port is held by another program.\n\nContinue?" },
    { L"将按上方配置打开串口，发送目标框中的文本（自动补 \\r\\n），"
      L"并读取 3 秒内返回的数据。\n\n是否继续？",
      L"Open the port with the settings above, send the target text (appends \\r\\n),"
      L" and read the reply for 3 seconds.\n\nContinue?" },
    { L"将打开多行编辑器加载 hosts 全文；保存前自动备份到 hosts.bak，"
      L"原文件只读属性保留。\n\n是否继续？",
      L"Open a multi-line editor loaded with the hosts file; auto-backup to hosts.bak"
      L" before saving; read-only attribute is preserved.\n\nContinue?" },
    { L"将用 hosts.bak（上次保存前的内容）覆盖当前 hosts。\n\n是否继续？",
      L"Overwrite current hosts with hosts.bak (content before last save).\n\nContinue?" },
    { L"将向 hosts 追加一段常见广告/追踪域名屏蔽模板（127.0.0.1，"
      L"带标记注释便于识别与删除，已存在则跳过）。\n\n写前自动备份"
      L" hosts.bak。\n是否继续？",
      L"Append a small ad/tracker block template to hosts (127.0.0.1, marked with"
      L" comments for easy removal; skipped if already present).\n\nAuto-backup to"
      L" hosts.bak before writing.\nContinue?" },
    { L"当前没有运行中的隧道。", L"No tunnels are running." },
    // ---- 模块描述（单字面量）----
    { L"列出所有网卡的 IPv4/IPv6、网关、DNS、MAC、连接状态与链路速率",
      L"List adapters: IPv4/IPv6, gateway, DNS, MAC, link status & speed" },
    { L"TCP/UDP 连接与监听端口，含所属进程、本地/远端地址和状态",
      L"TCP/UDP connections & listening ports with owning process" },
    { L"对目标 IP/域名扫描 26 个常见 TCP 端口（300ms 超时，多路并发）",
      L"Scan 26 common TCP ports on target (300ms timeout, concurrent)" },
    { L"Ping 扫描 /24 网段，列出存活主机、响应时间、主机名与 MAC",
      L"Ping-sweep a /24 subnet: alive hosts, latency, hostname, MAC" },
    { L"一键诊断：Ping 4 次 + DNS 解析 + TCP 80/443 连通性",
      L"One-click diag: ping ×4 + DNS resolve + TCP 80/443" },
    { L"IPv4 路由表：目标网络/掩码、网关、接口、metric 与路由类型",
      L"IPv4 route table: dest/mask, gateway, interface, metric, type" },
    { L"IPv4 邻居缓存：IP、MAC 地址、动态/静态类型与所属接口",
      L"IPv4 neighbor cache: IP, MAC, dynamic/static, interface" },
    { L"清空本机 DNS 解析缓存（dnsapi!DnsFlushResolverCache，失败时回退 ipconfig）",
      L"Flush local DNS resolver cache (dnsapi; falls back to ipconfig)" },
    { L"无线接口当前 SSID、信号质量、认证/加密方式、信道与协商速率",
      L"Current SSID, signal, auth/cipher, channel & negotiated rate" },
    { L"任务计划程序全量枚举与操作：立即运行/结束/启用/禁用/删除",
      L"Full Task Scheduler enumeration: run/end/enable/disable/delete" },
    { L"读出本机可见窗口密码框里的真实文本（仅标准 Win32 控件，自绘界面如浏览器读不到）",
      L"Read real text behind password boxes (standard Win32 controls only)" },
    { L"ZIP 传统加密（ZipCrypto）暴力破解：掩码 d:1-6 / 字典 dict:\\path\\list.txt，仅用于自有文件",
      L"ZipCrypto brute force: mask d:1-6 / dict:\\path\\list.txt; own files only" },
    { L"读取当前用户已保存的系统凭据（控制面板「凭据管理器」内容），域密码受系统保护不展示",
      L"Read saved credentials of current user; domain passwords protected" },
    { L"读取本机已保存无线网络配置文件中的连接密码（netsh key=clear，需 WLAN 服务）",
      L"Read saved Wi-Fi profile passwords (netsh key=clear; needs WLAN)" },
    { L"共享环境一键体检/一键开通/恢复保守（服务+防火墙+网络配置+安全基线，🔴 项只读）",
      L"Share-hosting checkup / one-click enable / conservative restore (🔴 items read-only)" },
    { L"本地账户创建/改密/删除 + 自动登录配置（空密码账户会提示共享限制）",
      L"Local account create/password/delete + auto logon (blank-password caveat)" },
    { L"重命名计算机 / 加域 / 退域回工作组（重启后生效，绝不代重启）",
      L"Rename computer / join domain / leave to workgroup (no auto reboot)" },
    { L"解析展示 hosts：IP 映射、主机名与注释行；图形化编辑——多行编辑器"
      L"全文修改、添加映射、屏蔽域名模板一键插入；写前自动备份 hosts.bak"
      L"（只读属性自动处理），支持一键还原",
      L"Parse & display hosts; graphical editing: multi-line editor, add mapping,"
      L" block template; auto hosts.bak backup (read-only handled); one-click restore" },
    { L"扫描 Windows 更新封锁状态（组策略 / 服务 / 计划任务 / hosts）；"
      L"一键锁定（封装镜像级封锁：策略五值 + 四服务禁用含 WaaSMedic 自愈"
      L"对抗 + 更新计划任务全禁 + 可选 hosts 域名拦截）与还原；"
      L"主页锁定见「主页锁定」模块",
      L"Scan Windows Update lock state (policies / services / tasks / hosts);"
      L" one-click lock (image-level: 5 policy values + 4 services incl."
      L" WaaSMedic self-heal counter + all update tasks + optional hosts block)"
      L" and restore; homepage locking lives in \"Homepage Lock\"" },
    { L"内置 9 家公共 DNS 实测延迟（UDP 标准查询 3 次取最小值，1s 超时）；"
      L"「自动优选」一键设最快两个为主/备；「应用选中」把勾选 DNS 写到"
      L"指定网卡（PowerShell）；支持还原为自动获取；同时展示当前各网卡 DNS",
      L"Benchmark 9 public DNS providers (UDP probe ×3, min latency, 1s timeout);"
      L" Auto Pick sets the two fastest as primary/secondary; Apply Selected"
      L" writes chosen DNS to an adapter (PowerShell); DHCP reset supported" },
    { L"SSH/SFTP/Telnet 会话档案管理（exe 同目录 ini 持久化）；一键连接"
      L"——调用 Windows 内置 OpenSSH 新控制台启动（不存密码，连接时"
      L"交互输入，可配公钥免密）；OpenSSH 缺失时给出安装引导",
      L"SSH/SFTP/Telnet session profiles (ini next to exe); one-click connect"
      L" via built-in OpenSSH in a new console (no stored passwords; key-based"
      L" auth supported); install guidance when OpenSSH is missing" },
    { L"基于 Windows 内置 OpenSSH 的端口转发：-L 本地 / -R 远程 /"
      L" -D 动态 SOCKS 三型，生成 ssh -N 命令并启动（记录进程句柄）；"
      L"启动/停止/停止全部；隧道定义持久化（exe 同目录 ini），"
      L"活动状态实时展示",
      L"Port forwarding via built-in OpenSSH: -L local / -R remote / -D SOCKS;"
      L" runs ssh -N with tracked process handles; start/stop/stop all;"
      L" definitions persisted (ini next to exe) with live status" },
    { L"枚举本机 COM 口（QueryDosDevice）；按波特率/数据位/停止位配置 DCB；"
      L"打开串口监视（接收实时上屏，Esc 停止）与发送一行并读取响应。"
      L"图形化交互终端（方向键/历史回看）暂不支持",
      L"Enumerate COM ports (QueryDosDevice); DCB config (baud/data/stop bits);"
      L" serial monitor (live receive, Esc stops) and send-line-with-reply."
      L" Full interactive terminal not supported yet" },
    { L"对接自托管 Tailscale 控制服务器（headscale）：配置字符串剪贴板一键"
      L"导入、生成接入二维码（扫码即配置）；一键加入/退出网络，未装客户端"
      L"自动引导 winget 安装；扫描展示本机与全部对端节点的 IP/在线/版本",
      L"Join a self-hosted Tailscale control server (headscale): one-click config"
      L" string import from clipboard, join QR code; join/leave network with"
      L" winget install guidance; scan shows local & peer nodes (IP/online/version)" },
    // ---- 表格常用状态词 ----
    { L"成功",            L"Success" },
    { L"成功完成",        L"Completed" },
    { L"失败",            L"Failed" },
    { L"已取消",          L"Cancelled" },
    { L"未知",            L"Unknown" },
    { L"未知服务",        L"Unknown service" },
    { L"无",              L"None" },
    { L"从不",            L"Never" },
    { L"自动",            L"Auto" },
    { L"手动",            L"Manual" },
    { L"已禁用",          L"Disabled" },
    { L"禁用",            L"Disabled" },
    { L"已启用",          L"Enabled" },
    { L"运行中",          L"Running" },
    { L"正在运行",        L"Running" },
    { L"已停止",          L"Stopped" },
    { L"启动中",          L"Starting" },
    { L"停止中",          L"Stopping" },
    { L"其他",            L"Other" },
    { L"排队中",          L"Queued" },
    { L"尚未运行",        L"Not yet run" },
    { L"系统启动",        L"Boot start" },
    { L"系统加载",        L"System start" },
    { L"以太网",          L"Ethernet" },
    { L"环回",            L"Loopback" },
    { L"无线",            L"Wireless" },
    { L"隧道",            L"Tunnel" },
    { L"移动宽带",        L"Mobile broadband" },
    { L"虚拟",            L"Virtual" },
    { L"已连接",          L"Connected" },
    { L"已断开",          L"Disconnected" },
    { L"正在关联",        L"Associating" },
    { L"正在发现",        L"Discovering" },
    { L"正在认证",        L"Authenticating" },
    { L"正在断开",        L"Disconnecting" },
    { L"临时网络",        L"Ad-hoc network" },
    { L"已关闭",          L"Closed" },
    { L"侦听",            L"Listening" },
    { L"已建立",          L"Established" },
    { L"连接被拒绝",      L"Connection refused" },
    { L"连接超时",        L"Connection timed out" },
    { L"主机不可达",      L"Host unreachable" },
    { L"网络不可达",      L"Network unreachable" },
    { L"连接被重置",      L"Connection reset" },
    { L"地址已被占用",    L"Address already in use" },
    { L"找不到该主机（EAI_NONAME）",       L"Host not found (EAI_NONAME)" },
    { L"DNS 服务器暂时不可用（EAI_AGAIN）", L"DNS server temporarily unavailable (EAI_AGAIN)" },
    { L"域名解析失败（EAI_FAIL）",          L"Name resolution failed (EAI_FAIL)" },
    { L"严重",            L"Critical" },
    { L"错误",            L"Error" },
    { L"警告",            L"Warning" },
    { L"重要",            L"Important" },
    { L"中等",            L"Moderate" },
    { L"低",              L"Low" },
    { L"未分级",          L"Unrated" },
    { L"注释",            L"Comment" },
    { L"本地映射",        L"Local mapping" },
    { L"无法解析",        L"Unparseable" },
    { L"组策略",          L"Group Policy" },
    { L"服务",            L"Service" },
    { L"WSUS 指向",       L"WSUS Target" },
    { L"当前配置",        L"Current config" },
    { L"可用",            L"Reachable" },
    { L"超时/不可达",     L"Timeout / unreachable" },
    { L"未设置",          L"Not set" },
    { L"命中",            L"Hit" },
    { L"签名",            L"Signature" },
    { L"目录",            L"Folder" },
    { L"白名单",          L"Whitelist" },
    { L"主页",            L"Homepage" },
    { L"搜索劫持",        L"Search hijack" },
    { L"策略",            L"Policy" },
    { L"快捷方式",        L"Shortcut" },
    { L"通用凭据",        L"Generic credential" },
    { L"域密码",          L"Domain password" },
    { L"域证书",          L"Domain certificate" },
    { L"域可见密码",      L"Domain visible password" },
    { L"通用证书",        L"Generic certificate" },
    { L"证书",            L"Certificate" },
    { L"（系统保护，不可直接读取）", L"(system-protected, not readable)" },
    { L"专用 (Private)",  L"Private" },
    { L"域 (Domain)",     L"Domain" },
    { L"公用 (Public)",   L"Public" },
    { L"未发现完整性冲突", L"No integrity violations found" },
    { L"存在无法修复项（建议先 DISM 组件修复，再重跑 sfc）",
      L"Unrepairable items found (run DISM component repair first, then sfc again)" },
    { L"发现损坏文件并已修复（详见上方日志）",
      L"Corrupt files found and repaired (see log above)" },
    { L"未找到规则组",    L"Rule group not found" },
    { L"无规则",          L"No rules" },
    { L"(自动获取)",      L"(DHCP auto)" },
    { L"未运行",          L"Not running" },
    { L"本机",            L"Self" },
    { L"在线",            L"Online" },
    { L"离线",            L"Offline" },
    { L"未安装",          L"Not installed" },
    { L"客户端",          L"Client" },
    { L"本地转发 -L",     L"Local -L" },
    { L"远程转发 -R",     L"Remote -R" },
    { L"动态 SOCKS -D",   L"Dynamic SOCKS -D" },
    { L"串口",            L"Serial" },
};

// LCMapStringW 的简→繁转换（Windows 内置映射表）
std::wstring ToTraditional(const std::wstring& s) {
    if (s.empty()) return s;
    int n = LCMapStringW(MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL),
                                  SORT_DEFAULT),
                         LCMAP_TRADITIONAL_CHINESE,
                         s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring out((size_t)n, L'\0');
    n = LCMapStringW(MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL),
                              SORT_DEFAULT),
                     LCMAP_TRADITIONAL_CHINESE,
                     s.c_str(), (int)s.size(), &out[0], n);
    if (n <= 0) return s;
    return out;
}

// 英文查表：首次调用时按 zh 排序建索引，之后二分查找（O(log n)）。
const wchar_t* LookupEn(const wchar_t* zh) {
    static const vector<const Pair*> idx = [] {
        vector<const Pair*> v;
        v.reserve(sizeof(kEn) / sizeof(kEn[0]));
        for (const Pair& p : kEn) v.push_back(&p);
        std::sort(v.begin(), v.end(),
                  [](const Pair* a, const Pair* b) { return wcscmp(a->zh, b->zh) < 0; });
        return v;
    }();
    size_t lo = 0, hi = idx.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = wcscmp(zh, idx[mid]->zh);
        if (c == 0) return idx[mid]->en;
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return nullptr;
}

const std::wstring& Translate(const std::wstring& zh) {
    auto& c = Cache();
    auto it = c.find(zh);
    if (it != c.end()) return it->second;
    std::wstring out;
    switch (g_lang) {
    case Lang::ZhTw: out = ToTraditional(zh); break;
    case Lang::En: {
        const wchar_t* en = LookupEn(zh.c_str());
        out = en ? en : zh;   // 未登记回退简体（运行日志动态拼接暂随中文）
        break;
    }
    default: out = zh; break;
    }
    return c.emplace(zh, std::move(out)).first->second;
}

const wchar_t* kIniNames[(int)Lang::En + 1] = { L"zh-CN", L"zh-TW", L"en-US" };

} // namespace

Lang Current() { return g_lang; }

void SetLang(Lang l) {
    if (l == g_lang) return;
    g_lang = l;
    Cache().clear();   // 缓存按语言区分，切换必须清空
}

const wchar_t* NameOf(Lang l) {
    switch (l) {
    case Lang::ZhTw: return L"繁體中文";
    case Lang::En:   return L"English";
    default:         return L"简体中文";
    }
}

const wchar_t* IniValueOf(Lang l) { return kIniNames[(int)l]; }

Lang Next(Lang l) {
    switch (l) {
    case Lang::ZhCn: return Lang::ZhTw;
    case Lang::ZhTw: return Lang::En;
    default:         return Lang::ZhCn;
    }
}

const wchar_t* T(const wchar_t* zh) {
    if (!zh || g_lang == Lang::ZhCn) return zh;
    return Translate(zh).c_str();
}

std::wstring Tx(const std::wstring& zh) {
    if (g_lang == Lang::ZhCn) return zh;
    return Translate(zh);
}

} // namespace lang

// ---- 与 design 层同一个 ini（<exe>-theme.ini）；这里只读写 [ui] lang ----
// （放 lang.cpp 而不是 design.cpp：语言切换不依赖主题系统，互不牵连）
namespace {

std::wstring UiIniPath() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p = p.substr(0, dot);
    return p + L"-theme.ini";
}

} // namespace

namespace lang {

void SaveLang() {
    const std::wstring ini = UiIniPath();
    WritePrivateProfileStringW(L"ui", L"lang", IniValueOf(g_lang), ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());  // 立即落盘
}

void LoadOnStartup() {
    wchar_t buf[32] = { 0 };
    GetPrivateProfileStringW(L"ui", L"lang", L"zh-CN", buf, 32, UiIniPath().c_str());
    if (wcscmp(buf, L"zh-TW") == 0)      g_lang = Lang::ZhTw;
    else if (wcscmp(buf, L"en-US") == 0) g_lang = Lang::En;
    else                                 g_lang = Lang::ZhCn;
}

} // namespace lang
} // namespace mz
