# qa28_r2_menu.ps1 — Round 2 定向回归：Bug-1「更多 ▾」菜单修复验证（QA 严过关独立复测）
# 三页（系统修复/服务管理/计划任务）：菜单展开尺寸枚举 + 截图 + danger 红 + 鼠标点选→确认框→Esc 取消
# 另：clean.basic 无动作模块 btnMore 隐藏回归
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class R2 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
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
  [R2]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object R2+RECT
  [R2]::GetClientRect($h, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); $ok = [R2]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc)
  if (-not $ok) { $pt = New-Object R2+POINT; [R2]::ClientToScreen($h, [ref]$pt) | Out-Null; $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name")); $g.Dispose(); $bmp.Dispose()
  [R2]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}
function Tap([byte]$vk) { [R2]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [R2]::keybd_event($vk,0,2,[UIntPtr]::Zero) }
function NavGo([string]$text) {
  $nav = [R2]::GetDlgItem($hwnd, 1001)
  $idx = [R2]::SendMessageW($nav, 0x018F, [IntPtr](-1), $text).ToInt32()
  if ($idx -lt 0) { Write-Output "  [WARN] nav $text not found"; return $false }
  [R2]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 300
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [R2]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null
  $b = New-Object byte[] 16
  [System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
  $pt = New-Object R2+POINT
  $pt.X = [int](([BitConverter]::ToInt32($b,0)) + ([BitConverter]::ToInt32($b,8))) / 2
  $pt.Y = [int](([BitConverter]::ToInt32($b,4)) + ([BitConverter]::ToInt32($b,12))) / 2
  [R2]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [R2]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 400
  [R2]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 130
  [R2]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 70
  [R2]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 800
  return $true
}
function ProbeMenu([string]$pageTag, [string]$shotName) {
  $script:menuRect = $null
  $cb = [R2+EnumProc]{
    param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [R2]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "#32768") {
      $rr = New-Object R2+RECT
      [R2]::GetWindowRect($h, [ref]$rr) | Out-Null
      if ([R2]::IsWindowVisible($h)) { $script:menuRect = $rr }
    }
    return $true
  }
  [R2]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  if (-not $script:menuRect) { Write-Output "  [$pageTag] [WARN] no visible menu"; return $null }
  $mr = $script:menuRect
  $w = $mr.Right - $mr.Left; $ht = $mr.Bottom - $mr.Top
  Write-Output ("  [{0}] menu size = {1} x {2}" -f $pageTag, $w, $ht)
  $cw = $w + 80; $cht = $ht + 80
  $bmp = New-Object System.Drawing.Bitmap $cw, $cht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($mr.Left - 40, $mr.Top - 40, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $dir "tools\$shotName")); $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $shotName"
  return $mr
}
function MenuClickAndCancel([R2+RECT]$mr, [string]$pageTag, [string]$shotName) {
  # 鼠标真实点击菜单第 1 项（约在菜单顶部 10px + 17px 中线）
  $x = $mr.Left + [int](($mr.Right - $mr.Left) / 2)
  $y = $mr.Top + 28
  [R2]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 200
  [R2]::mouse_event(2, $x, $y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 80
  [R2]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 1600
  $modal = [R2]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -eq [IntPtr]::Zero) { Write-Output "  [$pageTag] [WARN] no confirm modal after menu click"; Tap 0x1B; Start-Sleep -Milliseconds 500; return $false }
  [R2]::SetWindowPos($modal, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object R2+RECT
  [R2]::GetClientRect($modal, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); $ok2 = [R2]::PrintWindow($modal, $hdc, 3); $g.ReleaseHdc($hdc)
  if (-not $ok2) { $pt2 = New-Object R2+POINT; [R2]::ClientToScreen($modal, [ref]$pt2) | Out-Null; $g.CopyFromScreen($pt2.X, $pt2.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$shotName")); $g.Dispose(); $bmp.Dispose()
  [R2]::SetWindowPos($modal, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $shotName"
  Tap 0x1B; Start-Sleep -Milliseconds 800
  if ([R2]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero) { Write-Output "  [$pageTag] [OK] confirm cancelled by Esc (nothing executed)" }
  else { Tap 0x1B; Start-Sleep -Milliseconds 600; Write-Output "  [$pageTag] [WARN] modal persisted, forced Esc" }
  return $true
}
function CloseMenu { Tap 0x1B; Start-Sleep -Milliseconds 700 }

# ===== 净环境启动 =====
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Remove-Item $ini -Force -ErrorAction SilentlyContinue
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero -or $p.HasExited) { Write-Output "[FAIL] no window"; exit 1 }
Write-Output "== R2 menu regression =="

# ---- 1) sec.repair 菜单 ----
if (NavGo "系统修复") {
  Start-Sleep -Milliseconds 400
  [R2]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [R2]::GetDlgItem($hwnd, 1019)) | Out-Null
  Start-Sleep -Milliseconds 1300
  $mr = ProbeMenu "sec.repair" "qa28-r2-1-repair-menu.png"
  if ($mr) {
    MenuClickAndCancel $mr "sec.repair" "qa28-r2-1b-repair-confirm.png" | Out-Null
  } else { CloseMenu }
}

# ---- 2) sys.services 菜单 ----
if (NavGo "服务管理") {
  Start-Sleep -Milliseconds 400
  [R2]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [R2]::GetDlgItem($hwnd, 1019)) | Out-Null
  Start-Sleep -Milliseconds 1300
  $mr = ProbeMenu "sys.services" "qa28-r2-2-services-menu.png"
  if ($mr) {
    MenuClickAndCancel $mr "sys.services" "qa28-r2-2b-services-confirm.png" | Out-Null
  } else { CloseMenu }
}

# ---- 3) sys.tasks 菜单 ----
if (NavGo "计划任务") {
  Start-Sleep -Milliseconds 400
  [R2]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [R2]::GetDlgItem($hwnd, 1019)) | Out-Null
  Start-Sleep -Milliseconds 1300
  $mr = ProbeMenu "sys.tasks" "qa28-r2-3-tasks-menu.png"
  if ($mr) {
    MenuClickAndCancel $mr "sys.tasks" "qa28-r2-3b-tasks-confirm.png" | Out-Null
  } else { CloseMenu }
}

# ---- 4) clean.basic 按钮行回归（无动作模块 btnMore 应隐藏）----
if (NavGo "基础清理") {
  Start-Sleep -Milliseconds 600
  $bm = [R2]::GetDlgItem($hwnd, 1019)
  $vis = [R2]::IsWindowVisible($bm)
  Write-Output "  [clean.basic] btnMore(1019) visible = $vis (expect False)"
  Shot $hwnd "qa28-r2-4-cleanbasic.png"
}

# ---- 5) 稳定性抽查：10 次切换 + WM_CLOSE ----
$nav = [R2]::GetDlgItem($hwnd, 1001)
$cnt = [R2]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$sw = 0
for ($k = 0; $k -lt 20 -and $sw -lt 10; $k++) {
  $idx = (($k * 11) + 5) % $cnt
  # 逐项取文本判分组
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  $len = [R2]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
  $t = ""
  if ($len -gt 0) { $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf) }
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
  if ($t -match " · \d+$") { continue }
  if (-not (NavGo $t)) { continue }
  $sw++
  $p.Refresh()
  if ($p.HasExited) { Write-Output "  [FAIL] crashed at switch $sw"; break }
}
Write-Output "  stability spot switches: $sw, alive: $(-not $p.HasExited)"
[R2]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
Write-Output "  WM_CLOSE clean exit: $($p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Output "== done =="
