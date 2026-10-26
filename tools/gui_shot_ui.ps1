# Capture the revamped visuals: main data card, about, confirm, and 120% scale
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WV {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
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
  [WV]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $r = New-Object WV+RECT
  [WV]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WV]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name (${w}x${ht})"
}

function Click($h, $id) {
  $b = [WV]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [WV]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds 900
}

function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [WV]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav '$title' not found"; return $false }
  [WV]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [WV]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 700
  return $true
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO WINDOW"; exit 1 }
[WV]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 900
$nav = [WV]::GetDlgItem($hwnd, 1001)

# 1. data view with rows
SwitchModule $hwnd $nav "磁盘信息" | Out-Null
Click $hwnd 1010
Start-Sleep -Seconds 3
Shot $hwnd "ui-1-main.png"

# 2. about dialog
Click $hwnd 1015
$m = [WV]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($m -ne [IntPtr]::Zero) {
  Shot $m "ui-2-about.png"
  [WV]::PostMessageW($m, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 800
} else { Write-Output "  [WARN] about not found" }

# 3. confirm dialog (Windows 优化 -> 全选 -> 应用选中)
SwitchModule $hwnd $nav "Windows 优化" | Out-Null
Click $hwnd 1010
Start-Sleep -Seconds 3
Click $hwnd 1016          # 全选
Click $hwnd 1011          # 应用选中
$m2 = [WV]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($m2 -ne [IntPtr]::Zero) {
  Shot $m2 "ui-3-confirm.png"
  [WV]::PostMessageW($m2, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 800
} else { Write-Output "  [WARN] confirm not found" }

# 4. scale up twice -> 120%
[WV]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500
for ($i = 0; $i -lt 2; $i++) {
  [WV]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [WV]::keybd_event(0xBB, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [WV]::keybd_event(0xBB, 0, 2, [UIntPtr]::Zero)
  [WV]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 1100
}
Shot $hwnd "ui-4-scale120.png"

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
