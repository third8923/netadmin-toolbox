# VI-D2 复测：Ctrl+T 主题切换 + 墨暗主题截图（ini 确定性启动）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VI2 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200

function Shot($h, $name) {
  if ($h -eq [IntPtr]::Zero) { Write-Output "  [MISS] $name"; return }
  $r = New-Object VI2+RECT
  [VI2]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [VI2]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [VI2]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav '$title' not found"; return }
  [VI2]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [VI2]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 800
}

$env:MZ_ELEV_ATTEMPTED = "1"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"

# ---- A. ini=ink 确定性启动：墨暗主题主界面 + 空态 ----
if (Test-Path $ini) { Remove-Item $ini -Force }
"[ui]`ntheme=ink" | Set-Content -Path $ini -Encoding ASCII
$p = Start-Process $exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [VI2]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
Write-Output "main hwnd=$hwnd"
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
$nav = [VI2]::GetDlgItem($hwnd, 1001)
Write-Output "nav=$nav"
SwitchModule $hwnd $nav "磁盘信息" | Out-Null
Write-Output "after switch: HasExited=$($p.HasExited)"
[VI2]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [VI2]::GetDlgItem($hwnd, 1010)) | Out-Null
Start-Sleep -Seconds 4
Shot $hwnd "vi-main-ink.png"

# 空态：端口扫描（未运行）
SwitchModule $hwnd $nav "端口扫描" | Out-Null
Start-Sleep -Milliseconds 800
Shot $hwnd "vi-empty-ink.png"

# ---- B. Ctrl+T（SendKeys）：墨暗 → 纸亮，验证双主题循环 ----
$ws = New-Object -ComObject WScript.Shell
$null = $ws.AppActivate($p.Id)
Start-Sleep -Milliseconds 600
$ws.SendKeys("^t")
Start-Sleep -Milliseconds 1500
Shot $hwnd "vi-after-ctrlt.png"

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
