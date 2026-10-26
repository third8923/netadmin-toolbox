# Capture v2.6 visuals: main window, theme picker, about, confirm, tweaks
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W26 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 900
# ini is removed by the caller (bash) because PowerShell may be sandboxed from deletes

function Shot($h, $name) {
  [W26]::SetForegroundWindow($h) | Out-Null
  [W26]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 500
  # 只截客户区：窗口圆角/阴影/边框外的东西一律不进图
  $rc = New-Object W26+RECT
  [W26]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object W26+POINT
  [W26]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  # PW_RENDERFULLCONTENT：即使被别的窗口遮住也能拿到真实画面
  $hdc = $g.GetHdc()
  $ok = [W26]::PrintWindow($h, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [W26]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name (${w}x${ht})"
}

function Click($h, $id, $wait = 900) {
  $b = [W26]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [W26]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}

function SwitchLast($h, $nav) {
  $cnt = [W26]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  if ($cnt -le 0) { Write-Output "  [WARN] nav empty"; return }
  $idx = $cnt - 1
  [W26]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [W26]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 700
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "no main window"; exit 1 }
$nav = [W26]::GetDlgItem($hwnd, 1001)

# 1) empty state
Shot $hwnd "v26-1-empty.png"

# 2) filled table (disk info)
Click $hwnd 1010 1600
Shot $hwnd "v26-2-main.png"

# 3) theme picker
Click $hwnd 1018 1000
$modal = [W26]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) { Shot $modal "v26-3-theme.png"; [W26]::PostMessageW($modal, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null }
Start-Sleep -Milliseconds 700

# 4) about
Click $hwnd 1015 900
$modal = [W26]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) { Shot $modal "v26-4-about.png"; [W26]::PostMessageW($modal, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null }
Start-Sleep -Milliseconds 700

# 5) tweaks + confirm
SwitchLast $hwnd $nav
Click $hwnd 1010 1500
Shot $hwnd "v26-5-tweaks.png"
Click $hwnd 1011 900
$modal = [W26]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) { Shot $modal "v26-6-confirm.png"; [W26]::PostMessageW($modal, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null }

[W26]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 500
if (-not $p.HasExited) { $p.Kill() }
Write-Output "done"
