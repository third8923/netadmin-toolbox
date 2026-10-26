# t212_pw_debug.ps1 — 读取 1033 密码字段实际值
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class P212 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$exe = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\netadmin-toolbox.exe"
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$h = [P212]::FindWindowW("NetAdminToolboxWnd", [IntPtr]::Zero)
$found = @{}
$cb = [P212+EnumProc]{ param($hh,$l) $id=[P212]::GetDlgCtrlID($hh); if(-not $found.ContainsKey($id)){ $found[$id]=$hh }; return $true }
[P212]::EnumChildWindows($h, $cb, [IntPtr]::Zero) | Out-Null
$nav = $found[1001]
$cnt = [int][P212]::Send($nav, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
$row = -1
for ($i=0; $i -lt $cnt; ++$i) {
  $sb = New-Object System.Text.StringBuilder 256
  [P212]::SendText($nav, 0x0189, [IntPtr]$i, $sb) | Out-Null
  if ($sb.ToString() -eq "计算机名与域") { $row = $i; break }
}
[P212]::Send($nav, 0x0197, [IntPtr]$row, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$rc = New-Object P212+RECT; [P212]::GetClientRect($nav, [ref]$rc) | Out-Null
$top = [int][P212]::Send($nav, 0x018E, [IntPtr]::Zero, [IntPtr]::Zero)
$y = 0
for ($k=$top; $k -lt $row; ++$k) { $y += [int][P212]::Send($nav, 0x01A1, [IntPtr]$k, [IntPtr]::Zero) }
$hR = [int][P212]::Send($nav, 0x01A1, [IntPtr]$row, [IntPtr]::Zero)
$pt = New-Object P212+POINT
$pt.X = [int](($rc.Left+$rc.Right)/2); $pt.Y = $rc.Top + $y + [int]($hR/2)
[P212]::ClientToScreen($nav, [ref]$pt) | Out-Null
[P212]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
[P212]::SetCursorPos($pt.X,$pt.Y) | Out-Null; Start-Sleep -Milliseconds 150
[P212]::mouse_event(2,0,0,0,0); [P212]::mouse_event(4,0,0,0,0)
Start-Sleep -Milliseconds 1000
$sb2 = New-Object System.Text.StringBuilder 512
[P212]::SendText($found[1033], 0x000D, [IntPtr]512, $sb2) | Out-Null
$v = $sb2.ToString()
Write-Output ("field1033 = [" + $v + "] len=" + $v.Length)
$bytes = [System.Text.Encoding]::Unicode.GetBytes($v)
Write-Output ("bytes: " + (($bytes | ForEach-Object { $_.ToString('X2') }) -join ' '))
Stop-Process -Id $p.Id -Force
