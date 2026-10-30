# diag2: children presence over time, with theme ini variants
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DG2 {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  public static int Count(IntPtr h) {
    int n = 0;
    EnumChildWindows(h, (c, l) => { n++; return true; }, IntPtr.Zero);
    return n;
  }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Content $ini -ErrorAction SilentlyContinue | ForEach-Object { Write-Output "ini: $_" }
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
for ($i = 1; $i -le 6; $i++) {
  Start-Sleep -Seconds 1
  $p.Refresh()
  $h = $p.MainWindowHandle
  if ($h -ne [IntPtr]::Zero) {
    $nav = [DG2]::GetDlgItem($h, 1001)
    Write-Output "t=${i}s hwnd=$h children=$([DG2]::Count($h)) nav1001=$nav"
  } else {
    Write-Output "t=${i}s no main window yet"
  }
}
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
