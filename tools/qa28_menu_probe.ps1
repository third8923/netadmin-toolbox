# qa28_menu_probe.ps1 — 精确探测「更多 ▾」弹出菜单窗口尺寸与渲染
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class MP {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
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
$nav = [MP]::GetDlgItem($hwnd, 1001)

# 切到系统修复
$cnt = [MP]::PostMessageW
# 用 LB_FINDSTRINGEXACT 找「系统修复」
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("系统修复")
$idx = [MP]::FindWindowW; # placeholder
# LB_FINDSTRINGEXACT = 0x018F
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MP2 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
}
"@
$idx = [MP2]::SendMessageW($nav, 0x018F, [IntPtr](-1), "系统修复").ToInt32()
Write-Output "nav idx of 系统修复: $idx"
# 选中并通知（真实点击）
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MP3 {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out MP.RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
}
"@
[MP3]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object MP+RECT
[MP3]::GetWindowRect($nav, [ref]$r) | Out-Null
# 估算行位置：分组行 ~30px、模块行 ~44px — 直接逐行往下点不可靠，改用 LB_GETITEMRECT
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MP4 {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref MP.POINT p);
}
"@
$mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[MP4]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null
$b = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
$lx = [BitConverter]::ToInt32($b,0); $ty = [BitConverter]::ToInt32($b,4); $rx = [BitConverter]::ToInt32($b,8); $by = [BitConverter]::ToInt32($b,12)
$pt = New-Object MP+POINT
$pt.X = [int](($lx+$rx)/2); $pt.Y = [int](($ty+$by)/2)
[MP4]::ClientToScreen($nav, [ref]$pt) | Out-Null
[MP]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
[MP3]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 150
[MP3]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null; Start-Sleep -Milliseconds 80; [MP3]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 800

# 打开「更多 ▾」菜单
$btnMore = [MP]::GetDlgItem($hwnd, 1019)
[MP]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, $btnMore) | Out-Null
Start-Sleep -Milliseconds 1200

# 枚举所有 #32768 窗口
$found = @()
$cb = [MP+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [MP]::GetClassNameW($h, $sb, 256) | Out-Null
  if ($sb.ToString() -eq "#32768") {
    $rr = New-Object MP+RECT
    [MP]::GetWindowRect($h, [ref]$rr) | Out-Null
    $vis = [MP]::IsWindowVisible($h)
    Write-Output ("menu hwnd={0} visible={1} rect=({2},{3})-({4},{5}) size={6}x{7}" -f $h, $vis, $rr.Left, $rr.Top, $rr.Right, $rr.Bottom, ($rr.Right-$rr.Left), ($rr.Bottom-$rr.Top))
    if ($vis) { $script:menuRect = $rr }
  }
  return $true
}
[MP]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

if ($script:menuRect) {
  $mr = $script:menuRect
  $w = [Math]::Max(($mr.Right-$mr.Left)+40, 60); $ht = [Math]::Max(($mr.Bottom-$mr.Top)+40, 60)
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($mr.Left-20, $mr.Top-20, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $dir "tools\qa28-6-repair-more-menu.png"))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  captured menu region"
} else { Write-Output "  [WARN] no visible #32768 found" }

# Esc 关闭
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MP5 { [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra); }
"@
[MP5]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [MP5]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 700
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
