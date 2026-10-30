# VI-D2 错误态复测：目标 nonexistent-host-4f2a.invalid（不可路由，扫描超时失败）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VI4 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force }

function Shot($h, $name) {
  $r = New-Object VI4+RECT
  [VI4]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [VI4]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}
function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [VI4]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  [VI4]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [VI4]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 900
}

$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [VI4]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
$nav = [VI4]::GetDlgItem($hwnd, 1001)
SwitchModule $hwnd $nav "端口扫描" | Out-Null
$target = [VI4]::GetDlgItem($hwnd, 1021)   # ID_TARGET
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("nonexistent-host-4f2a.invalid")
[VI4]::SendMessageW($target, 0x000C, [IntPtr]0, $ptr) | Out-Null
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 400
[VI4]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [VI4]::GetDlgItem($hwnd, 1010)) | Out-Null
Start-Sleep -Seconds 8
Shot $hwnd "vi-error-paper.png"
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
