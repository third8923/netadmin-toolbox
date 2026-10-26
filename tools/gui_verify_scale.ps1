# Verify UI scaling: Ctrl+= / Ctrl+- / Ctrl+0, window auto-grow, persistence
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WS {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string t);
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
Start-Sleep -Milliseconds 500
if (Test-Path $ini) { Remove-Item $ini -Force -ErrorAction SilentlyContinue }

function Shot($hwnd, $name) {
  [WS]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object WS+RECT
  [WS]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WS]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name  ($($r.Right - $r.Left)x$($r.Bottom - $r.Top))"
}

function PressCtrl([byte]$vk) {
  [WS]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 350
  [WS]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [WS]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 80
  [WS]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
  [WS]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 1100
}

function IniScale {
  if (-not (Test-Path $ini)) { return "no-ini" }
  $txt = Get-Content $ini -Raw
  if ($txt -match 'scale=(\d+)') { return $Matches[1] }
  return "unset"
}

function SwitchModule($nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [WS]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()   # LB_FINDSTRINGEXACT
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav '$title' not found"; return $false }
  [WS]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [WS]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 600
  return $true
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "MAIN WINDOW NOT FOUND"; exit 1 }
Write-Output "[OK] main window $hwnd"
[WS]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 900

$nav = [WS]::GetDlgItem($hwnd, 1001)
SwitchModule $nav "进程列表" | Out-Null
$btnRun = [WS]::GetDlgItem($hwnd, 1010)
[WS]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
Start-Sleep -Seconds 5

Write-Output "start scale: $(IniScale)"
Shot $hwnd "scale-1-100.png"

PressCtrl 0xBB      # VK_OEM_PLUS  (Ctrl + =)
Write-Output "after 1 step: ini scale=$(IniScale)"
Shot $hwnd "scale-2-110.png"

PressCtrl 0xBB
PressCtrl 0xBB
Write-Output "after 3 steps: ini scale=$(IniScale)"
Shot $hwnd "scale-3-135.png"

PressCtrl 0x30      # Ctrl + 0 -> reset
Write-Output "after reset: ini scale=$(IniScale)"
Shot $hwnd "scale-4-reset.png"

Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "DONE"
