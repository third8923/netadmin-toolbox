# VI-D2 复测：空态/错误态（修复 span 裁剪后）+ 纸亮主题主界面
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VI3 {
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
  if ($h -eq [IntPtr]::Zero) { Write-Output "  [MISS] $name"; return }
  $r = New-Object VI3+RECT
  [VI3]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [MISS] $name (rect)"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [VI3]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [VI3]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav '$title' not found"; return }
  [VI3]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [VI3]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 900
}

$env:MZ_ELEV_ATTEMPTED = "1"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [VI3]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
$nav = [VI3]::GetDlgItem($hwnd, 1001)

# ---- 1. 空态（纸亮）：端口扫描 未运行 ----
SwitchModule $hwnd $nav "端口扫描" | Out-Null
Start-Sleep -Milliseconds 700
Shot $hwnd "vi-empty-paper.png"

# ---- 2. 错误态（纸亮）：端口扫描 目标非法 → 扫描失败、列表空 ----
$target = [VI3]::GetDlgItem($hwnd, 1031)   # ID_TARGET
if ($target -eq [IntPtr]::Zero) { $target = [VI3]::GetDlgItem($hwnd, 1003) }
[VI3]::SendMessageW($target, 0x000C, [IntPtr]0, [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("999.999.999.999")) | Out-Null
Start-Sleep -Milliseconds 400
[VI3]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [VI3]::GetDlgItem($hwnd, 1010)) | Out-Null
Start-Sleep -Seconds 5
Shot $hwnd "vi-error-paper.png"

# ---- 3. 主界面（纸亮，含数据行）重新截取 ----
SwitchModule $hwnd $nav "磁盘信息" | Out-Null
Start-Sleep -Milliseconds 600
[VI3]::SendMessageW([VI3]::GetDlgItem($hwnd, 1031), 0x000C, [IntPtr]0, [IntPtr]0) | Out-Null
[VI3]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [VI3]::GetDlgItem($hwnd, 1010)) | Out-Null
Start-Sleep -Seconds 4
Shot $hwnd "vi-main-paper.png"

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
