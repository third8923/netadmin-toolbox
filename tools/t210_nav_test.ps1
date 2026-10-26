# v2.10 sidebar smoke test: no group-count numbers, accordion, expand/collapse-all buttons
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $dir 'dist\netadmin-toolbox.exe'
$ini = Join-Path $dir 'dist\netadmin-toolbox-theme.ini'
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
if (Test-Path $ini) { Remove-Item $ini -Force }

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class T210 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder c, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static Dictionary<int, IntPtr> Kids = new Dictionary<int, IntPtr>();
  public static void Collect(IntPtr parent) {
    Kids.Clear();
    EnumChildWindows(parent, (h, l) => { Kids[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
  }
}
"@

$WM_COMMAND = 0x0111; $LB_GETCOUNT = 0x018B; $LB_GETTEXT = 0x0189
$LB_GETITEMRECT = 0x0198; $LB_SETTOPINDEX = 0x0197; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$WM_CLOSE = 0x0010
$pass = 0; $fail = 0
function Check($cond, $name) {
  if ($cond) { $script:pass++; Write-Host "PASS  $name" }
  else { $script:fail++; Write-Host "FAIL  $name" }
}

$env:MZ_ELEV_ATTEMPTED = '1'
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir 'dist') -PassThru
$ok = $false
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 500
  $p.Refresh()
  if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $ok = $true; break }
}
Check $ok 'main window appeared'
$main = $p.MainWindowHandle
[T210]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 1500

[T210]::Collect($main)
$nav = [T210]::Kids[1001]
Check ($nav -ne $null -and $nav -ne [IntPtr]::Zero) 'nav listbox found (CtrlID 1001)'
if ($nav -eq $null -or $nav -eq [IntPtr]::Zero) {
  Write-Host "children ids: $([string]::Join(',', ([T210]::Kids.Keys | Sort-Object)))"
}

function NavCount { return [int][T210]::SendMessageW($nav, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) }
function NavText($i) {
  $sb = New-Object System.Text.StringBuilder 256
  $ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
  try {
    [T210]::SendMessageW($nav, $LB_GETTEXT, [IntPtr]$i, $ptr) | Out-Null
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($ptr)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr) }
}
function ClickRow($i) {
  [T210]::SendMessageW($nav, $LB_SETTOPINDEX, [IntPtr]$i, [IntPtr]::Zero) | Out-Null
  $ptr = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    [T210]::SendMessageW($nav, $LB_GETITEMRECT, [IntPtr]$i, $ptr) | Out-Null
    $r = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptr, [type][T210+RECT])
    $x = [int](($r.L + $r.R) / 2); $y = [int](($r.T + $r.B) / 2)
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [T210]::SendMessageW($nav, $WM_LBUTTONDOWN, [IntPtr]1, $lp) | Out-Null
    [T210]::SendMessageW($nav, $WM_LBUTTONUP, [IntPtr]0, $lp) | Out-Null
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr) }
  Start-Sleep -Milliseconds 600
}
function SendCmd($id) {
  [T210]::PostMessageW($main, $WM_COMMAND, [IntPtr]((0 -shl 16) -bor ($id -band 0xFFFF)), [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 700
}

# 1) fresh start, default all expanded
Check ((NavCount) -eq 67) "initial all-expanded = 67 rows (got $(NavCount))"
# 2) group header text has no count number
$t0 = NavText 0
Check ($t0 -notmatch '\u00b7') "group header has no '· N' count (row0='$t0')"
# 3) collapse-all button
SendCmd 1025
Check ((NavCount) -eq 12) "collapse-all -> 12 group headers (got $(NavCount))"
# 4) accordion: click first group header (磁盘清理)
ClickRow 0
Check ((NavCount) -eq 17) "expand 磁盘清理 -> 17 rows (12+5) (got $(NavCount))"
# 5) accordion: click another group header collapses previous
#    after expand, row6 = 空间分析 header
$t6 = NavText 6
ClickRow 6
Check ((NavCount) -eq 18) "accordion switch to 空间分析 -> 18 rows (12+6), previous collapsed (got $(NavCount))"
Check ((NavText 0) -eq '磁盘清理') "row0 still 磁盘清理 header after accordion switch (got '$(NavText 0)')"
# 6) expand-all button
SendCmd 1024
Check ((NavCount) -eq 67) "expand-all -> 67 rows (got $(NavCount))"
# 7) expand-all again after collapse-all mid-state
SendCmd 1025; SendCmd 1024
Check ((NavCount) -eq 67) "collapse+expand cycle stable (got $(NavCount))"

Write-Host "----- RESULT: PASS=$pass FAIL=$fail -----"
[T210]::PostMessageW($main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1500
$p.Refresh()
Check $p.HasExited 'clean exit'
Write-Host "EXIT_OK=$($p.HasExited)"
