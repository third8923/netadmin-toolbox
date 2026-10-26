# Verify the confirm dialog actually closes (the critical "stuck dialog" bug)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WD {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$fail = 0

function Shot($hwnd, $name) {
  [WD]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object WD+RECT
  [WD]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WD]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}
function WaitModal($seconds = 5) {
  for ($i = 0; $i -lt ($seconds * 10); $i++) {
    $m = [WD]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    if ($m -ne [IntPtr]::Zero) { return $m }
    Start-Sleep -Milliseconds 100
  }
  return [IntPtr]::Zero
}
function WaitNoModal($seconds = 5) {
  for ($i = 0; $i -lt ($seconds * 10); $i++) {
    $m = [WD]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    if ($m -eq [IntPtr]::Zero) { return $true }
    Start-Sleep -Milliseconds 100
  }
  return $false
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [WD]::GetDlgItem($hwnd, 1001)
$list = [WD]::GetDlgItem($hwnd, 1002)
$btnRun = [WD]::GetDlgItem($hwnd, 1010)
$btnClean = [WD]::GetDlgItem($hwnd, 1011)

# switch to 基础清理 (clean.basic)
$navCount = [WD]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$target = -1
for ($i = 0; $i -lt $navCount; $i++) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
  try {
    [WD]::SendMessageW($nav, 0x0189, [IntPtr]$i, $buf) | Out-Null
    $t = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
    if ($t -match "基础清理") { $target = $i; break }
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
Write-Output "nav[$target] = 基础清理 (clean.basic)"
[WD]::SendMessageW($nav, 0x0186, [IntPtr]$target, [IntPtr]0) | Out-Null
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
Start-Sleep -Milliseconds 1500

# scan first (needed before clean is enabled)
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 400
  if ([WD]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() -gt 0) { break }
}
$rows = [WD]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "scanned rows: $rows"

# ---- Test 1: click 取消 button ----
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr]1011, $btnClean) | Out-Null
$m = WaitModal 5
if ($m -eq [IntPtr]::Zero) {
  Write-Output "[FAIL] confirm dialog did not appear"; $fail = 1
} else {
  Write-Output "[OK] confirm dialog appeared"
  # NOTE: no Shot() here — TOPMOST juggling in test 1 destabilizes Esc/X timing
  $btnCancel = [WD]::GetDlgItem($m, 2)
  [WD]::SendMessageW($btnCancel, 0x00F5, [IntPtr]0, [IntPtr]0) | Out-Null   # BM_CLICK
  if (WaitNoModal 5) {
    Write-Output "[OK] cancel button closes dialog"
  } else {
    Write-Output "[FAIL] cancel button did NOT close dialog (user-reported bug)"; $fail = 1
  }
  $enabled = [WD]::IsWindowEnabled($hwnd)
  Write-Output "main window re-enabled: $enabled"
  if (-not $enabled) { $fail = 1 }
}

# ---- Test 2: Esc key ----
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr]1011, $btnClean) | Out-Null
$m = WaitModal 5
if ($m -ne [IntPtr]::Zero) {
  $btnCancel = [WD]::GetDlgItem($m, 2)
  [WD]::SendMessageW($btnCancel, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null  # WM_KEYDOWN VK_ESCAPE
  if (WaitNoModal 5) { Write-Output "[OK] Esc key closes dialog" }
  else { Write-Output "[FAIL] Esc key did NOT close dialog"; $fail = 1 }
} else { Write-Output "[FAIL] dialog did not reappear for Esc test"; $fail = 1 }

# ---- Test 3: title-bar close (X) button ----
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr]1011, $btnClean) | Out-Null
$m = WaitModal 5
if ($m -ne [IntPtr]::Zero) {
  $btnClose = [WD]::GetDlgItem($m, 4)
  [WD]::SendMessageW($btnClose, 0x00F5, [IntPtr]0, [IntPtr]0) | Out-Null   # BM_CLICK
  if (WaitNoModal 5) { Write-Output "[OK] close (X) button closes dialog" }
  else { Write-Output "[FAIL] close (X) button did NOT close dialog"; $fail = 1 }
} else { Write-Output "[FAIL] dialog did not reappear for X test"; $fail = 1 }

# ---- Test 4: app still functional after dialog interactions ----
[WD]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
Start-Sleep -Seconds 2
$rows2 = [WD]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()
Write-Output "re-scan rows after dialog tests: $rows2"
if ($rows2 -gt 0) { Write-Output "[OK] app still functional" } else { Write-Output "[FAIL] app broken after dialog tests"; $fail = 1 }

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
if ($fail -eq 0) { Write-Output "[ALL OK] dialog bug fixed" } else { Write-Output "[FAILED] see above" }
