# t03 补充：组头收起 + ini 折叠记忆（按实际 item rect 点击）
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T3 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int GetItemCount(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct LVITEM { public uint mask; public int iItem; public int iSubItem; public uint state; public uint stateMask; public IntPtr pszText; public int cchTextMax; public int iImage; public IntPtr lParam; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
Remove-Item $ini -Force -ErrorAction SilentlyContinue
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $c = $p.MainWindowHandle
  if ($c -ne [IntPtr]::Zero -and [T3]::GetDlgItem($c, 1001) -ne [IntPtr]::Zero) { $hwnd = $c; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO WINDOW"; exit 1 }
$nav = [T3]::GetDlgItem($hwnd, 1001)
[T3]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 800
$count = [T3]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()   # LVM_GETITEMCOUNT
Write-Output "nav rows initial: $count"
# 用 LB_GETITEMRECT 取第 1 行（组头）真实位置后点击其中心
$ir = New-Object T3+RECT
$ptrI = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[System.Runtime.InteropServices.Marshal]::StructureToPtr($ir, $ptrI, $false)
[T3]::SendMessageW($nav, 0x0198, [IntPtr]0, $ptrI) | Out-Null   # LB_GETITEMRECT
$ir = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptrI, [type][T3+RECT])
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptrI)
$nr = New-Object T3+RECT
[T3]::GetWindowRect($nav, [ref]$nr) | Out-Null
$cx = $nr.Left + (($ir.Right - $ir.Left) / 2)
$cy = $nr.Top + $ir.Top + (($ir.Bottom - $ir.Top) / 2)
Write-Host ("item0 rect in client: {0},{1} -> screen {2},{3}" -f $ir.Left, $ir.Top, $cx, $cy)
[T3]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 250
[T3]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [T3]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 1200
$count2 = [T3]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "nav rows after collapse click: $count2"
Start-Sleep -Milliseconds 600
Write-Output "--- ini after collapse ---"
Get-Content $ini -ErrorAction SilentlyContinue
# 重启验证折叠保持
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
$p2 = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
$hwnd2 = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p2.Refresh()
  $c = $p2.MainWindowHandle
  if ($c -ne [IntPtr]::Zero -and [T3]::GetDlgItem($c, 1001) -ne [IntPtr]::Zero) { $hwnd2 = $c; break }
}
$nav2 = [T3]::GetDlgItem($hwnd2, 1001)
Start-Sleep -Milliseconds 800
$count3 = [T3]::SendMessageW($nav2, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "nav rows after restart: $count3 (expect $count2 if persisted)"
Get-Process -Id $p2.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
