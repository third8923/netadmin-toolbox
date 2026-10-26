# t03_menu_click.ps1 — 补拍：更多菜单鼠标点击「停用 Server 服务」→ 确认框 → 非提权拒绝日志
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T03M {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
function Shot([IntPtr]$h, [string]$name) {
  [T03M]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object T03M+RECT
  [T03M]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object T03M+POINT
  [T03M]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [T03M]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  [T03M]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function Click([int]$x, [int]$y) {
  [T03M]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T03M]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [T03M]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 400
}
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T03M]::GetDlgItem($hwnd, 1001)
# 定位共享部署（展开网络管理组头）
$cnt = [T03M]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$iShare = -1
for ($i = 0; $i -lt $cnt; $i++) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  $len = [T03M]::SendMessageW($nav, 0x0189, [IntPtr]$i, $mem).ToInt32()
  $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($mem)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
  if ($t -eq "共享部署") { $iShare = $i; break }
}
# 若网络管理组收起，先点组头
if ($iShare -lt 0) {
  $iG = -1
  for ($i = 0; $i -lt $cnt; $i++) {
    $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
    [void][T03M]::SendMessageW($nav, 0x0189, [IntPtr]$i, $mem)
    $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($mem)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
    if ($t.StartsWith("网络管理")) { $iG = $i; break }
  }
  [T03M]::SendMessageW($nav, 0x0197, [IntPtr]$iG, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  # 用 item rect 点击
  $mem2 = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [void][T03M]::SendMessageW($nav, 0x0198, [IntPtr]$iG, $mem2)
  $b = New-Object byte[] 16
  [System.Runtime.InteropServices.Marshal]::Copy($mem2, $b, 0, 16)
  $pt = New-Object T03M+POINT
  $pt.X = [BitConverter]::ToInt32($b, 0) + 40
  $pt.Y = [BitConverter]::ToInt32($b, 4) + ([BitConverter]::ToInt32($b, 12) - [BitConverter]::ToInt32($b, 4)) / 2
  [T03M]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T03M]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 200
  Click $pt.X $pt.Y
  Start-Sleep -Milliseconds 600
  $cnt = [T03M]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  for ($i = 0; $i -lt $cnt; $i++) {
    $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
    [void][T03M]::SendMessageW($nav, 0x0189, [IntPtr]$i, $mem)
    $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($mem)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
    if ($t -eq "共享部署") { $iShare = $i; break }
  }
}
# 点击共享部署行
$r = New-Object T03M+RECT
$mem2 = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[void][T03M]::SendMessageW($nav, 0x0198, [IntPtr]$iShare, $mem2)
$b = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem2, $b, 0, 16)
$pt = New-Object T03M+POINT
$pt.X = [BitConverter]::ToInt32($b, 0) + 40
$pt.Y = [BitConverter]::ToInt32($b, 4) + ([BitConverter]::ToInt32($b, 12) - [BitConverter]::ToInt32($b, 4)) / 2
[T03M]::ClientToScreen($nav, [ref]$pt) | Out-Null
[T03M]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 200
Click $pt.X $pt.Y
Start-Sleep -Milliseconds 1200
# 打开更多菜单
$btn = [T03M]::GetDlgItem($hwnd, 1019)
[T03M]::PostMessageW($hwnd, 0x0111, [IntPtr]1019, $btn) | Out-Null
Start-Sleep -Milliseconds 1200
$menu = [T03M]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -eq [IntPtr]::Zero) { Write-Output "[FAIL] menu not open"; Stop-Process -Id $p.Id -Force; exit 1 }
$mr = New-Object T03M+RECT
[T03M]::GetWindowRect($menu, [ref]$mr) | Out-Null
$mw = $mr.Right - $mr.Left; $mh = $mr.Bottom - $mr.Top
$itemH = [int]($mh / 10)
# 第 3 项（索引 2）= 停用 Server 服务；条目垂直中心 ≈ top + itemH*2.5
$mx = $mr.Left + [int]($mw / 2)
$my = $mr.Top + 15
Click $mx $my
Start-Sleep -Milliseconds 1500
$modal = [T03M]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t03-5-stopserver-confirm.png"
  # Enter 确认 → 非提权拒绝（keybd_event 物理 Enter）
  [T03M]::SetForegroundWindow($modal) | Out-Null
  Start-Sleep -Milliseconds 300
  [T03M]::keybd_event(0x0D, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60
  [T03M]::keybd_event(0x0D, 0, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 1500
  $modal2 = [T03M]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal2 -eq [IntPtr]::Zero) {
    # 日志区滚动到底（在日志区域滚轮）
    [T03M]::SetCursorPos(640, 580) | Out-Null; Start-Sleep -Milliseconds 200
    [T03M]::mouse_event(0x0800, 640, 580, [uint32]::MaxValue - 5, 0) | Out-Null
    Start-Sleep -Milliseconds 800
    Shot $hwnd "t03-6-deny-log.png"
    Write-Output "  deny-log captured"
  } else { Write-Output "  [WARN] modal still open after Enter" }
} else {
  Write-Output "  [WARN] no modal after clicking menu item (item index guess off)"
  Shot $hwnd "t03-5-miss.png"
}
Start-Sleep -Milliseconds 500
[T03M]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Output "done"
