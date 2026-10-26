# v2.10 sharehost scan + More-menu screenshots
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $dir 'dist\netadmin-toolbox.exe'
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class T210C {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageWStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static Dictionary<int, IntPtr> Kids = new Dictionary<int, IntPtr>();
  public static void Collect(IntPtr parent) {
    Kids.Clear();
    EnumChildWindows(parent, (h, l) => { Kids[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
  }
}
"@
$WM_COMMAND=0x0111; $LB_GETCOUNT=0x018B; $LB_GETTEXT=0x0189; $LB_GETITEMRECT=0x0198
$LB_SETTOPINDEX=0x0197; $WM_LBUTTONDOWN=0x0201; $WM_LBUTTONUP=0x0202; $WM_CLOSE=0x0010
$env:MZ_ELEV_ATTEMPTED='1'
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir 'dist') -PassThru
for ($i=0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break } }
$main = $p.MainWindowHandle
[T210C]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 1500
[T210C]::Collect($main)
$nav=[T210C]::Kids[1001]; $search=[T210C]::Kids[1022]

function ShotMain($path) {
  $r = New-Object T210C+RECT
  [T210C]::GetWindowRect($main, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R-$r.L), ($r.B-$r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
  [T210C]::PrintWindow($main, $dc, 3) | Out-Null
  $g.ReleaseHdc($dc); $g.Dispose(); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Write-Host "saved $path"
}

# switch to sharehost
[T210C]::SendMessageWStr($search, 0x000C, [IntPtr]::Zero, 'sharehost') | Out-Null
Start-Sleep -Milliseconds 900
$ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
$row = -1
$cnt = [int][T210C]::SendMessageW($nav, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
for ($i=0; $i -lt $cnt; $i++) {
  [T210C]::SendMessageW($nav, $LB_GETTEXT, [IntPtr]$i, $ptr) | Out-Null
  if ([System.Runtime.InteropServices.Marshal]::PtrToStringUni($ptr) -eq '共享部署') { $row = $i; break }
}
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
$ptr2 = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[T210C]::SendMessageW($nav, $LB_GETITEMRECT, [IntPtr]$row, $ptr2) | Out-Null
$rc = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptr2, [type][T210C+RECT])
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr2)
$x=[int](($rc.L+$rc.R)/2); $y=[int](($rc.T+$rc.B)/2)
$lp=[IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
[T210C]::SendMessageW($nav, $WM_LBUTTONDOWN, [IntPtr]1, $lp) | Out-Null
[T210C]::SendMessageW($nav, $WM_LBUTTONUP, [IntPtr]0, $lp) | Out-Null
Start-Sleep -Milliseconds 800
[T210C]::SendMessageWStr($search, 0x000C, [IntPtr]::Zero, '') | Out-Null
Start-Sleep -Milliseconds 800

# run scan (执行 = ID_BTN_RUN 1010)
[T210C]::PostMessageW($main, $WM_COMMAND, [IntPtr]((0 -shl 16) -bor 1010), [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 4000
ShotMain (Join-Path $dir 'tools\t210-3-sharehost-scan.png')

# open More menu (ID_BTN_MORE 1019), capture popup by CopyFromScreen
[T210C]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 400
[T210C]::PostMessageW($main, $WM_COMMAND, [IntPtr]((0 -shl 16) -bor 1019), [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 700
$menu = [T210C]::FindWindowW('#32768', [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object T210C+RECT
  [T210C]::GetWindowRect($menu, [ref]$mr) | Out-Null
  $w = $mr.R-$mr.L; $h = $mr.B-$mr.T
  Write-Host "menu size: ${w}x${h}"
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($mr.L, $mr.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
  $g.Dispose()
  $bmp.Save((Join-Path $dir 'tools\t210-4-more-menu.png'), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Write-Host 'saved t210-4-more-menu.png'
  [T210C]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero)   # Esc
  [T210C]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
} else { Write-Host 'menu NOT found' }
Start-Sleep -Milliseconds 600
[T210C]::PostMessageW($main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1200
$p.Refresh(); Write-Host "exited=$($p.HasExited)"
