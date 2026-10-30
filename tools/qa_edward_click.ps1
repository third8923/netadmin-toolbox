# Edward QA R2 - real mouse click on theme button (ASCII-only)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class EDQ4 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
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
  $r = New-Object EDQ4+RECT
  [EDQ4]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [EDQ4]::PrintWindow($h, $hdc, 3) | Out-Null
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
  if ($cand -ne [IntPtr]::Zero -and [EDQ4]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NO MAIN WINDOW"; exit 1 }
Start-Sleep -Milliseconds 800
Shot $hwnd "qa-ed-r2-before-click.png"

# real mouse click on theme button (ID 1018)
$btn = [EDQ4]::GetDlgItem($hwnd, 1018)
if ($btn -eq [IntPtr]::Zero) { Write-Output "THEME BUTTON HWND NOT FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }
$r = New-Object EDQ4+RECT
[EDQ4]::GetWindowRect($btn, [ref]$r) | Out-Null
$cx = [int](($r.Left + $r.Right) / 2); $cy = [int](($r.Top + $r.Bottom) / 2)
[EDQ4]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 600
[EDQ4]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 200
[EDQ4]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down
Start-Sleep -Milliseconds 80
[EDQ4]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # left up
Start-Sleep -Milliseconds 1500
Shot $hwnd "qa-ed-r2-after-click.png"
Write-Output "  ini after click:"
if (Test-Path $ini) { Get-Content $ini | ForEach-Object { Write-Output "    | $_" } } else { Write-Output "    (ini missing)" }

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
if (Test-Path $ini) { Remove-Item $ini -Force }
Write-Output "DONE"
