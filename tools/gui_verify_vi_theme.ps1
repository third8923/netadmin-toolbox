# VI-D2 主题映射验证：paper 默认 / legacy deepspace→ink / 真实点击切换模块
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TV {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
$env:MZ_ELEV_ATTEMPTED = "1"

function Shot-Bg($name, $expectRgb) {
  $p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 900
    $p.Refresh()
    $c = $p.MainWindowHandle
    if ($c -ne [IntPtr]::Zero -and [TV]::GetDlgItem($c, 1001) -ne [IntPtr]::Zero) { $hwnd = $c; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Host "  [FAIL] $name no window"; return $p }
  Start-Sleep -Milliseconds 800
  $r = New-Object TV+RECT
  [TV]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [TV]::PrintWindow($hwnd, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $c1 = $bmp.GetPixel(500, 300)   # 主区画布
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Host ("  {0}: bg=({1},{2},{3}) expect={4} -> {5}" -f $name, $c1.R, $c1.G, $c1.B, $expectRgb, $(if ("$($c1.R),$($c1.G),$($c1.B)" -eq $expectRgb) { "[OK]" } else { "[FAIL]" }))
  return $p
}

# A. 默认（删 ini）→ 纸亮 (251,247,236)
if (Test-Path $ini) { Remove-Item $ini -Force }
$p1 = Shot-Bg "vi-legacy-default.png" "251,247,236"
Get-Process -Id $p1.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

# B. legacy deepspace → 墨暗 (58,58,68)
"[ui]`ntheme=deepspace" | Set-Content -Path $ini -Encoding ASCII
$p2 = Shot-Bg "vi-legacy-deepspace.png" "58,58,68"
$hwnd = $p2.MainWindowHandle
[TV]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 800

# C. 真实鼠标点击侧栏「磁盘信息」→ 模块切换
$nav = [TV]::GetDlgItem($hwnd, 1001)
# 侧栏导航是 owner-draw，无句柄；点侧栏区域 y~660（磁盘信息项）x~120
$r = New-Object TV+RECT
[TV]::GetWindowRect($hwnd, [ref]$r) | Out-Null
[TV]::SetCursorPos($r.Left + 120, $r.Top + 662) | Out-Null
Start-Sleep -Milliseconds 300
[TV]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
Start-Sleep -Milliseconds 60
[TV]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
Start-Sleep -Milliseconds 1200
# 截图确认切换
$r2 = New-Object TV+RECT
[TV]::GetWindowRect($hwnd, [ref]$r2) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r2.Right-$r2.Left), ($r2.Bottom-$r2.Top)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[TV]::PrintWindow($hwnd, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$bmp.Save((Join-Path $shot "vi-click-switch.png"))
$g.Dispose(); $bmp.Dispose()
Write-Host "  saved vi-click-switch.png"
Get-Process -Id $p2.Id -ErrorAction SilentlyContinue | Stop-Process -Force
# 清理：恢复默认主题 ini
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
