# Verify enlarged fonts + self-drawn LogView scroll clarity
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WF {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetScrollInfo(IntPtr h, int fnBar, ref SCROLLINFO si);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct SCROLLINFO { public int cbSize; public int fMask; public int nMin; public int nMax; public uint nPage; public int nPos; public int nTrackPos; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
if (Test-Path $ini) { Remove-Item $ini -Force -ErrorAction SilentlyContinue }

function Shot($hwnd, $name) {
  [WF]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  [WF]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 450
  $r = New-Object WF+RECT
  [WF]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WF]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}

function ScrollInfo($h) {
  $si = New-Object WF+SCROLLINFO
  $si.cbSize = 28; $si.fMask = 0x17
  [WF]::GetScrollInfo($h, 1, [ref]$si) | Out-Null
  return "$($si.nMin)..$($si.nMax) page=$($si.nPage) pos=$($si.nPos)"
}

function SwitchModule($hwnd, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [WF]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()   # LB_FINDSTRINGEXACT
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  if ($idx -lt 0) { Write-Output "  [WARN] nav item '$title' not found"; return $false }
  [WF]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null    # LB_SETCURSEL
  [WF]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 700
  return $true
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "MAIN WINDOW NOT FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }
Write-Output "[OK] main window: $hwnd"
[WF]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Seconds 1

# 1. enlarged-font main window
Shot $hwnd "font-1-main.png"

$nav = [WF]::GetDlgItem($hwnd, 1001)
$log = [WF]::FindWindowExW($hwnd, [IntPtr]::Zero, "MzLogView", $null)
Write-Output "logview hwnd: $log  scrollinfo: $(if ($log -ne [IntPtr]::Zero) { ScrollInfo $log } else { 'N/A' })"

# 2. generate plenty of log lines: switch modules + run process scan
SwitchModule $hwnd $nav "进程扫描" | Out-Null
SwitchModule $hwnd $nav "磁盘信息" | Out-Null
SwitchModule $hwnd $nav "进程扫描" | Out-Null
$btnRun = [WF]::GetDlgItem($hwnd, 1010)
[WF]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
Start-Sleep -Seconds 6
# extra F5 rescans to build scrollable log volume (simulates a long session)
for ($i = 0; $i -lt 8; $i++) {
  [WF]::PostMessageW($hwnd, 0x0100, [IntPtr]0x74, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 800
}
$si = ScrollInfo $log
Write-Output "after scans scrollinfo: $si"
Shot $hwnd "font-2-log-bottom.png"

# 3. drag thumb to row 6 (simulates user dragging the scrollbar)
$lp = [IntPtr]((6 -shl 16) -bor 5)
[WF]::PostMessageW($log, 0x0115, $lp, [IntPtr]0) | Out-Null   # WM_VSCROLL SB_THUMBTRACK
Start-Sleep -Milliseconds 500
Write-Output "after thumb-drag scrollinfo: $(ScrollInfo $log)"
Shot $hwnd "font-3-scrolled-mid.png"

# 4. jump to top
[WF]::PostMessageW($log, 0x0115, [IntPtr]6, [IntPtr]0) | Out-Null  # SB_TOP
Start-Sleep -Milliseconds 400
Shot $hwnd "font-4-scrolled-top.png"

# 5. theme switch -> LogView must follow theme (dark)
$btnTheme = [WF]::GetDlgItem($hwnd, 1018)
[WF]::PostMessageW($hwnd, 0x0111, [IntPtr]1018, $btnTheme) | Out-Null
Start-Sleep -Milliseconds 900
$modal = [WF]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  $lp = [IntPtr]((140 -shl 16) -bor (599 -band 0xFFFF))   # card 4 = deepspace
  [WF]::PostMessageW($modal, 0x0201, [IntPtr]1, $lp) | Out-Null
  Start-Sleep -Milliseconds 900
}
Shot $hwnd "font-5-logview-dark.png"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
