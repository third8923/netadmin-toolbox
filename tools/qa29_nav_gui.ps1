# qa29_nav_gui.ps1 — v2.9 独立验收 GUI 走查（QA Edward/Yan，全新会话）
# 覆盖：侧栏专项 + 4 新模块页 + danger 确认框（只弹出+取消，严禁真执行写操作）
#       + 稳定性 30+ 次切换 + WM_CLOSE 正常退出
# 约束：导航切换必须真实鼠标点击；MZ_ELEV_ATTEMPTED=1 跳过 UAC
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QA29 {
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
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
function Shot([IntPtr]$h, [string]$name) {
  [QA29]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QA29+RECT
  [QA29]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object QA29+POINT
  [QA29]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [QA29]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  [QA29]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function ShotRect([QA29+RECT]$r, [string]$name) {
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [WARN] $name empty rect"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  Write-Output ("  saved " + $name + " (${w}x${ht})")
}
function RealClick([int]$x, [int]$y) {
  [QA29]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [QA29]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [QA29]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 350
}
function NavTextAt([IntPtr]$nav, [int]$idx) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [QA29]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavCount([IntPtr]$nav) { return [QA29]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavCurSel([IntPtr]$nav) { return [QA29]::SendMessageW($nav, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [QA29]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
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
function NavGo([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  [QA29]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  $r = New-Object QA29+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { return $false }
  $pt = New-Object QA29+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [QA29]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [QA29]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  RealClick $pt.X $pt.Y
  Start-Sleep -Milliseconds 500
  return $true
}
function FindNav([IntPtr]$nav, [string]$text) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) { if ((NavTextAt $nav $i) -eq $text) { return $i } }
  return -1
}
function FindNavPrefix([IntPtr]$nav, [string]$prefix) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) { if ((NavTextAt $nav $i).StartsWith($prefix)) { return $i } }
  return -1
}
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
function ClickId([IntPtr]$h, [int]$id, [int]$wait = 900) {
  $b = [QA29]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [QA29]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}
function SetEditText([IntPtr]$ctrl, [string]$s) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($s)
  [QA29]::SendMessageW($ctrl, 0x000C, [IntPtr]0, $ptr) | Out-Null
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}
function KeyDown([byte]$vk) { [QA29]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
function KeyUp([byte]$vk)   { [QA29]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function Tap([byte]$vk)     { KeyDown $vk; Start-Sleep -Milliseconds 60; KeyUp $vk }
function SendHotkey([IntPtr]$h, [byte]$vk) {
  KeyDown 0x11; Start-Sleep -Milliseconds 80
  [QA29]::PostMessageW($h, 0x0100, [IntPtr]$vk, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 80
  KeyUp 0x11; Start-Sleep -Milliseconds 400
}
function ReadIniNav() {
  $ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $ini)) { return "" }
  foreach ($ln in (Get-Content $ini -Encoding Default)) {
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
  [QA29]::PostMessageW($proc.MainWindowHandle, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
  Start-Sleep -Seconds 2
  $proc.Refresh()
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Start-Sleep -Seconds 1 }
}
function EscModal() {
  $m = [QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($m -ne [IntPtr]::Zero) {
    [QA29]::SetForegroundWindow($m) | Out-Null
    Start-Sleep -Milliseconds 300
    Tap 0x1B
    Start-Sleep -Milliseconds 800
  }
  return ([QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero)
}

$env:MZ_ELEV_ATTEMPTED = "1"
$expected = @("磁盘清理","空间分析","网络管理","网络诊断","系统管理","系统信息","常用管理","软件管家","安全体检","高级工具","系统优化","系统维护")

# ================= 0) 清场 =================
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== QA29 GUI 独立验收（侧栏专项 + 4 新模块 + danger 弹出/取消 + 稳定性）=="

# ================= 1) 启动：基线 67 行 + 固定组序 =================
$la = Launch "start"; $p = $la[0]; $hwnd = $la[1]
$nav = [QA29]::GetDlgItem($hwnd, 1001)
$cnt = NavCount $nav
Write-Output ("1) initial nav rows: " + $cnt)
Assert ($cnt -eq 67) ("初始行数 67（12 组头 + 55 模块）实际 " + $cnt)
$orderOk = $true; $prev = -1
foreach ($g in $expected) {
  $gi = FindNavPrefix $nav $g
  if ($gi -le $prev) { $orderOk = $false }
  $prev = $gi
}
Assert $orderOk "12 组按固定顺序（磁盘清理→空间分析→网络管理→网络诊断→系统管理→系统信息→常用管理→软件管家→安全体检→高级工具→系统优化→系统维护）"
Shot $hwnd "qa29-1-home.png"
Write-Output "   （页脚 v2.9 / chevron 方向 / 组头 17px 半粗 人工走查截图）"

# ================= 2) 组头点击收起/展开 + chevron =================
$i0 = FindNavPrefix $nav "磁盘清理"
[void](NavGo $hwnd $nav $i0)
Start-Sleep -Milliseconds 600
$c2 = NavCount $nav
Assert ($c2 -eq 62) ("点击「磁盘清理」组头收起：67→62 实际 " + $c2)
Shot $hwnd "qa29-2-collapsed-disk.png"
[void](NavGo $hwnd $nav (FindNavPrefix $nav "磁盘清理"))
Start-Sleep -Milliseconds 600
$c3 = NavCount $nav
Assert ($c3 -eq 67) ("再次点击组头展开：62→67 实际 " + $c3)
Shot $hwnd "qa29-3-expanded-disk.png"
Write-Output "   （chevron 方向 0xE70D/0xE76C 对照 qa29-2/qa29-3 人工走查）"

# ================= 3) 全收起 → 12 行 + ini '-' =================
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $true) }
Start-Sleep -Milliseconds 600
$c4 = NavCount $nav
Assert ($c4 -eq 12) ("收起全部 12 组：行数=12 实际 " + $c4)
Shot $hwnd "qa29-4-all-collapsed.png"
$iniVal = ReadIniNav
Assert ($iniVal -eq "-") ("ini navExpanded 全收起占位符 '-' 实际 [" + $iniVal + "]")

# ================= 4) 重启保持展开态 =================
CloseApp $p
$la = Launch "restart"; $p = $la[0]; $hwnd = $la[1]
$nav = [QA29]::GetDlgItem($hwnd, 1001)
$c5 = NavCount $nav
$firstOk = (NavTextAt $nav 0).StartsWith("磁盘清理")
Assert (($c5 -eq 12) -and $firstOk) ("重启后保持收起态：行数=12 且首组=磁盘清理 实际 " + $c5 + " / " + (NavTextAt $nav 0))
Shot $hwnd "qa29-5-persist-restart.png"
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $false) }
Start-Sleep -Milliseconds 600
Assert ((NavCount $nav) -eq 67) ("逐组展开恢复 67 行，实际 " + (NavCount $nav))

# ================= 5) 搜索强制展开 / 清空恢复 =================
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $true) }
Start-Sleep -Milliseconds 500
$cBase = NavCount $nav
$search = [QA29]::GetDlgItem($hwnd, 1022)
SetEditText $search "清理"
Start-Sleep -Milliseconds 900
$c7 = NavCount $nav
Assert (($cBase -eq 12) -and ($c7 -ge 5)) ("收起态搜索「清理」强制展开：" + $cBase + " → " + $c7)
Shot $hwnd "qa29-6-search-expand.png"
SetEditText $search ""
Start-Sleep -Milliseconds 900
$c8 = NavCount $nav
Assert ($c8 -eq 12) ("清空搜索恢复记忆折叠态：行数=12 实际 " + $c8)
foreach ($g in $expected) { [void](ToggleGroup $hwnd $nav $g $false) }
Start-Sleep -Milliseconds 500

# ================= 6) Ctrl+= 缩放 / Ctrl+0 复位 =================
[QA29]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
$hB = [QA29]::SendMessageW($nav, 0x01A1, [IntPtr]1, [IntPtr]0).ToInt32()
SendHotkey $hwnd 0xBB
Start-Sleep -Milliseconds 900
$hA = [QA29]::SendMessageW($nav, 0x01A1, [IntPtr]1, [IntPtr]0).ToInt32()
Assert ($hA -gt $hB) ("Ctrl+= 缩放：模块行高 " + $hB + "→" + $hA)
Shot $hwnd "qa29-7-zoom.png"
SendHotkey $hwnd 0x30
Start-Sleep -Milliseconds 700
$hR = [QA29]::SendMessageW($nav, 0x01A1, [IntPtr]1, [IntPtr]0).ToInt32()
Assert ($hR -eq $hB) ("Ctrl+0 复位：行高回到 " + $hR)

# ================= 7) 四主题侧栏配色 =================
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 600
Shot $hwnd "qa29-8-theme-warmpaper.png"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 600
Shot $hwnd "qa29-9-theme-mistblue.png"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-10-theme-deepspace.png"
Write-Output "   （重点深空夜侧栏配色 qa29-10 人工走查）"
SendHotkey $hwnd 0x54; Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-11-theme-daylight.png"

# ================= 8) 4 个新模块页 + 更多菜单 + danger 弹出/取消 =================
# --- 8a sys.acct ---
$ia = FindNav $nav "账户部署"
[void](NavGo $hwnd $nav $ia)
Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-12-acct-page.png"
SetEditText ([QA29]::GetDlgItem($hwnd, 1021)) "mzqa01"
SetEditText ([QA29]::GetDlgItem($hwnd, 1020)) ""
ClickId $hwnd 1012 1200     # 动作「配置自动登录」
$modal = [QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "qa29-13-acct-danger.png"
  $ok = EscModal
  Assert $ok "sys.acct 配置自动登录 danger 弹出+Esc 取消（未写注册表）"
} else { Assert $false "sys.acct 配置自动登录 danger 确认框未弹出" }
ClickId $hwnd 1019 1200
$menu = [QA29]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object QA29+RECT
  [QA29]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "qa29-14-acct-more.png"
  Tap 0x1B; Start-Sleep -Milliseconds 700
} else { Assert $false "sys.acct 更多菜单未弹出" }

# --- 8b sys.domain ---
$idm = FindNav $nav "计算机名与域"
[void](NavGo $hwnd $nav $idm)
Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-15-domain-page.png"
SetEditText ([QA29]::GetDlgItem($hwnd, 1021)) "bad_name!"
ClickId $hwnd 1011 1200     # 动作「重命名计算机」
$modal = [QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "qa29-16-domain-danger.png"
  $ok = EscModal
  Assert $ok "sys.domain 重命名 danger 弹出+Esc 取消（未改名）"
} else { Assert $false "sys.domain 重命名 danger 确认框未弹出" }
ClickId $hwnd 1019 1200
$menu = [QA29]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object QA29+RECT
  [QA29]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "qa29-17-domain-more.png"
  Tap 0x1B; Start-Sleep -Milliseconds 700
} else { Assert $false "sys.domain 更多菜单未弹出" }

# --- 8c net.sharehost ---
$ish = FindNav $nav "共享部署"
[void](NavGo $hwnd $nav $ish)
Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-18-sharehost-page.png"
ClickId $hwnd 1011 1200     # 动作「一键开通共享环境」
$modal = [QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "qa29-19-sharehost-danger.png"
  $ok = EscModal
  Assert $ok "net.sharehost 一键开通 danger 弹出+Esc 取消（未改任何设置）"
} else { Assert $false "net.sharehost 一键开通 danger 确认框未弹出" }
ClickId $hwnd 1019 1200
$menu = [QA29]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object QA29+RECT
  [QA29]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "qa29-20-sharehost-more.png"
  Tap 0x1B; Start-Sleep -Milliseconds 700
} else { Assert $false "net.sharehost 更多菜单未弹出" }

# --- 8d net.sharemap ---
$ism = FindNav $nav "共享扫描与网盘映射"
[void](NavGo $hwnd $nav $ism)
Start-Sleep -Milliseconds 800
Shot $hwnd "qa29-21-sharemap-page.png"
ClickId $hwnd 1019 1200     # 更多 ▾
$menu = [QA29]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object QA29+RECT
  [QA29]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "qa29-22-sharemap-more.png"
  $mw = $mr.Right - $mr.Left; $mh = $mr.Bottom - $mr.Top
  $itemH = [Math]::Max(1, [int]($mh / 3))
  $mx = $mr.Left + [int]($mw / 2)
  $my = $mr.Top + [int]($itemH * 2.5)   # 第 3 项 = 断开全部网盘（danger 红）
  RealClick $mx $my
  Start-Sleep -Milliseconds 1200
  $modal = [QA29]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "qa29-23-sharemap-danger.png"
    $ok = EscModal
    Assert $ok "net.sharemap 断开全部 danger 弹出+Esc 取消（未断任何映射）"
  } else { Assert $false "net.sharemap 断开全部 danger 确认框未弹出" }
  EscModal | Out-Null
} else { Assert $false "net.sharemap 更多菜单未弹出" }

# ================= 9) 稳定性：连续切换 12 组 ≥30 次 =================
$p.Refresh()
$switches = 0
for ($round = 0; $round -lt 3; $round++) {
  foreach ($g in $expected) {
    # 收起
    [void](ToggleGroup $hwnd $nav $g $true)
    $switches++
    # 展开
    [void](ToggleGroup $hwnd $nav $g $false)
    $switches++
    $p.Refresh()
    if ($p.HasExited) { break }
  }
  if ($p.HasExited) { break }
}
$p.Refresh()
Assert ((-not $p.HasExited) -and $switches -ge 30) ("连续切换 " + $switches + " 次无崩溃（3 轮 × 12 组 × 收/展）")
# 逐模块切换抽查（含 4 新模块）
$mods = @("账户部署","计算机名与域","共享部署","共享扫描与网盘映射","系统概览","基础清理","网络适配器","装机必备 · 推荐总榜","一键体检","进程管理")
$okN = 0
foreach ($m in $mods) {
  $mi = FindNav $nav $m
  if ($mi -lt 0) { continue }
  [void](NavGo $hwnd $nav $mi)
  Start-Sleep -Milliseconds 350
  if ((NavCurSel $nav) -eq $mi) { $okN++ }
}
Assert ($okN -eq $mods.Count) ("10 个模块（含 4 新模块）真实点击切换 cur_ 映射全部正确 实际 " + $okN)
$p.Refresh()
Assert (-not $p.HasExited) "切换后进程仍存活"

# ================= 10) WM_CLOSE 正常退出 =================
[QA29]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
Assert ($p.HasExited) "WM_CLOSE 正常退出"

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
