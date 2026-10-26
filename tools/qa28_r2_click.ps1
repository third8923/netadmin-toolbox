# qa28_r2_click.ps1 — Round 2 补充：菜单鼠标点选 → 确认框 → Esc 取消（三页）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class R3 {
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
$env:MZ_ELEV_ATTEMPTED = "1"

function Tap([byte]$vk) { [R3]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [R3]::keybd_event($vk,0,2,[UIntPtr]::Zero) }
function NavGo([string]$text) {
  $nav = [R3]::GetDlgItem($hwnd, 1001)
  $idx = [R3]::SendMessageW($nav, 0x018F, [IntPtr](-1), $text).ToInt32()
  if ($idx -lt 0) { Write-Host "  [WARN] nav $text"; return }
  [R3]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 300
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [R3]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null
  $b = New-Object byte[] 16
  [System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
  $pt = New-Object R3+POINT
  $pt.X = [int](([BitConverter]::ToInt32($b,0)) + ([BitConverter]::ToInt32($b,8))) / 2
  $pt.Y = [int](([BitConverter]::ToInt32($b,4)) + ([BitConverter]::ToInt32($b,12))) / 2
  [R3]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [R3]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 400
  [R3]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 130
  [R3]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 70
  [R3]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 800
}
function GetMenuRect {
  $script:mr = $null
  $cb = [R3+EnumProc]{
    param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [R3]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "#32768") {
      $rr = New-Object R3+RECT
      [R3]::GetWindowRect($h, [ref]$rr) | Out-Null
      if ([R3]::IsWindowVisible($h)) { $script:mr = $rr }
    }
    return $true
  }
  [R3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  if ($script:mr) {
    $w = $script:mr.Right - $script:mr.Left; $ht = $script:mr.Bottom - $script:mr.Top
    Write-Host ("  menu size = {0} x {1}" -f $w, $ht)
  } else { Write-Host "  [WARN] no visible menu" }
}
function ClickFirstAndCancel([string]$tag, [string]$shot) {
  if (-not $script:mr) { Tap 0x1B; Start-Sleep -Milliseconds 500; return }
  $x = $script:mr.Left + [int](($script:mr.Right - $script:mr.Left) / 2)
  $y = $script:mr.Top + 28
  [R3]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 200
  [R3]::mouse_event(2, $x, $y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 80
  [R3]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 1600
  $modal = [R3]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -eq [IntPtr]::Zero) { Write-Host "  [$tag] [WARN] no confirm modal"; Tap 0x1B; Start-Sleep -Milliseconds 500; return }
  [R3]::SetWindowPos($modal, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object R3+RECT
  [R3]::GetClientRect($modal, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); $ok = [R3]::PrintWindow($modal, $hdc, 3); $g.ReleaseHdc($hdc)
  if (-not $ok) { $pt2 = New-Object R3+POINT; [R3]::ClientToScreen($modal, [ref]$pt2) | Out-Null; $g.CopyFromScreen($pt2.X, $pt2.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$shot")); $g.Dispose(); $bmp.Dispose()
  [R3]::SetWindowPos($modal, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Host "  saved $shot"
  Tap 0x1B; Start-Sleep -Milliseconds 800
  if ([R3]::FindWindowW("MzModalHost", [IntPtr]::Zero) -eq [IntPtr]::Zero) { Write-Host "  [$tag] [OK] confirm cancelled by Esc (nothing executed)" }
  else { Tap 0x1B; Start-Sleep -Milliseconds 600; Write-Host "  [$tag] [WARN] modal persisted, forced Esc" }
}

Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "[FAIL] no window"; exit 1 }

foreach ($case in @(@("系统修复","sec.repair","qa28-r2-1b-repair-confirm.png"),
                    @("服务管理","sys.services","qa28-r2-2b-services-confirm.png"),
                    @("计划任务","sys.tasks","qa28-r2-3b-tasks-confirm.png"))) {
  Write-Host ("== {0} ==" -f $case[1])
  NavGo $case[0]
  Start-Sleep -Milliseconds 400
  [R3]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, [R3]::GetDlgItem($hwnd, 1019)) | Out-Null
  Start-Sleep -Milliseconds 1300
  GetMenuRect
  ClickFirstAndCancel $case[1] $case[2]
}
[R3]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Host "== done =="
