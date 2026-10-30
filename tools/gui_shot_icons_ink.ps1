# v2.18 暗主题单脚本：预置 ini → 启动 → 侧栏 4 屏截图
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
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
Set-Content -Path "$dir\dist\netadmin-toolbox-theme.ini" -Value "[ui]`r`ntheme=ink`r`nlang=zh-CN" -Encoding ascii

$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process "$dir\dist\netadmin-toolbox.exe" -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [ICO3]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
[ICO3]::MoveWindow($hwnd, 10, 10, 1500, 1020, $true) | Out-Null
[ICO3]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 1200
$nav = [ICO3]::GetDlgItem($hwnd, 1001)

foreach ($top in 0, 22, 44, 66) {
  [ICO3]::SendMessageW($nav, 0x0197, [IntPtr]$top, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 500
  $bmp = New-Object System.Drawing.Bitmap 1500, 1020
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [ICO3]::PrintWindow($hwnd, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $name = "icons-ink-" + ($top / 22 + 1) + ".png"
  $bmp.Save((Join-Path "$dir\screenshots" $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "saved $name"
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
