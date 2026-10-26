# qa29_r2_gui.ps1 — Round 2 产物 GUI 启动冒烟（页脚 v2.9）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QAR2 {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
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
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "[FAIL] no window"; exit 1 }
[QAR2]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
Start-Sleep -Milliseconds 450
$rc = New-Object QAR2+RECT
[QAR2]::GetClientRect($h, [ref]$rc) | Out-Null
$w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [QAR2]::PrintWindow($h, $hdc, 3)
$g.ReleaseHdc($hdc)
if (-not $ok) { $pt = New-Object QAR2+POINT; [QAR2]::ClientToScreen($h, [ref]$pt) | Out-Null; $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
$bmp.Save((Join-Path $dir "tools\qa29-25-r2-home.png"))
$g.Dispose(); $bmp.Dispose()
[QAR2]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
Write-Output "  saved qa29-25-r2-home.png"
[QAR2]::PostMessageW($h, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  WM_CLOSE OK" } else { Stop-Process -Id $p.Id -Force; Write-Output "  forced kill" }
