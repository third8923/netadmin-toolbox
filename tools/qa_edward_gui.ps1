# Edward QA - independent GUI acceptance test (ASCII-only, no BOM dependency)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class EDQ {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$exe  = Join-Path $dir "dist\netadmin-toolbox.exe"
$ini  = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$portscan = -join [char[]](0x7AEF,0x53E3,0x626B,0x63CF)  # "端口扫描"

function Kill-App {
  Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 1200
}
function Shot($h, $name) {
  $r = New-Object EDQ+RECT
  [EDQ]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [EDQ]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output "  saved $name (${w}x${ht})"
}
function SwitchModule($h, $nav, $title) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($title)
  $idx = [EDQ]::SendMessageW($nav, 0x018F, [IntPtr](-1), $ptr).ToInt32()
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
  [EDQ]::SendMessageW($nav, 0x0186, [IntPtr]$idx, [IntPtr]0) | Out-Null
  [EDQ]::PostMessageW($h, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
  Start-Sleep -Milliseconds 900
}
function Launch($label) {
  $env:MZ_ELEV_ATTEMPTED = "1"
  $p = Start-Process $exe -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 900
    $p.Refresh()
    if ($p.HasExited) { Write-Host "  [$label] PROCESS EXITED EARLY rc=$($p.ExitCode)"; return $null }
    $cand = $p.MainWindowHandle
    if ($cand -ne [IntPtr]::Zero -and [EDQ]::GetDlgItem($cand, 1001) -ne [IntPtr]::Zero) { $hwnd = $cand; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Host "  [$label] NO MAIN WINDOW"; return $null }
  Write-Host "  [$label] main window ok"
  return @($p, $hwnd)
}

Write-Output "=== RUN 1: default paper, theme switch, persistence ==="
Kill-App
if (Test-Path $ini) { Remove-Item $ini -Force }
$r = Launch "run1"
if ($r -eq $null) { exit 1 }
$p = $r[0]; $hwnd = $r[1]
Start-Sleep -Milliseconds 600
Shot $hwnd "qa-ed-1-first-paint.png"
$nav = [EDQ]::GetDlgItem($hwnd, 1001)
SwitchModule $hwnd $nav $portscan | Out-Null
Shot $hwnd "qa-ed-2-portscan-paper.png"

# Ctrl+T via real input
[EDQ]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500
[EDQ]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)      # Ctrl down
[EDQ]::keybd_event(0x54, 0, 0, [UIntPtr]::Zero)      # T down
Start-Sleep -Milliseconds 60
[EDQ]::keybd_event(0x54, 0, 2, [UIntPtr]::Zero)      # T up
[EDQ]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)      # Ctrl up
Start-Sleep -Milliseconds 1200
Shot $hwnd "qa-ed-3-portscan-ink.png"
Write-Output "  ini after ctrl+t:"
if (Test-Path $ini) { Get-Content $ini | ForEach-Object { Write-Output "    | $_" } } else { Write-Output "    (ini missing)" }

# switch back to paper, then graceful close
[EDQ]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
[EDQ]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
[EDQ]::keybd_event(0x54, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 60
[EDQ]::keybd_event(0x54, 0, 2, [UIntPtr]::Zero)
[EDQ]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1000
[EDQ]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
$p.WaitForExit(8000) | Out-Null
Start-Sleep -Milliseconds 500
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) {
  Write-Output "  [run1] still alive after WM_CLOSE -> force kill"
  Stop-Process -Id $p.Id -Force
  Start-Sleep -Milliseconds 800
} else { Write-Output "  [run1] exited gracefully on WM_CLOSE" }
Write-Output "  ini persisted after close:"
if (Test-Path $ini) { Get-Content $ini | ForEach-Object { Write-Output "    | $_" } } else { Write-Output "    (ini missing)" }

Write-Output "=== RUN 2: lang=en-US preset, empty-state hint check ==="
Kill-App
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Set-Content -Path $ini -Value "[ui]`r`nlang=en-US" -Encoding ASCII
$r = Launch "run2"
if ($r -ne $null) {
  $p = $r[0]; $hwnd = $r[1]
  Start-Sleep -Milliseconds 600
  $nav = [EDQ]::GetDlgItem($hwnd, 1001)
  SwitchModule $hwnd $nav $portscan | Out-Null
  Shot $hwnd "qa-ed-4-portscan-enUS.png"
  [EDQ]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  $p.WaitForExit(8000) | Out-Null
  Start-Sleep -Milliseconds 400
  if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Force }
}
Write-Output "=== cleanup ==="
Kill-App
if (Test-Path $ini) { Remove-Item $ini -Force; Write-Output "  ini removed" } else { Write-Output "  ini already gone" }
Write-Output "DONE"
