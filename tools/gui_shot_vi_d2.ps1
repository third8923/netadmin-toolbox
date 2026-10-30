# VI-D2 验收截图：splash / 主界面双主题 / 主题选择 / 关于 / 空态 / 日志
# 依赖：UTF-8 BOM；MZ_ELEV_ATTEMPTED=1；PrintWindow(hwnd,hdc,3)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class VI {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force -ErrorAction SilentlyContinue }

function Shot($h, $name) {
  if ($h -eq [IntPtr]::Zero) { Write-Output "  [MISS] $name (no hwnd)"; return }
  $r = New-Object VI+RECT
  [VI]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [MISS] $name (empty rect)"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [VI]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_RENDERFULLCONTENT
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [VI]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav '$title' not found"; return $false }
  [VI]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [VI]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 800
  return $true
}

function CtrlKey($vk) {
  [VI]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [VI]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [VI]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
  [VI]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
}

$env:MZ_ELEV_ATTEMPTED = "1"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"

# ---- 1. splash（启动后 ~700ms 内截取）----
$p = Start-Process $exe -PassThru
Start-Sleep -Milliseconds 700
$sp = [VI]::FindWindowW("MzSplashWnd", [IntPtr]::Zero)
Shot $sp "vi-splash.png"

# ---- 2. 等主窗口（轮询直到 nav 子控件就绪，避开 splash 句柄竞态）----
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [VI]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO MAIN WINDOW"; exit 1 }
[VI]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 900
$nav = [VI]::GetDlgItem($hwnd, 1001)

# ---- 3. 磁盘信息：扫描 → 数据行 + 日志（纸亮主题）----
SwitchModule $hwnd $nav "磁盘信息" | Out-Null
[VI]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, [VI]::GetDlgItem($hwnd, 1010)) | Out-Null
Start-Sleep -Seconds 4
Shot $hwnd "vi-main-paper.png"

# ---- 4. 空态（未扫描模块）----
SwitchModule $hwnd $nav "系统信息" | Out-Null
Start-Sleep -Milliseconds 600
Shot $hwnd "vi-empty-paper.png"

# ---- 5. Ctrl+T → 墨暗主题 ----
[VI]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
CtrlKey 0x54
Start-Sleep -Milliseconds 1300
Shot $hwnd "vi-main-ink.png"

# ---- 6. 主题选择（再按 Ctrl+T 回纸亮后打开选择页，或直接开）----
[VI]::PostMessageW($hwnd, 0x0111, [IntPtr]1018, [VI]::GetDlgItem($hwnd, 1018)) | Out-Null
Start-Sleep -Milliseconds 900
$m = [VI]::FindWindowW("MzModalHost", [IntPtr]::Zero)
Shot $m "vi-themepicker.png"
if ($m -ne [IntPtr]::Zero) {
  [VI]::PostMessageW($m, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 700
}

# ---- 7. 关于页 ----
[VI]::PostMessageW($hwnd, 0x0111, [IntPtr]1015, [VI]::GetDlgItem($hwnd, 1015)) | Out-Null
Start-Sleep -Milliseconds 900
$m2 = [VI]::FindWindowW("MzModalHost", [IntPtr]::Zero)
Shot $m2 "vi-about.png"
if ($m2 -ne [IntPtr]::Zero) {
  [VI]::PostMessageW($m2, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 700
}

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
