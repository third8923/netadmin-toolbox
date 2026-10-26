# qa28_gui2.ps1 — v2.8 GUI 走查补测（QA 严过关）
# 补测项：①sec.repair 动作0确认框(按钮1011)+取消 ②「更多 ▾」菜单截图 ③danger 菜单项确认框+取消
#         ④深空夜主题（模态卡片点击，ini 校验）+ 回晴空白 ⑤缩放 Ctrl+=/Ctrl+0（ini 校验）
#         ⑥本地用户扫描数据页 ⑦稳定性 30 次切换 + WM_CLOSE
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Q28B {
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
$env:MZ_ELEV_ATTEMPTED = "1"

function Shot([IntPtr]$h, [string]$name) {
  [Q28B]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object Q28B+RECT
  [Q28B]::GetClientRect($h, [ref]$rc) | Out-Null
  $pt = New-Object Q28B+POINT
  [Q28B]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [Q28B]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  [Q28B]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}

function ShotRegion([int]$x, [int]$y, [int]$w, [int]$ht, [string]$name) {
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

function KeyDown([byte]$vk) { [Q28B]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
function KeyUp([byte]$vk)   { [Q28B]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function Tap([byte]$vk)     { KeyDown $vk; Start-Sleep -Milliseconds 70; KeyUp $vk }
function RealClick([int]$x, [int]$y) {
  [Q28B]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 130
  [Q28B]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [Q28B]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 200
}
function ClickId([IntPtr]$h, [int]$id, [int]$wait = 900) {
  $b = [Q28B]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [Q28B]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [Q28B]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
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
    $len = [Q28B]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavGo([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  [Q28B]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  $r = New-Object Q28B+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { return $false }
  $pt = New-Object Q28B+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [Q28B]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [Q28B]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 250
  RealClick $pt.X $pt.Y
  Start-Sleep -Milliseconds 650
  return $true
}
function FindNav([IntPtr]$nav, [string]$text) {
  $cnt = [Q28B]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  for ($i = 0; $i -lt $cnt; $i++) { if ((NavTextAt $nav $i) -eq $text) { return $i } }
  return -1
}
function IniTheme {
  $f = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $f)) { return "no-ini" }
  if ((Get-Content $f -Raw) -match 'theme=([a-z]+)') { return $Matches[1] }
  return "unset"
}
function IniScale {
  $f = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $f)) { return "no-ini" }
  if ((Get-Content $f -Raw) -match 'scale=(\d+)') { return $Matches[1] }
  return "unset"
}
function FocusApp([IntPtr]$h) {
  [Q28B]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
}
function ChordVerified([IntPtr]$h, [byte]$mod, [byte]$vk, [string]$expect, [int]$maxTry) {
  for ($t = 1; $t -le $maxTry; $t++) {
    FocusApp $h
    KeyDown $mod; Start-Sleep -Milliseconds 90; Tap $vk; Start-Sleep -Milliseconds 90; KeyUp $mod
    Start-Sleep -Milliseconds 900
    $cur = IniTheme; $sc = IniScale
    if ($expect -eq "scale110") { if ($sc -eq "110") { Write-Output "  [OK] try $t scale=$sc"; return $true } }
    elseif ($cur -eq $expect) { Write-Output "  [OK] try $t theme=$cur"; return $true }
    else { Write-Output "  [retry $t] theme=$cur scale=$sc (expect $expect)" }
  }
  return $false
}

# ============ 开始 ============
Write-Output "== launch =="
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero -or $p.HasExited) { Write-Output "[FAIL] no window"; exit 1 }
$nav = [Q28B]::GetDlgItem($hwnd, 1001)
Write-Output "  theme at launch: $(IniTheme)"

# ---- A. sec.repair 确认框 + 更多菜单 ----
$i = FindNav $nav "系统修复"
if ($i -lt 0) { Write-Output "[FAIL] nav 系统修复" }
else {
  NavGo $hwnd $nav $i | Out-Null
  Start-Sleep -Milliseconds 400
  # A1. 动作 0（sfc）确认框：按钮 1011 = Actions[0]
  ClickId $hwnd 1011 1500
  $modal = [Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "qa28-5-repair-sfc-confirm.png"
    Tap 0x1B; Start-Sleep -Milliseconds 800
    if ([Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero) { Write-Output "  [OK] sfc confirm cancelled (Esc), sfc NOT run" }
    else { Tap 0x1B; Start-Sleep -Milliseconds 600; Write-Output "  [WARN] modal persisted" }
  } else { Write-Output "  [WARN] no confirm modal on 1011" }
  # A2. 「更多 ▾」菜单（owner-draw，danger 红色）：整屏区域截图
  $btnMore = [Q28B]::GetDlgItem($hwnd, 1019)
  $br = New-Object Q28B+RECT
  [Q28B]::GetWindowRect($btnMore, [ref]$br) | Out-Null
  FocusApp $hwnd
  ClickId $hwnd 1019 1100
  ShotRegion ($br.Left - 40) ($br.Bottom - 6) 420 340 "qa28-6-repair-more-menu.png"
  Tap 0x1B; Start-Sleep -Milliseconds 700
  # A3. danger 动作确认框（菜单第 2 项 Winsock 重置）：DOWN→Enter→确认框→截图→Esc 取消
  FocusApp $hwnd
  ClickId $hwnd 1019 1100
  Start-Sleep -Milliseconds 400
  Tap 0x28; Start-Sleep -Milliseconds 350   # DOWN → 第2项 Winsock 重置(danger)
  Tap 0x0D; Start-Sleep -Milliseconds 1500  # Enter → 应弹确认框
  $modal = [Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "qa28-12-danger-confirm.png"
    Tap 0x1B; Start-Sleep -Milliseconds 800
    if ([Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero) { Write-Output "  [OK] danger confirm cancelled (Esc), winsock NOT reset" }
    else { Tap 0x1B; Start-Sleep -Milliseconds 600 }
  } else { Write-Output "  [WARN] no confirm modal after menu item"; Tap 0x1B }
}

# ---- B. 本地用户扫描数据页 ----
$i = FindNav $nav "本地用户"
if ($i -ge 0) {
  NavGo $hwnd $nav $i | Out-Null
  ClickId $hwnd 1010 800
  Start-Sleep -Seconds 3
  Shot $hwnd "qa28-7-2-本地用户.png"
}

# ---- C. 主题：模态卡片点击 深空夜 → 截图 → 模态点击 晴空白 → 截图 ----
$i = FindNav $nav "共享目录"
if ($i -ge 0) { NavGo $hwnd $nav $i | Out-Null; Start-Sleep -Milliseconds 300 }
function PickTheme([IntPtr]$h, [string]$cardPos, [string]$expect) {
  for ($t = 1; $t -le 4; $t++) {
    FocusApp $h
    ClickId $h 1018 1200
    $modal = [Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    if ($modal -eq [IntPtr]::Zero) { Write-Output "  [WARN] theme modal not open"; continue }
    if ($t -eq 1 -and $cardPos -eq "deepspace") { Shot $modal "qa28-13-theme-modal.png" }
    $rc = New-Object Q28B+RECT
    [Q28B]::GetClientRect($modal, [ref]$rc) | Out-Null
    $pt = New-Object Q28B+POINT
    if ($cardPos -eq "deepspace") { $pt.X = [int]($rc.Right * 0.75); $pt.Y = [int]($rc.Bottom * 0.66) }
    else { $pt.X = [int]($rc.Right * 0.25); $pt.Y = [int]($rc.Bottom * 0.30) }
    [Q28B]::ClientToScreen($modal, [ref]$pt) | Out-Null
    RealClick $pt.X $pt.Y
    Start-Sleep -Milliseconds 1000
    if ([Q28B]::FindWindowW("MzModalHost", [IntPtr]::Zero) -ne [IntPtr]::Zero) { Tap 0x1B; Start-Sleep -Milliseconds 700 }
    if ((IniTheme) -eq $expect) { Write-Output "  [OK] theme now $(IniTheme) (try $t)"; return $true }
    Write-Output "  [retry] theme=$(IniTheme) expect=$expect"
  }
  return $false
}
Write-Output "== theme deepspace =="
PickTheme $hwnd "deepspace" "deepspace" | Out-Null
Start-Sleep -Milliseconds 600
Shot $hwnd "qa28-8-deepspace.png"
Write-Output "== theme back daylight =="
PickTheme $hwnd "daylight" "daylight" | Out-Null
Start-Sleep -Milliseconds 600
Shot $hwnd "qa28-9-daylight-back.png"

# ---- D. 缩放 Ctrl+= 一档 → 截图 → Ctrl+0 复位 ----
Write-Output "== zoom =="
Write-Output "  scale before: $(IniScale)"
if (ChordVerified $hwnd 0x11 0xBB "scale110" 4) {
  Start-Sleep -Milliseconds 500
  Shot $hwnd "qa28-10-zoom-in.png"
  FocusApp $hwnd
  KeyDown 0x11; Start-Sleep -Milliseconds 90; Tap 0x30; Start-Sleep -Milliseconds 90; KeyUp 0x11
  Start-Sleep -Milliseconds 900
  Write-Output "  scale after reset: $(IniScale)"
} else { Write-Output "  [FAIL] zoom chord not verified" }

# ---- E. 稳定性：连续切换 ≥30 次 ----
Write-Output "== stability =="
$cnt = [Q28B]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$switches = 0; $crashed = $false
for ($k = 0; $k -lt 60 -and $switches -lt 32; $k++) {
  $idx = (($k * 5) + 3) % $cnt
  $t = NavTextAt $nav $idx
  if ($t -match " · \d+$") { continue }
  if (-not (NavGo $hwnd $nav $idx)) { continue }
  $switches++
  $p.Refresh()
  if ($p.HasExited) { $crashed = $true; Write-Output "  [FAIL] crashed at switch $switches"; break }
}
Write-Output "  switches: $switches, crashed: $crashed"
Shot $hwnd "qa28-11-after-stability.png"

# ---- F. WM_CLOSE ----
[Q28B]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" }
else { Write-Output "  [WARN] still running; retry"; [Q28B]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Seconds 2; $p.Refresh(); if ($p.HasExited) { Write-Output "  [OK] exited" } else { Write-Output "  [FAIL]"; Stop-Process -Id $p.Id -Force } }

Write-Output "  final theme: $(IniTheme), scale: $(IniScale)"
Write-Output "== done =="
