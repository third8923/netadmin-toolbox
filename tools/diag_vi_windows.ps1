# diag: enumerate top-level windows of netadmin-toolbox process
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class DG {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static List<string> Found = new List<string>();
  public static void Enum(uint pid) {
    Found.Clear();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var c = new StringBuilder(256); GetClassNameW(h, c, 256);
        var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
        RECT r; GetWindowRect(h, out r);
        Found.Add(string.Format("hwnd=0x{0:X} vis={1} class={2} title='{3}' rect={4},{5},{6},{7}",
          h.ToInt64(), IsWindowVisible(h), c, t, r.Left, r.Top, r.Right, r.Bottom));
      }
      return true;
    }, IntPtr.Zero);
    foreach (var s in Found) Console.WriteLine(s);
  }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
Write-Output "HasExited=$($p.HasExited) MWH=$($p.MainWindowHandle)"
[DG]::Enum([uint32]$p.Id)
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
