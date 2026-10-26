# v2.10 visual walkthrough: main page + sharehost page screenshots
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
public class T210B {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageWStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static Dictionary<int, IntPtr> Kids = new Dictionary<int, IntPtr>();
  public static void Collect(IntPtr parent) {
    Kids.Clear();
    EnumChildWindows(parent, (h, l) => { Kids[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
  }
}
"@

$WM_COMMAND = 0x0111; $LB_GETCOUNT = 0x018B; $LB_GETTEXT = 0x0189
$LB_GETITEMRECT = 0x0198; $LB_SETTOPINDEX = 0x0197
$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202; $WM_CLOSE = 0x0010

$env:MZ_ELEV_ATTEMPTED = '1'
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir 'dist') -PassThru
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 500; $p.Refresh()
  if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
}
$main = $p.MainWindowHandle
[T210B]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 1800

function Shot($path) {
  $r = New-Object T210B+RECT
  [T210B]::GetWindowRect($main, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $h = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  [T210B]::PrintWindow($main, $dc, 3) | Out-Null   # PW_CLIENTONLY|PW_RENDERFULLCONTENT
  $g.ReleaseHdc($dc); $g.Dispose()
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Write-Host "saved $path"
}

[T210B]::Collect($main)
$nav = [T210B]::Kids[1001]; $search = [T210B]::Kids[1022]
# main page (sharehost not selected): shot with expand-all visual
Shot (Join-Path $dir 'tools\t210-1-main.png')

# search for sharehost and switch to it
[T210B]::SendMessageWStr($search, 0x000C, [IntPtr]::Zero, 'sharehost') | Out-Null  # WM_SETTEXT
Start-Sleep -Milliseconds 900
$cnt = [int][T210B]::SendMessageW($nav, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$row = -1
$ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
for ($i = 0; $i -lt $cnt; $i++) {
  [T210B]::SendMessageW($nav, $LB_GETTEXT, [IntPtr]$i, $ptr) | Out-Null
  $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($ptr)
  if ($t -eq '共享部署') { $row = $i; break }
}
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Write-Host "search rows=$cnt hit=$row"
if ($row -ge 0) {
  [T210B]::SendMessageW($nav, $LB_SETTOPINDEX, [IntPtr]$row, [IntPtr]::Zero) | Out-Null
  $ptr2 = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [T210B]::SendMessageW($nav, $LB_GETITEMRECT, [IntPtr]$row, $ptr2) | Out-Null
  $rc = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptr2, [type][T210B+RECT])
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr2)
  $x = [int](($rc.L + $rc.R) / 2); $y = [int](($rc.T + $rc.B) / 2)
  $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [T210B]::SendMessageW($nav, $WM_LBUTTONDOWN, [IntPtr]1, $lp) | Out-Null
  [T210B]::SendMessageW($nav, $WM_LBUTTONUP, [IntPtr]0, $lp) | Out-Null
  Start-Sleep -Milliseconds 1200
  [T210B]::SendMessageWStr($search, 0x000C, [IntPtr]::Zero, '') | Out-Null
  Start-Sleep -Milliseconds 900
  Shot (Join-Path $dir 'tools\t210-2-sharehost.png')
}
[T210B]::PostMessageW($main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1200
$p.Refresh()
Write-Host "exited=$($p.HasExited)"
