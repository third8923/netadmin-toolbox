# t01_nav_test.ps1 — T01 侧栏两级可展开导航 验收（v2.9）
# 前置：脚本自行 taskkill 残留 + 删 dist/netadmin-toolbox-*.ini
# 关键约束：导航切换/组头点击必须真实鼠标点击（LB_SETCURSEL 对本 App 无效）；
#           MZ_ELEV_ATTEMPTED=1 跳过自动提权
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T01 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shotDir = Join-Path $dir "tools"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
function Shot([IntPtr]$h, [string]$name) {
  [T01]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object T01+RECT
  [T01]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object T01+POINT
  [T01]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [T01]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  [T01]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function RealClick([int]$x, [int]$y) {
  [T01]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T01]::mouse_event(2, $x, $y, 0, 0) | Out-Null   # LEFTDOWN
  Start-Sleep -Milliseconds 70
  [T01]::mouse_event(4, $x, $y, 0, 0) | Out-Null   # LEFTUP
  Start-Sleep -Milliseconds 350
}
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [T01]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)  # LB_GETITEMRECT
    if ($r -eq [IntPtr]::Zero) { return $false }
    $bytes = New-Object byte[] 16
    [System.Runtime.InteropServices.Marshal]::Copy($mem, $bytes, 0, 16)
    $out.Value.Left   = [BitConverter]::ToInt32($bytes, 0)
    $out.Value.Top    = [BitConverter]::ToInt32($bytes, 4)
    $out.Value.Right  = [BitConverter]::ToInt32($bytes, 8)
    $out.Value.Bottom = [BitConverter]::ToInt32($bytes, 12)
    return $true
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem) }
}
function NavTextAt([IntPtr]$nav, [int]$idx) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [T01]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()  # LB_GETTEXT
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavCount([IntPtr]$nav) { return [T01]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavCurSel([IntPtr]$nav) { return [T01]::SendMessageW($nav, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavTopIndex([IntPtr]$nav) { return [T01]::SendMessageW($nav, 0x018E, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavItemH([IntPtr]$nav, [int]$idx) { return [T01]::SendMessageW($nav, 0x01A1, [IntPtr]$idx, [IntPtr]0).ToInt32() }
# 滚动到可见 → 真实鼠标点击导航行
function NavGo([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  [T01]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null  # LB_SETTOPINDEX
  Start-Sleep -Milliseconds 250
  $r = New-Object T01+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { Write-Output "  [WARN] navrect fail idx=$idx"; return $false }
  $pt = New-Object T01+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [T01]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T01]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  RealClick $pt.X $pt.Y
  Start-Sleep -Milliseconds 500
  return $true
}
function FindNavPrefix([IntPtr]$nav, [string]$prefix) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) {
    $t = NavTextAt $nav $i
    if ($t.StartsWith($prefix)) { return $i }
  }
  return -1
}
function FindNav([IntPtr]$nav, [string]$text) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) {
    if ((NavTextAt $nav $i) -eq $text) { return $i }
  }
  return -1
}
function KeyDown([byte]$vk) { [T01]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
function KeyUp([byte]$vk)   { [T01]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function Tap([byte]$vk)     { KeyDown $vk; Start-Sleep -Milliseconds 60; KeyUp $vk }
function Chord([byte]$mod, [byte]$vk) {
  KeyDown $mod; Start-Sleep -Milliseconds 80; Tap $vk; Start-Sleep -Milliseconds 80; KeyUp $mod
}
function SetEditText([IntPtr]$ctrl, [string]$s) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($s)
  [T01]::SendMessageW($ctrl, 0x000C, [IntPtr]0, $ptr) | Out-Null   # WM_SETTEXT
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}
# 主窗口快捷键（Ctrl+T / Ctrl+= / Ctrl+0）：nav listbox 聚焦时不转发未处理按键，
# 直接 PostMessage WM_KEYDOWN 到主窗口；物理按住 Ctrl 保证 GetKeyState 可见
function SendHotkey([IntPtr]$h, [byte]$vk) {
  KeyDown 0x11; Start-Sleep -Milliseconds 80
  [T01]::PostMessageW($h, 0x0100, [IntPtr]$vk, [IntPtr]0) | Out-Null   # WM_KEYDOWN
  Start-Sleep -Milliseconds 80
  KeyUp 0x11; Start-Sleep -Milliseconds 400
}
function ReadIniNav() {
  $ini = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $ini)) { return "" }
  foreach ($ln in (Get-Content $ini -Encoding Default)) {   # WritePrivateProfileStringW 写 ANSI
    if ($ln -like "navExpanded=*") { return $ln.Substring(12) }
  }
  return ""
}
function Launch([string]$tag) {
  $p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
  Start-Sleep -Seconds 4
  $p.Refresh()
  $h = $p.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { Write-Output "[FAIL] no main window ($tag)"; exit 1 }
  return @($p, $h)
}
function CloseApp([object]$proc) {
  [T01]::PostMessageW($proc.MainWindowHandle, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null  # WM_CLOSE
  Start-Sleep -Seconds 2
  $proc.Refresh()
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Start-Sleep -Seconds 1 }
}

$env:MZ_ELEV_ATTEMPTED = "1"
$expected = @("磁盘清理","空间分析","网络管理","网络诊断","系统管理","系统信息","常用管理","软件管家","安全体检","高级工具","系统优化","系统维护")

# ================= 0) 清场 =================
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== T01 侧栏两级导航验收 =="

# ================= 1) 启动：固定分组顺序 + 默认全展开 =================
$la = Launch "start"; $p = $la[0]; $hwnd = $la[1]
$nav = [T01]::GetDlgItem($hwnd, 1001)
$cnt = NavCount $nav
Write-Output ("1) initial nav rows: " + $cnt)
Assert ($cnt -eq 67) ("初始行数 67（12 组头 + 55 模块）实际 " + $cnt)
$orderOk = $true
$prev = -1
foreach ($g in $expected) {
  $gi = FindNavPrefix $nav $g
  Write-Output ("    group " + $g + " @ row " + $gi)
  if ($gi -le $prev) { $orderOk = $false }
  $prev = $gi
}
Assert $orderOk "12 组按固定顺序表展示（磁盘清理→空间分析→网络管理→网络诊断→系统管理→系统信息→常用管理→软件管家→安全体检→高级工具→系统优化→系统维护）"
$hg = NavItemH $nav 0; $hm = NavItemH $nav 1
Write-Output ("  row heights: group=" + $hg + " module=" + $hm)
Shot $hwnd "t01-1-home.png"

# ================= 2) 真实点击组头：收起 / 展开 + chevron =================
$i0 = FindNavPrefix $nav "磁盘清理"
[void](NavGo $hwnd $nav $i0)
Start-Sleep -Milliseconds 600
$c2 = NavCount $nav
Assert ($c2 -eq 62) ("点击「磁盘清理」组头收起：67→62 实际 " + $c2)
Shot $hwnd "t01-2-collapse.png"
[void](NavGo $hwnd $nav (FindNavPrefix $nav "磁盘清理"))
Start-Sleep -Milliseconds 600
$c3 = NavCount $nav
Assert ($c3 -eq 67) ("再次点击组头展开：62→67 实际 " + $c3)
Shot $hwnd "t01-3-expand.png"

# 组头切换（带状态验证与重试）：$wantCollapsed=$true 收起 / $false 展开
# 修复 67 行基线下的点击竞态：点击后核对组状态，未达目标则重试（最多 4 次）
function ToggleGroup([IntPtr]$h, [IntPtr]$nav, [string]$g, [bool]$wantCollapsed) {
  for ($a = 0; $a -lt 4; $a++) {
    $gi = FindNavPrefix $nav $g
    if ($gi -lt 0) { Start-Sleep -Milliseconds 300; continue }
    $next = NavTextAt $nav ($gi + 1)
    $isExpanded = ($next -ne "") -and ($next -notmatch " · \d+$")
    if ($wantCollapsed -and -not $isExpanded) { return $true }
    if ((-not $wantCollapsed) -and $isExpanded) { return $true }
    [void](NavGo $h $nav $gi)
    Start-Sleep -Milliseconds 500
  }
  return $false
}

# ================= 3) 收起全部 12 组 =================
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $true) }
Start-Sleep -Milliseconds 600
$c4 = NavCount $nav
Assert ($c4 -eq 12) ("收起全部 12 组：行数=12 实际 " + $c4)
Shot $hwnd "t01-4-all-collapsed.png"
$iniVal = ReadIniNav
Assert ($iniVal -eq "-") ("ini 持久化：全收起写占位符 '-' 实际 navExpanded=" + $iniVal)

# ================= 4) 重启 → 展开态保持 =================
CloseApp $p
$la = Launch "restart"; $p = $la[0]; $hwnd = $la[1]
$nav = [T01]::GetDlgItem($hwnd, 1001)
$c5 = NavCount $nav
$firstOk = (NavTextAt $nav 0).StartsWith("磁盘清理")
Assert (($c5 -eq 12) -and $firstOk) ("重启后展开态保持：行数=12 且首组=磁盘清理 实际 " + $c5 + " / " + (NavTextAt $nav 0))
Shot $hwnd "t01-5-persist-restart.png"

# ================= 5) 逐组展开恢复 + ini 回写 =================
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $false) }
Start-Sleep -Milliseconds 600
$c6 = NavCount $nav
Assert ($c6 -eq 67) ("逐组点击展开恢复：行数=67 实际 " + $c6)
$iniVal2 = ReadIniNav
$grpCnt = 0
if ($iniVal2 -and $iniVal2 -ne "-") { $grpCnt = ($iniVal2 -split ";").Count }
Assert ($grpCnt -eq 12) ("ini 持久化：全展开回写 12 个组名 实际 " + $grpCnt + " [" + $iniVal2 + "]")

# ================= 6) 搜索强制展开 / 清空恢复 =================
# 先全部收起（12 行），再搜索：若强制展开生效，可见行 = 匹配组头 + 匹配模块 ≥ 5
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $true) }
Start-Sleep -Milliseconds 500
$cBase = NavCount $nav
$search = [T01]::GetDlgItem($hwnd, 1022)
SetEditText $search "清理"
Start-Sleep -Milliseconds 900
$c7 = NavCount $nav
Assert (($cBase -eq 12) -and ($c7 -ge 5)) ("收起态下搜索「清理」强制展开：12 → " + $c7 + "（组头+匹配模块）")
Shot $hwnd "t01-6-search.png"
SetEditText $search ""
Start-Sleep -Milliseconds 900
$c8 = NavCount $nav
Assert ($c8 -eq 12) ("清空搜索恢复记忆折叠态：行数=12 实际 " + $c8)
# 恢复全展开供后续测试
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $false) }
Start-Sleep -Milliseconds 500

# ================= 7) 逐组真实点击模块行，≥15 个模块切换正确 =================
$mods = @("基础清理","微信清理","大文件查找","重复文件","网络适配器","端口扫描","局域网扫描","服务管理","进程管理","系统概览","系统还原点","电源计划","装机必备 · 推荐总榜","软件搜索","一键体检","软件搬家","Windows 优化","hosts 文件")
$okMods = 0
foreach ($m in $mods) {
  $mi = FindNav $nav $m
  if ($mi -lt 0) { Write-Output ("  [FAIL] nav 未找到模块: " + $m); $script:fail++; continue }
  [void](NavGo $hwnd $nav $mi)
  Start-Sleep -Milliseconds 400
  $cur = NavCurSel $nav
  if ($cur -eq $mi) { $okMods++; Write-Output ("  [PASS] 切换 " + $m + " (row " + $mi + ")") }
  else { $script:fail++; Write-Output ("  [FAIL] 切换 " + $m + " curSel=" + $cur + " expect " + $mi) }
}
Assert ($okMods -ge 15) ("模块切换正确数 ≥15（覆盖全部 12 组）实际 " + $okMods + "/" + $mods.Count)
Shot $hwnd "t01-7-mod-current.png"

# ================= 8) Ctrl+= 缩放一档 → 行高同步；Ctrl+0 复位 =================
[T01]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
$hBefore = NavItemH $nav 1
SendHotkey $hwnd 0xBB   # Ctrl+=
Start-Sleep -Milliseconds 900
$hAfter = NavItemH $nav 1
Assert ($hAfter -gt $hBefore) ("Ctrl+= 缩放：模块行高 " + $hBefore + "→" + $hAfter)
Shot $hwnd "t01-8-zoom.png"
SendHotkey $hwnd 0x30   # Ctrl+0
Start-Sleep -Milliseconds 700
$hReset = NavItemH $nav 1
Assert ($hReset -eq $hBefore) ("Ctrl+0 复位：行高回到 " + $hReset)

# ================= 9) 四主题侧栏走查 =================
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 600   # 暖阳米
Shot $hwnd "t01-9-theme-warmpaper.png"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 600   # 薄雾蓝
Shot $hwnd "t01-10-theme-mistblue.png"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 800   # 深空夜
Shot $hwnd "t01-11-theme-deepspace.png"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 800   # 晴空白
Shot $hwnd "t01-12-theme-daylight.png"
Write-Output "9) 四主题截图完成（组头/选中/悬停配色人工走查）"

# ================= 10) 键盘 + 滚轮 =================
[T01]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
# 先点一个模块行让 nav 拿到焦点
$mi = FindNav $nav "系统概览"
[void](NavGo $hwnd $nav $mi)
# 方向键移动高亮
$curA = NavCurSel $nav
Tap 0x28   # VK_DOWN
Start-Sleep -Milliseconds 300
$curB = NavCurSel $nav
Assert ($curB -gt $curA) ("键盘 ↓ 移动高亮：" + $curA + "→" + $curB)
# 上移到最近的组头行，回车收起
$guard = 0
while ($guard -lt 30) {
  $t = NavTextAt $nav (NavCurSel $nav)
  if ($t -match " · \d+$") { break }
  Tap 0x26; Start-Sleep -Milliseconds 200   # VK_UP
  $guard++
}
$tHead = NavTextAt $nav (NavCurSel $nav)
$cK1 = NavCount $nav
Tap 0x0D   # VK_RETURN
Start-Sleep -Milliseconds 600
$cK2 = NavCount $nav
Assert ($cK2 -lt $cK1) ("键盘回车切换组头收起：" + $tHead + " 行数 " + $cK1 + "→" + $cK2)
Tap 0x0D
Start-Sleep -Milliseconds 600
$cK3 = NavCount $nav
Assert ($cK3 -eq $cK1) ("键盘回车再次展开：行数恢复 " + $cK3)
# 滚轮滚动
$t0 = NavTopIndex $nav
$pt = New-Object T01+RECT
[void](NavRect $nav ([Math]::Min(5, (NavCount $nav) - 1)) ([ref]$pt))
$sp = New-Object T01+POINT
$sp.X = [int](($pt.Left + $pt.Right) / 2); $sp.Y = [int](($pt.Top + $pt.Bottom) / 2)
[T01]::ClientToScreen($nav, [ref]$sp) | Out-Null
[T01]::SetCursorPos($sp.X, $sp.Y) | Out-Null
Start-Sleep -Milliseconds 200
[T01]::mouse_event(0x0800, $sp.X, $sp.Y, [uint32]::MaxValue - 2, 0) | Out-Null   # WHEEL down
Start-Sleep -Milliseconds 400
$t1 = NavTopIndex $nav
Assert ($t1 -gt $t0) ("滚轮滚动：topIndex " + $t0 + "→" + $t1)

# ================= 11) 正常退出 =================
CloseApp $p
Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
