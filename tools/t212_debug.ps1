# t212_debug.ps1 — 第二次启动回填诊断
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class D212 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendRect(IntPtr h, uint m, IntPtr w, ref RECT l);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder t, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$exe = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\netadmin-toolbox.exe"
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$h = [D212]::FindWindowW("NetAdminToolboxWnd", [IntPtr]::Zero)
$nav = [IntPtr]::Zero
# 枚举找 nav
1..1 | Out-Null
$nav = [IntPtr][IntPtr]::Zero
$cb = {
  param($hh, $l)
  return $true
}
# 直接用 GetDlgCtrlID 遍历：EnumChildWindows 未在此脚本声明，改用 GetDlgItem 不适用于自定义父。
# 简化：重新声明 EnumChildWindows
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class D212B {
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$found = @{}
$cb2 = [D212B+EnumProc]{ param($hh,$l) $id=[D212B]::GetDlgCtrlID($hh); if(-not $found.ContainsKey($id)){ $found[$id]=$hh }; return $true }
[D212B]::EnumChildWindows($h, $cb2, [IntPtr]::Zero) | Out-Null
$nav = $found[1001]
$LB_GETCOUNT=0x018B; $LB_GETTEXT=0x0189; $LB_GETITEMRECT=0x0198; $LB_SETTOPINDEX=0x0197; $LB_GETCURSEL=0x0188
$cnt = [int][D212]::Send($nav, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$row = -1
for ($i=0; $i -lt $cnt; ++$i) {
  $sb = New-Object System.Text.StringBuilder 256
  [D212]::SendText($nav, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
  if ($sb.ToString() -eq "计算机名与域") { $row = $i; break }
}
Write-Output ("row=" + $row)
[D212]::Send($nav, $LB_SETTOPINDEX, [IntPtr]$row, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$rc = New-Object D212+RECT; [D212]::GetClientRect($nav, [ref]$rc) | Out-Null
$top = [int][D212]::Send($nav, 0x018E, [IntPtr]::Zero, [IntPtr]::Zero)
$y = 0
for ($k=$top; $k -lt $row; ++$k) { $y += [int][D212]::Send($nav, 0x01A1, [IntPtr]$k, [IntPtr]::Zero) }
$hR = [int][D212]::Send($nav, 0x01A1, [IntPtr]$row, [IntPtr]::Zero)
$pt = New-Object D212+POINT
$pt.X = [int](($rc.Left+$rc.Right)/2); $pt.Y = $rc.Top + $y + [int]($hR/2)
[D212]::ClientToScreen($nav, [ref]$pt) | Out-Null
[D212]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
[D212]::SetCursorPos($pt.X,$pt.Y) | Out-Null; Start-Sleep -Milliseconds 150
[D212]::mouse_event(2,0,0,0,0); [D212]::mouse_event(4,0,0,0,0)
Start-Sleep -Milliseconds 900
$cur = [int][D212]::Send($nav, $LB_GETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Output ("cursel=" + $cur + " (want " + $row + ")")
$found2 = @{}
$cb3 = [D212B+EnumProc]{ param($hh,$l) $id=[D212B]::GetDlgCtrlID($hh); $found2[$id]=$hh; return $true }
[D212B]::EnumChildWindows($h, $cb3, [IntPtr]::Zero) | Out-Null
foreach ($id in 1020,1021,1032,1033,1034,1035) {
  $sb = New-Object System.Text.StringBuilder 512
  [D212]::GetWindowTextW($found2[$id], $sb, 512) | Out-Null
  $vis = (([D212B]::GetWindowLongW($found2[$id], -16)) -band 0x10000000) -ne 0
  Write-Output ("id " + $id + " vis=" + $vis + " text=[" + $sb.ToString() + "]")
}
Stop-Process -Id $p.Id -Force
