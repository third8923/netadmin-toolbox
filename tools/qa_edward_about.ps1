# Edward QA - About dialog screenshot (ASCII-only)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class EDQ2 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$exe  = Join-Path $dir "dist\netadmin-toolbox.exe"
$ini  = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force }

function Shot($h, $name) {
  $r = New-Object EDQ2+RECT
  [EDQ2]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [EDQ2]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}

$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process $exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $cand = $p.MainWindowHandle
  if ($cand -ne [IntPtr]::Zero -and [EDQ2]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NO MAIN WINDOW"; exit 1 }
Start-Sleep -Milliseconds 800
# open About dialog: WM_COMMAND ID_BTN_ABOUT=1015
[EDQ2]::PostMessageW($hwnd, 0x0111, [IntPtr]1015, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1500
# find dialog window (owned popup)
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class EDQ3 {
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
$dlg = [IntPtr]::Zero
$cur = [EDQ3]::GetWindow($hwnd, 5)  # GW_HWNDNEXT? use 4=GW_CHILD fallback loop instead
# enumerate top-level windows of this process via .NET
$proc = Get-Process -Id $p.Id
$proc.Refresh()
$found = $false
for ($try = 0; $try -lt 10 -and -not $found; $try++) {
  Start-Sleep -Milliseconds 400
  $p.Refresh()
  if ($p.MainWindowHandle -ne $hwnd -and $p.MainWindowHandle -ne [IntPtr]::Zero) { $dlg = $p.MainWindowHandle; $found = $true }
}
if (-not $found) { Write-Output "no separate dialog hwnd; shooting main"; Shot $hwnd "qa-ed-5-about-fallback.png" }
else { Write-Output "dialog hwnd found"; Shot $dlg "qa-ed-5-about.png" }
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
if (Test-Path $ini) { Remove-Item $ini -Force }
Write-Output "DONE"
