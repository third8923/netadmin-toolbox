# Screenshot the tweaks module (most buttons) to verify MDL2 glyphs render
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WT {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
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
  [WT]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object WT+RECT
  [WT]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WT]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [WT]::GetDlgItem($hwnd, 1001)
$list = [WT]::GetDlgItem($hwnd, 1002)
$btnRun = [WT]::GetDlgItem($hwnd, 1010)

# find Windows 优化 (tweak.winopt)
$navCount = [WT]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$target = -1
for ($i = 0; $i -lt $navCount; $i++) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
  try {
    [WT]::SendMessageW($nav, 0x0189, [IntPtr]$i, $buf) | Out-Null
    $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
    if ($t -match "Windows 优化|优化") { $target = $i; Write-Output "nav[$i] = $t"; break }
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
if ($target -ge 0) {
  [WT]::SendMessageW($nav, 0x0186, [IntPtr]$target, [IntPtr]0) | Out-Null
  [WT]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
}
Start-Sleep -Milliseconds 1500
Shot $hwnd "new-12-tweaks.png"

# scan to enable apply/undo buttons
[WT]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 400
  if ([WT]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() -ge 40) { break }
}
$rows = [WT]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "tweaks rows: $rows"
Shot $hwnd "new-13-tweaks-scan.png"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
