# qa29_about.ps1 — 关于框版本核对
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QAAB {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
function Shot([IntPtr]$h, [string]$name) {
  [QAAB]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QAAB+RECT
  [QAAB]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object QAAB+POINT
  [QAAB]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [QAAB]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  [QAAB]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
[QAAB]::PostMessageW($hwnd, 0x0111, [IntPtr]1015, [QAAB]::GetDlgItem($hwnd, 1015)) | Out-Null
Start-Sleep -Milliseconds 1200
$modal = [QAAB]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) { Shot $modal "qa29-24-about.png" } else { Write-Output "  [WARN] about modal not found" }
if ($modal -ne [IntPtr]::Zero) {
  [QAAB]::SetForegroundWindow($modal) | Out-Null
  [QAAB]::PostMessageW($modal, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 800
}
[QAAB]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Output "done"
