# t02_fix_rows.ps1 — clean.basic 行数读取修正复验（LVM_GETITEMCOUNT=0x1004）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T02F {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T02F]::GetDlgItem($hwnd, 1001)
# 程序化选中行不允许；此处仅验证 clean.basic 扫描数据行，用 FindNav+真实点击
$buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
$target = -1
$cnt = [T02F]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
for ($i = 0; $i -lt $cnt; $i++) {
  [T02F]::SendMessageW($nav, 0x0189, [IntPtr]$i, $buf) | Out-Null
  if ([System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf) -eq "基础清理") { $target = $i; break }
}
if ($target -lt 0) { Write-Output "[FAIL] nav not found"; Stop-Process -Id $p.Id -Force; exit 1 }
# 滚动到可见 + 真实点击（复用 T01 结论：必须真实鼠标点击）
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T02G {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct POINT { public int X, Y; }
}
"@
$mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[T02G]::SendMessageW($nav, 0x0197, [IntPtr]$target, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 300
[T02G]::SendMessageW($nav, 0x0198, [IntPtr]$target, $mem) | Out-Null   # LB_GETITEMRECT
$bytes = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem, $bytes, 0, 16)
$L = [BitConverter]::ToInt32($bytes, 0); $T = [BitConverter]::ToInt32($bytes, 4)
$R = [BitConverter]::ToInt32($bytes, 8); $B = [BitConverter]::ToInt32($bytes, 12)
$pt = New-Object T02G+POINT
$pt.X = [int](($L + $R) / 2); $pt.Y = [int](($T + $B) / 2)
[T02G]::ClientToScreen($nav, [ref]$pt) | Out-Null
[T02G]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 200
[T02G]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 120
[T02G]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 70
[T02G]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 600
# 开始扫描
[T02F]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [T02F]::GetDlgItem($hwnd, 1010)) | Out-Null
$rows = -1
for ($k = 0; $k -lt 90; $k++) {
  $rows = [T02F]::SendMessageW([T02F]::GetDlgItem($hwnd, 1002), 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()  # LVM_GETITEMCOUNT
  if ($rows -gt 0) { break }
  Start-Sleep -Milliseconds 500
}
Write-Output ("clean.basic rows (LVM_GETITEMCOUNT): " + $rows)
if ($rows -gt 0) { Write-Output "[PASS] clean.basic 扫描回归" } else { Write-Output "[FAIL] rows=0" }
Stop-Process -Id $p.Id -Force
