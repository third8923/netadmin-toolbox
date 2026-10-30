# v2.18 i18n 验收截图：en-US 界面 4 屏（paper 主题）+ 回归 zh-CN 1 屏
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ICO3 {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"

function Shot($h, $name) {
  if ($h -eq [IntPtr]::Zero) { Write-Output "  [MISS] $name"; return }
  $bmp = New-Object System.Drawing.Bitmap 1500, 1020
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [ICO3]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name"
}

function Launch($lang) {
  Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 1200
  Set-Content -Path $ini -Value "[ui]`r`ntheme=paper`r`nlang=$lang" -Encoding ascii
  $env:MZ_ELEV_ATTEMPTED = "1"
  $p = Start-Process $exe -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 900
    $p.Refresh()
    $cand = $p.MainWindowHandle
    if ($cand -ne [IntPtr]::Zero -and [ICO3]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW ($lang)"; exit 1 }
  [ICO3]::MoveWindow($hwnd, 10, 10, 1500, 1020, $true) | Out-Null
  [ICO3]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 1200
  return $hwnd
}

# ---- en-US：4 屏侧栏 ----
$h = Launch "en-US"
$nav = [ICO3]::GetDlgItem($h, 1001)
foreach ($top in 0, 22, 44, 66) {
  [ICO3]::SendMessageW($nav, 0x0197, [IntPtr]$top, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 500
  Shot $h ("i18n-en-" + ($top / 22 + 1) + ".png")
}

# ---- zh-CN 回归：1 屏 ----
$h2 = Launch "zh-CN"
$nav2 = [ICO3]::GetDlgItem($h2, 1001)
[ICO3]::SendMessageW($nav2, 0x0197, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 500
Shot $h2 "i18n-zh-1.png"

Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
