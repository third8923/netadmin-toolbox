# qa28_gui.ps1 — NetAdmin Toolbox v2.8 发版 GUI 走查（QA 严过关）
# 前置（由调用方完成）：taskkill 残留进程；删除 dist/netadmin-toolbox-*.ini
# 关键约束：导航切换必须真实鼠标点击（LB_SETCURSEL 对本 App 无效，已验证结论）；
#           启动前设 MZ_ELEV_ATTEMPTED=1 以跳过自动提权（等价 UAC 取消降级路径，非提权走查）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QA28 {
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

$env:MZ_ELEV_ATTEMPTED = "1"   # 跳过自动提权（走标准用户路径，UAC 降级等价验证）

function Shot([IntPtr]$h, [string]$name) {
  [QA28]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QA28+RECT
  [QA28]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object QA28+POINT
  [QA28]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [QA28]::PrintWindow($h, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  [QA28]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name (${w}x${ht})"
}

function ShotRect([QA28+RECT]$r, [string]$name) {
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [WARN] $name empty rect"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

function ClickId([IntPtr]$h, [int]$id, [int]$wait = 900) {
  $b = [QA28]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [QA28]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}

function KeyDown([byte]$vk) { [QA28]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
function KeyUp([byte]$vk)   { [QA28]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function Tap([byte]$vk)     { KeyDown $vk; Start-Sleep -Milliseconds 60; KeyUp $vk }
function Chord([byte]$mod, [byte]$vk) {
  KeyDown $mod; Start-Sleep -Milliseconds 80; Tap $vk; Start-Sleep -Milliseconds 80; KeyUp $mod
}

function RealClick([int]$x, [int]$y) {
  [QA28]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [QA28]::mouse_event(2, $x, $y, 0, 0) | Out-Null   # LEFTDOWN
  Start-Sleep -Milliseconds 70
  [QA28]::mouse_event(4, $x, $y, 0, 0) | Out-Null   # LEFTUP
  Start-Sleep -Milliseconds 150
}

function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [QA28]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)  # LB_GETITEMRECT
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
    $len = [QA28]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()  # LB_GETTEXT
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}

# 导航切换：滚动到可见（LB_SETTOPINDEX 只滚动不改选中）→ 真实鼠标点击
function NavGo([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  [QA28]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null  # LB_SETTOPINDEX
  Start-Sleep -Milliseconds 250
  $r = New-Object QA28+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { Write-Output "  [WARN] navrect fail idx=$idx"; return $false }
  $pt = New-Object QA28+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [QA28]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [QA28]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  RealClick $pt.X $pt.Y
  Start-Sleep -Milliseconds 650
  return $true
}

function FindNav([IntPtr]$nav, [string]$text) {
  $cnt = [QA28]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()  # LB_GETCOUNT
  for ($i = 0; $i -lt $cnt; $i++) {
    $t = NavTextAt $nav $i
    if ($t -eq $text) { return $i }
  }
  return -1
}

function ListRows([IntPtr]$h) {
  $list = [QA28]::GetDlgItem($h, 1002)
  if ($list -eq [IntPtr]::Zero) { return -1 }
  return [QA28]::SendMessageW($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
}

function WaitRows([IntPtr]$h, [int]$maxMs) {
  $elapsed = 0
  while ($elapsed -lt $maxMs) {
    $n = ListRows $h
    if ($n -gt 0) { return $n }
    Start-Sleep -Milliseconds 400; $elapsed += 400
  }
  return (ListRows $h)
}

# ============ 开始 ============
Write-Output "== launch =="
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "[FAIL] no main window"; exit 1 }
if ($p.HasExited) { Write-Output "[FAIL] exited early"; exit 1 }
$nav = [QA28]::GetDlgItem($hwnd, 1001)

# 1) 主页
Shot $hwnd "qa28-1-home.png"
Write-Output "  home nav items: $([QA28]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32())"

# 2) 进程管理：真实点击导航 + 扫描
$i = FindNav $nav "进程管理"
if ($i -lt 0 -or -not (NavGo $hwnd $nav $i)) { Write-Output "[FAIL] nav 进程管理" } else {
  Write-Output "  switched to 进程管理"
  ClickId $hwnd 1010 800
  $rows = WaitRows $hwnd 20000
  Write-Output "  process rows: $rows"
  Shot $hwnd "qa28-2-process.png"
}

# 3) 一键体检：等 8 项出齐
$i = FindNav $nav "一键体检"
if ($i -lt 0 -or -not (NavGo $hwnd $nav $i)) { Write-Output "[FAIL] nav 一键体检" } else {
  ClickId $hwnd 1010 1200   # 开始体检
  $rows = WaitRows $hwnd 60000
  Start-Sleep -Milliseconds 1500
  Write-Output "  sec.scan rows: $rows"
  Shot $hwnd "qa28-3-secscan.png"
}

# 4) 系统修复页 + 9 动作按钮渲染
$i = FindNav $nav "系统修复"
if ($i -lt 0 -or -not (NavGo $hwnd $nav $i)) { Write-Output "[FAIL] nav 系统修复" } else {
  Start-Sleep -Milliseconds 500
  Shot $hwnd "qa28-4-repair-page.png"
  # 动作 0（sfc）确认框弹出与取消路径（不真跑）
  ClickId $hwnd 1010 1500
  $modal = [QA28]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "qa28-5-repair-confirm.png"
    Tap 0x1B   # Esc 取消
    Start-Sleep -Milliseconds 800
    $modal2 = [QA28]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    if ($modal2 -eq [IntPtr]::Zero) { Write-Output "  [OK] confirm cancelled by Esc" }
    else { Write-Output "  [WARN] modal still open"; Tap 0x1B; Start-Sleep -Milliseconds 600 }
  } else { Write-Output "  [WARN] no confirm modal for action 0" }
  # 「更多 ▾」菜单截图（danger 红色渲染）
  ClickId $hwnd 1019 1200
  $menu = [QA28]::FindWindowW("#32768", [IntPtr]::Zero)
  if ($menu -ne [IntPtr]::Zero) {
    $mr = New-Object QA28+RECT
    [QA28]::GetWindowRect($menu, [ref]$mr) | Out-Null
    ShotRect $mr "qa28-6-repair-more-menu.png"
    Tap 0x1B   # Esc 关菜单
    Start-Sleep -Milliseconds 700
  } else { Write-Output "  [WARN] popup menu not found" }
}

# 5) 常用管理 6 模块逐一切换截图（含侧栏图标走查）
$mods = @("系统还原点", "本地用户", "共享目录", "事件日志分析", "电源计划", "缺失更新检查")
$n = 1
foreach ($m in $mods) {
  $i = FindNav $nav $m
  if ($i -lt 0) { Write-Output "  [WARN] nav not found: $m"; continue }
  if (NavGo $hwnd $nav $i) {
    Start-Sleep -Milliseconds 500
    Shot $hwnd ("qa28-7-$n-" + ($m -replace '\s','') + ".png")
  }
  $n++
}

# 6) 主题回归：晴空白 → 深空夜（Ctrl+T ×3）→ 回晴空白（×1）
[QA28]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
$i = FindNav $nav "本地用户"
if ($i -ge 0) { NavGo $hwnd $nav $i | Out-Null }
Chord 0x11 0x54; Start-Sleep -Milliseconds 600
Chord 0x11 0x54; Start-Sleep -Milliseconds 600
Chord 0x11 0x54; Start-Sleep -Milliseconds 900
Shot $hwnd "qa28-8-deepspace.png"
Chord 0x11 0x54; Start-Sleep -Milliseconds 900
Shot $hwnd "qa28-9-daylight-back.png"

# 7) 缩放：Ctrl+= 一档 → 新模块页布局 → Ctrl+0 复位
[QA28]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
Chord 0x11 0xBB   # Ctrl + =
Start-Sleep -Milliseconds 900
Shot $hwnd "qa28-10-zoom-in.png"
Chord 0x11 0x30   # Ctrl + 0
Start-Sleep -Milliseconds 700

# 8) 稳定性：连续切换全部分组 ≥30 次
$cnt = [QA28]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$switches = 0; $crashed = $false
for ($k = 0; $k -lt 45 -and $switches -lt 32; $k++) {
  $idx = (($k * 7) + 3) % $cnt
  $t = NavTextAt $nav $idx
  if ($t -match " · \d+$") { continue }   # 跳过分组标题行
  if (-not (NavGo $hwnd $nav $idx)) { continue }
  $switches++
  $p.Refresh()
  if ($p.HasExited) { $crashed = $true; Write-Output "  [FAIL] crashed at switch $switches (nav idx $idx '$t')"; break }
}
Write-Output "  stability switches: $switches, crashed: $crashed"
Shot $hwnd "qa28-11-after-stability.png"

# 9) WM_CLOSE 正常退出
[QA28]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" }
else { Write-Output "  [WARN] still running after WM_CLOSE"; [QA28]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Seconds 2; $p.Refresh(); if ($p.HasExited) { Write-Output "  [OK] exited on second WM_CLOSE" } else { Write-Output "  [FAIL] not exiting"; Stop-Process -Id $p.Id -Force } }

Write-Output "== done =="
