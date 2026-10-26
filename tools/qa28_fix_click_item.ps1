# qa28_fix_click_item.ps1 — Round 2 第 3 步：真实点击「更多」菜单项 → 确认框 → Esc
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QB {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
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

# 切到计划任务（sys.tasks，更多菜单含 禁用选中任务 danger 项）
$idx = [QB]::SendMessageW($nav, 0x018F, [IntPtr](-1), "计划任务").ToInt32()
Write-Output "nav idx of 计划任务: $idx"
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

# 打开「更多 ▾」
$btnMore = [QB]::GetDlgItem($hwnd, 1019)
[QB]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, $btnMore) | Out-Null
Start-Sleep -Milliseconds 1500

# 找可见菜单窗口
$script:menuRect = $null
$cb = [QB+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [QB]::GetClassNameW($h, $sb, 256) | Out-Null
  if ($sb.ToString() -eq "#32768") {
    $rr = New-Object QB+RECT
    [QB]::GetWindowRect($h, [ref]$rr) | Out-Null
    if ([QB]::IsWindowVisible($h) -and ($rr.Right - $rr.Left) -gt 10) { $script:menuRect = $rr }
  }
  return $true
}
[QB]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if (-not $script:menuRect) { Write-Output "FAIL: menu not found"; Stop-Process -Id $p.Id -Force; exit 1 }
$mr = $script:menuRect
Write-Output ("menu size={0}x{1}" -f ($mr.Right-$mr.Left), ($mr.Bottom-$mr.Top))

# 真实点击菜单第一项（禁用选中任务，danger → 确认框）
$it = New-Object QB+POINT
$it.X = ($mr.Left + $mr.Right) / 2
$it.Y = $mr.Top + 26          # 菜单边框(~4) + 第一项(34px)中心
[QB]::SetCursorPos($it.X, $it.Y) | Out-Null
Start-Sleep -Milliseconds 150
[QB]::mouse_event(2, $it.X, $it.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 80
[QB]::mouse_event(4, $it.X, $it.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 1200

# 截主窗口（确认框应居中弹出）
$r = New-Object QB+RECT
[QB]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $dir "tools\qa28-fix-menu-click-confirm.png"))
$g.Dispose(); $bmp.Dispose()
Write-Output "confirm-shot=saved"

# Esc 取消确认框
[QB]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[QB]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 700
# 再 Esc 兜底关菜单
[QB]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[QB]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 500
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
