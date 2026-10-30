# 三语言检查：zh-TW / en-US 下新 UI 文案
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class LG {
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
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
$env:MZ_ELEV_ATTEMPTED = "1"

foreach ($lang in @("en-US", "zh-TW")) {
  "[ui]`nlang=$lang" | Set-Content -Path $ini -Encoding ASCII
  $p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 900
    $p.Refresh()
    $c = $p.MainWindowHandle
    if ($c -ne [IntPtr]::Zero -and [LG]::GetDlgItem($c, 1001) -ne [IntPtr]::Zero) { $hwnd = $c; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Host "  [FAIL] $lang no window"; continue }
  Start-Sleep -Milliseconds 800
  $r = New-Object LG+RECT
  [LG]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right-$r.Left), ($r.Bottom-$r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [LG]::PrintWindow($hwnd, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot "vi-lang-$lang.png"))
  $g.Dispose(); $bmp.Dispose()
  Write-Host "  saved vi-lang-$lang.png"
  # 主题选择页（新文案：纸亮/墨暗 名称与描述）
  [LG]::PostMessageW($hwnd, 0x0111, [IntPtr]1018, [LG]::GetDlgItem($hwnd, 1018)) | Out-Null
  Start-Sleep -Milliseconds 900
  $r2 = New-Object LG+RECT
  [LG]::GetWindowRect($hwnd, [ref]$r2) | Out-Null
  $bmp2 = New-Object System.Drawing.Bitmap ($r2.Right-$r2.Left), ($r2.Bottom-$r2.Top)
  $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
  $hdc2 = $g2.GetHdc()
  [LG]::PrintWindow($hwnd, $hdc2, 3) | Out-Null
  $g2.ReleaseHdc($hdc2)
  $bmp2.Save((Join-Path $shot "vi-lang-$lang-theme.png"))
  $g2.Dispose(); $bmp2.Dispose()
  Write-Host "  saved vi-lang-$lang-theme.png"
  Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 800
}
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Host "DONE"
