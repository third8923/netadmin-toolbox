# qa28_menu_probe2.ps1 — 精确探测「更多 ▾」弹出菜单窗口（单类型块版本）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QB {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
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
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
$nav = [QB]::GetDlgItem($hwnd, 1001)

$idx = [QB]::SendMessageW($nav, 0x018F, [IntPtr](-1), "系统修复").ToInt32()   # LB_FINDSTRINGEXACT
Write-Output "nav idx of 系统修复: $idx"
[QB]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null   # LB_SETTOPINDEX
Start-Sleep -Milliseconds 300
$mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[QB]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null         # LB_GETITEMRECT
$b = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
$pt = New-Object QB+POINT
$pt.X = [int](([BitConverter]::ToInt32($b,0)) + ([BitConverter]::ToInt32($b,8))) / 2
$pt.Y = [int](([BitConverter]::ToInt32($b,4)) + ([BitConverter]::ToInt32($b,12))) / 2
[QB]::ClientToScreen($nav, [ref]$pt) | Out-Null
[QB]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
[QB]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 150
[QB]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 80
[QB]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 900
Write-Output "clicked nav item"

$btnMore = [QB]::GetDlgItem($hwnd, 1019)
[QB]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, $btnMore) | Out-Null
Start-Sleep -Milliseconds 1500

$script:menuRect = $null
$cb = [QB+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [QB]::GetClassNameW($h, $sb, 256) | Out-Null
  if ($sb.ToString() -eq "#32768") {
    $rr = New-Object QB+RECT
    [QB]::GetWindowRect($h, [ref]$rr) | Out-Null
    $vis = [QB]::IsWindowVisible($h)
    Write-Output ("menu hwnd={0} visible={1} rect=({2},{3})-({4},{5}) size={6}x{7}" -f $h, $vis, $rr.Left, $rr.Top, $rr.Right, $rr.Bottom, ($rr.Right-$rr.Left), ($rr.Bottom-$rr.Top))
    if ($vis -and ($rr.Right - $rr.Left) -gt 10) { $script:menuRect = $rr }
  }
  return $true
}
[QB]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

if ($script:menuRect) {
  $mr = $script:menuRect
  $w = ($mr.Right - $mr.Left) + 60; $ht = ($mr.Bottom - $mr.Top) + 60
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($mr.Left - 30, $mr.Top - 30, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $dir "tools\qa28-6-repair-more-menu.png"))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  captured menu region ($w x $ht)"
} else { Write-Output "  [WARN] no visible #32768 found" }

[QB]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[QB]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 700
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
