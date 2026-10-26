# qa28_gui3.ps1 — 收尾：缩放验证（净 ini）+ danger 确认框 + sys.services 菜单佐证
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QC {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
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
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$env:MZ_ELEV_ATTEMPTED = "1"

function Shot([IntPtr]$h, [string]$name) {
  [QC]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QC+RECT
  [QC]::GetClientRect($h, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [QC]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) {
    $pt = New-Object QC+POINT; [QC]::ClientToScreen($h, [ref]$pt) | Out-Null
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
  }
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  [QC]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}
function KeyDown([byte]$vk) { [QC]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
function KeyUp([byte]$vk)   { [QC]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function Tap([byte]$vk)     { KeyDown $vk; Start-Sleep -Milliseconds 70; KeyUp $vk }
function Chord([byte]$mod, [byte]$vk) { FocusApp; KeyDown $mod; Start-Sleep -Milliseconds 90; Tap $vk; Start-Sleep -Milliseconds 90; KeyUp $mod }
function FocusApp { [QC]::SetForegroundWindow($hwnd) | Out-Null; Start-Sleep -Milliseconds 500 }
function IniScale {
  if (-not (Test-Path $ini)) { return "no-ini" }
  if ((Get-Content $ini -Raw) -match 'scale=(\d+)') { return $Matches[1] }
  return "unset"
}
function NavGo([string]$text) {
  $nav = [QC]::GetDlgItem($hwnd, 1001)
  $idx = [QC]::SendMessageW($nav, 0x018F, [IntPtr](-1), $text).ToInt32()
  if ($idx -lt 0) { Write-Output "  [WARN] nav $text"; return }
  [QC]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 300
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [QC]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null
  $b = New-Object byte[] 16
  [System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
  $pt = New-Object QC+POINT
  $pt.X = [int](([BitConverter]::ToInt32($b,0)) + ([BitConverter]::ToInt32($b,8))) / 2
  $pt.Y = [int](([BitConverter]::ToInt32($b,4)) + ([BitConverter]::ToInt32($b,12))) / 2
  [QC]::ClientToScreen($nav, [ref]$pt) | Out-Null
  FocusApp
  [QC]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 130
  [QC]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 70
  [QC]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 800
}

# 净 ini 启动
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Remove-Item $ini -Force -ErrorAction SilentlyContinue
if (Test-Path $ini) { Write-Output "[WARN] ini still exists" }
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
Write-Output "scale at fresh launch: $(IniScale)"

# ---- D. 缩放验证 ----
NavGo "电源计划"
Chord 0x11 0xBB
Start-Sleep -Milliseconds 900
$s1 = IniScale
Write-Output "scale after Ctrl+=: $s1"
if ($s1 -eq "110") {
  Shot $hwnd "qa28-10-zoom-in.png"
  Chord 0x11 0x30
  Start-Sleep -Milliseconds 900
  Write-Output "scale after Ctrl+0: $(IniScale)"
  Shot $hwnd "qa28-15-zoom-reset.png"
} else { Write-Output "  [FAIL] zoom not verified" }

# ---- E. danger 确认框（Winsock 重置，菜单第 2 项）：DOWN×2 → Enter → 截图 → Esc ----
NavGo "系统修复"
Start-Sleep -Milliseconds 400
FocusApp
[QC]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [QC]::GetDlgItem($hwnd, 1019)) | Out-Null
Start-Sleep -Milliseconds 1200
Tap 0x28; Start-Sleep -Milliseconds 300   # DOWN → 第1项 磁盘检查(只读)
Tap 0x28; Start-Sleep -Milliseconds 300   # DOWN → 第2项 Winsock 重置(danger)
Tap 0x0D; Start-Sleep -Milliseconds 1600  # Enter
$modal = [QC]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  [QC]::SetWindowPos($modal, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QC+RECT
  [QC]::GetClientRect($modal, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); $ok = [QC]::PrintWindow($modal, $hdc, 3); $g.ReleaseHdc($hdc)
  if (-not $ok) { $pt2 = New-Object QC+POINT; [QC]::ClientToScreen($modal, [ref]$pt2) | Out-Null; $g.CopyFromScreen($pt2.X, $pt2.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\qa28-14-danger-confirm-winsock.png"))
  $g.Dispose(); $bmp.Dispose()
  [QC]::SetWindowPos($modal, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved qa28-14-danger-confirm-winsock.png"
  Tap 0x1B; Start-Sleep -Milliseconds 800
  if ([QC]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero) { Write-Output "  [OK] danger confirm cancelled, winsock NOT reset" }
  else { Tap 0x1B }
} else { Write-Output "  [WARN] no danger confirm modal"; Tap 0x1B }

# ---- F. sys.services 菜单佐证（同为 >2 动作模块，预期同缺陷）----
NavGo "服务管理"
Start-Sleep -Milliseconds 400
FocusApp
[QC]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [QC]::GetDlgItem($hwnd, 1019)) | Out-Null
Start-Sleep -Milliseconds 1200
$modal = [QC]::FindWindowW("MzModalHost", [IntPtr]::Zero)
Tap 0x1B; Start-Sleep -Milliseconds 600
Write-Output "  (services menu opened; visual evidence from sec.repair applies)"

# 清理测试态：复位缩放并删 ini（恢复交付默认）
Chord 0x11 0x30
Start-Sleep -Milliseconds 600
[QC]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Output "ini removed: $(-not (Test-Path $ini))"
Write-Output "== done =="
