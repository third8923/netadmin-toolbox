# v2.18 侧栏矢量图标验收截图：亮/暗主题 × 侧栏滚动 4 屏
# 依赖：UTF-8 BOM；MZ_ELEV_ATTEMPTED=1；PrintWindow(hwnd,hdc,3)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ICO {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force -ErrorAction SilentlyContinue }

function Shot($h, $name) {
  if ($h -eq [IntPtr]::Zero) { Write-Output "  [MISS] $name"; return }
  $bmp = New-Object System.Drawing.Bitmap 1500, 1020
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [ICO]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name"
}

$env:MZ_ELEV_ATTEMPTED = "1"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [ICO]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
[ICO]::MoveWindow($hwnd, 10, 10, 1500, 1020, $true) | Out-Null
[ICO]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 1200
$nav = [ICO]::GetDlgItem($hwnd, 1001)

# ---- 亮主题：侧栏 4 屏 ----
foreach ($top in 0, 22, 44, 66) {
  [ICO]::SendMessageW($nav, 0x0197, [IntPtr]$top, [IntPtr]0) | Out-Null   # LB_SETTOPINDEX
  Start-Sleep -Milliseconds 500
  Shot $hwnd ("icons-paper-" + ($top / 22 + 1) + ".png")
}

# ---- Ctrl+T → 墨暗主题：侧栏 4 屏 ----
[ICO]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
[ICO]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[ICO]::keybd_event(0x54, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[ICO]::keybd_event(0x54, 0, 2, [UIntPtr]::Zero)
[ICO]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1300
foreach ($top in 0, 22, 44, 66) {
  [ICO]::SendMessageW($nav, 0x0197, [IntPtr]$top, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 500
  Shot $hwnd ("icons-ink-" + ($top / 22 + 1) + ".png")
}

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
