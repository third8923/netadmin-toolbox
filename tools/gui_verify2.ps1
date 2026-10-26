# Final verification: tweaks module (undo button) + column sort click + screenshots
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WF {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"

function Shot($hwnd, $name) {
  [WF]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
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
function ListText($list, $idx, $col) {
  $lvfi = New-Object Byte[] 88
  [System.Runtime.InteropServices.Marshal]::WriteInt32($lvfi, 8, $idx)   # iItem offset
  [System.Runtime.InteropServices.Marshal]::WriteInt32($lvfi, 12, $col)  # iSubItem offset
  $ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
  [WF]::SendMessageW($list, 0x104B, $ptr, $lvfi) | Out-Null  # LVM_GETITEMTEXTW w=LVITEM ptr? actually (w=item, l=LVITEM)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  return ""
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [WF]::GetDlgItem($hwnd, 1001)
$list = [WF]::GetDlgItem($hwnd, 1002)

# --- switch to tweak.winopt (last module): count nav items then select last ---
$cnt = [WF]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "nav total: $cnt"
[WF]::SendMessageW($nav, 0x0186, [IntPtr]($cnt - 1), [IntPtr]0) | Out-Null
[WF]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
Start-Sleep -Seconds 1
Shot $hwnd "new-6-tweaks.png"

# --- scan the tweaks (40 items) ---
$btnRun = [WF]::GetDlgItem($hwnd, 1010)
[WF]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
Start-Sleep -Seconds 5
Shot $hwnd "new-7-tweaks-scan.png"
$rowCnt = [WF]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()  # LVM_GETITEMCOUNT
Write-Output "tweaks rows: $rowCnt"

# --- click header column 0 (name) to sort ---
$header = [WF]::SendMessageW($list, 0x101F, [IntPtr]0, [IntPtr]0)  # LVM_GETHEADER
$hr = New-Object WF+RECT
[WF]::GetWindowRect($header, [ref]$hr) | Out-Null
# header item 0 center ~ x+150, y+10 (relative to header client)
[WF]::PostMessageW($header, 0x0201, [IntPtr]1, [IntPtr]((10 -shl 16) -bor 150)) | Out-Null  # WM_LBUTTONDOWN MK_LBUTTON
[WF]::PostMessageW($header, 0x0202, [IntPtr]0, [IntPtr]((10 -shl 16) -bor 150)) | Out-Null  # WM_LBUTTONUP
Start-Sleep -Seconds 1
Shot $hwnd "new-8-sorted.png"

# --- undo button visibility check ---
$btnUndo = [WF]::GetDlgItem($hwnd, 1012)
$vis = [WF]::IsWindowVisible($btnUndo)
Write-Output "undo button visible: $vis"

Stop-Process -Id $p.Id -Force
Write-Output "[OK] done"
