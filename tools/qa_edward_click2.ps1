# Edward QA R2b - click theme button, capture the modal picker (foreground window)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class EDQ5 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$exe  = Join-Path $dir "dist\netadmin-toolbox.exe"
$ini  = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force }

function Shot($h, $name) {
  $r = New-Object EDQ5+RECT
  [EDQ5]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  skip $name (bad rect)"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [EDQ5]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process $exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [EDQ5]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NO MAIN WINDOW"; exit 1 }
Start-Sleep -Milliseconds 800
$btn = [EDQ5]::GetDlgItem($hwnd, 1018)
$r = New-Object EDQ5+RECT
[EDQ5]::GetWindowRect($btn, [ref]$r) | Out-Null
$cx = [int](($r.Left + $r.Right) / 2); $cy = [int](($r.Top + $r.Bottom) / 2)
[EDQ5]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 600
[EDQ5]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 250
[EDQ5]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[EDQ5]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1800
$fg = [EDQ5]::GetForegroundWindow()
$fgpid = 0
[EDQ5]::GetWindowThreadProcessId($fg, [ref]$fgpid) | Out-Null
Write-Output ("  foreground hwnd=" + $fg + " pid=" + $fgpid + " (target " + $p.Id + ")")
if ($fgpid -eq $p.Id -and $fg -ne $hwnd) {
  Write-Output "  modal picker window detected, capturing..."
  Shot $fg "qa-ed-r2-picker.png"
} else {
  Write-Output "  no separate modal window; capturing main"
  Shot $hwnd "qa-ed-r2-main-after-click.png"
}
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
if (Test-Path $ini) { Remove-Item $ini -Force }
Write-Output "DONE"
