# qa28_fix_cleanbasic.ps1 — Round 2 第 4 步：clean.basic（无扩展动作）按钮行抽查
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QB {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
$nav = [QB]::GetDlgItem($hwnd, 1001)

$idx = [QB]::SendMessageW($nav, 0x018F, [IntPtr](-1), "基础清理").ToInt32()
Write-Output "nav idx of 基础清理: $idx"
[QB]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 300
$mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[QB]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem) | Out-Null
$b = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
$pt = New-Object QB+POINT
$pt.X = [int](([BitConverter]::ToInt32($b,0)) + ([BitConverter]::ToInt32($b,8))) / 2
$pt.Y = [int](([BitConverter]::ToInt32($b,4)) + ([BitConverter]::ToInt32($b,12))) / 2
[QB]::ClientToScreen($nav, [ref]$pt) | Out-Null
[QB]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
[QB]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 150
[QB]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 80
[QB]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 900

# 「更多」按钮（ID 1019）在无扩展动作模块上应不可见
$btnMore = [QB]::GetDlgItem($hwnd, 1019)
$moreVis = [QB]::IsWindowVisible($btnMore)
Write-Output ("btnMore(1019) visible = {0}（期望 False）" -f $moreVis)

$r = New-Object QB+RECT
[QB]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.Right-$r.Left), ($r.Bottom-$r.Top)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $dir "tools\qa28-fix-cleanbasic-buttons.png"))
$g.Dispose(); $bmp.Dispose()
Write-Output "shot=saved"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
