# qa28_zoom.ps1 — 缩放终验（净 ini：Ctrl+= →110 →截图 →Ctrl+0 →复位 →截图）
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class QZ {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
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
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$env:MZ_ELEV_ATTEMPTED = "1"
function Shot([IntPtr]$h, [string]$name) {
  [QZ]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object QZ+RECT
  [QZ]::GetClientRect($h, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $rc.Right, $rc.Bottom
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); $ok = [QZ]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc)
  if (-not $ok) { $pt = New-Object QZ+POINT; [QZ]::ClientToScreen($h, [ref]$pt) | Out-Null; $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name")); $g.Dispose(); $bmp.Dispose()
  [QZ]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}
function IniScale {
  if (-not (Test-Path $ini)) { return "no-ini" }
  if ((Get-Content $ini -Raw) -match 'scale=(\d+)') { return $Matches[1] }
  return "unset"
}
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
Write-Output "scale fresh: $(IniScale)"
[QZ]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 600
[QZ]::keybd_event(0x11,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[QZ]::keybd_event(0xBB,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[QZ]::keybd_event(0xBB,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[QZ]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 1000
Write-Output "scale after Ctrl+=: $(IniScale)"
if ((IniScale) -eq "110") {
  Shot $hwnd "qa28-10-zoom-in.png"
  [QZ]::keybd_event(0x11,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
  [QZ]::keybd_event(0x30,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
  [QZ]::keybd_event(0x30,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
  [QZ]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 1000
  Write-Output "scale after Ctrl+0: $(IniScale)"
  Shot $hwnd "qa28-15-zoom-reset.png"
} else { Write-Output "[FAIL] zoom step not 110" }
[QZ]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
Write-Output "exit clean: $($p.HasExited)"
