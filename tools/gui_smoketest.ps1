# GUI smoke test v2: launch, switch module, scan, search(ascii), about dialog, screenshots
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$env:MZ_ELEV_ATTEMPTED = "1"   # 沙箱标准用户：跳过启动自动提权（否则卡 UAC 重启流程，主窗口句柄拿不到）
$shot = Join-Path $dir "screenshots"
$HWND_TOPMOST = [IntPtr]-1
$HWND_NOTOPMOST = [IntPtr]-2
$SWP_NOSIZE = 0x0001
$SWP_NOMOVE = 0x0002

function Shot($hwnd, $name) {
  [W]::SetWindowPos($hwnd, $HWND_TOPMOST, 0, 0, 0, 0, $SWP_NOMOVE -bor $SWP_NOSIZE) | Out-Null
  [W]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object W+RECT
  [W]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $path = Join-Path $shot $name
  $bmp.Save($path)
  $g.Dispose(); $bmp.Dispose()
  [W]::SetWindowPos($hwnd, $HWND_NOTOPMOST, 0, 0, 0, 0, $SWP_NOMOVE -bor $SWP_NOSIZE) | Out-Null
  Write-Output "  saved $name"
}
function SetEditText($hwndCtrl, $s) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($s)
  [W]::SendMessageW($hwndCtrl, 0x000C, [IntPtr]0, $ptr) | Out-Null
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {   # 最多等 20s（重建后首次启动可能被杀软扫描拖慢）
  Start-Sleep -Seconds 1
  $p.Refresh()
  $hwnd = $p.MainWindowHandle
  if ($hwnd -ne [IntPtr]::Zero) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "MAIN WINDOW NOT FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }
Write-Output "[OK] main window: $hwnd"
[W]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Seconds 1
Shot $hwnd "new-1-main.png"

# switch to disk info (nav idx 2)
$nav = [W]::GetDlgItem($hwnd, 1001)
[W]::SendMessageW($nav, 0x0186, [IntPtr]2, [IntPtr]0) | Out-Null
[W]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
Start-Sleep -Seconds 1
Shot $hwnd "new-2-disk.png"

# scan
$btnRun = [W]::GetDlgItem($hwnd, 1010)
[W]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
Start-Sleep -Seconds 4
Shot $hwnd "new-3-scan.png"

# search filter with ASCII
$search = [W]::GetDlgItem($hwnd, 1022)
SetEditText $search "net"
Start-Sleep -Seconds 1
Shot $hwnd "new-4-search.png"
$cnt = [W]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "  nav count after filter 'net': $cnt"

# clear search
SetEditText $search ""
Start-Sleep -Seconds 1
$cnt = [W]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "  nav count after clear: $cnt"

# about dialog
$btnAbout = [W]::GetDlgItem($hwnd, 1015)
[W]::PostMessageW($hwnd, 0x0111, [IntPtr]1015, $btnAbout) | Out-Null
Start-Sleep -Seconds 2
$modal = [W]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Write-Output "[OK] about dialog: $modal"
  Shot $modal "new-5-about.png"
  [W]::PostMessageW($modal, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null  # Esc
  Start-Sleep -Seconds 1
} else {
  Write-Output "[WARN] about dialog not found"
}

Stop-Process -Id $p.Id -Force
Start-Sleep -Seconds 1
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Write-Error "process still running" } else { Write-Output "[OK] app exited cleanly" }
