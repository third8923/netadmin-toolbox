# Verify column sorting with real mouse click (remote-memory LVM_GETITEMTEXTW, cross-process safe)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WS {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
public class RM {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);
  [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint dwFreeType);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out int lpNumberOfBytesWritten);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out int lpNumberOfBytesRead);
}
"@

$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"

function Shot($hwnd, $name) {
  [WS]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object WS+RECT
  [WS]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WS]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}
function Get-LvItemText($procId, $listHwnd, $item, $subItem) {
  # allocate LVITEMW (88B, x64) + text buffer inside the target process
  $hProc = [RM]::OpenProcess(0x001F0FFF, $false, $procId)
  if ($hProc -eq [IntPtr]::Zero) { return $null }
  try {
    $structSize = 88; $bufChars = 256
    $remote = [RM]::VirtualAllocEx($hProc, [IntPtr]::Zero, ($structSize + $bufChars * 2), 0x3000, 0x40)
    if ($remote -eq [IntPtr]::Zero) { return $null }
    try {
      $textPtr = [IntPtr]($remote.ToInt64() + $structSize)
      $lv = New-Object byte[] $structSize
      [BitConverter]::GetBytes([int]1).CopyTo($lv, 0)       # mask = LVIF_TEXT (required by comctl32)
      [BitConverter]::GetBytes([int]$item).CopyTo($lv, 4)      # iItem
      [BitConverter]::GetBytes([int]$subItem).CopyTo($lv, 8)   # iSubItem
      [BitConverter]::GetBytes($textPtr.ToInt64()).CopyTo($lv, 24)  # pszText
      [BitConverter]::GetBytes([int]$bufChars).CopyTo($lv, 32) # cchTextMax
      $written = 0
      [RM]::WriteProcessMemory($hProc, $remote, $lv, $structSize, [ref]$written) | Out-Null
      [WS]::SendMessageW($listHwnd, 0x104B, [IntPtr]$item, $remote) | Out-Null   # LVM_GETITEMTEXTW
      $out = New-Object byte[] ($bufChars * 2)
      $read = 0
      [RM]::ReadProcessMemory($hProc, $textPtr, $out, $bufChars * 2, [ref]$read) | Out-Null
      $s = [System.Text.Encoding]::Unicode.GetString($out)
      $z = $s.IndexOf([char]0)
      if ($z -ge 0) { $s = $s.Substring(0, $z) }
      return $s
    } finally {
      [RM]::VirtualFreeEx($hProc, $remote, 0, 0x8000) | Out-Null
    }
  } finally {
    [RM]::CloseHandle($hProc) | Out-Null
  }
}
function NavTextAt($nav, $idx) {
  # LB_GETTEXT is USER32-marshaled cross-process (works for LBS_HASSTRINGS listboxes)
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [WS]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
  }
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [WS]::GetDlgItem($hwnd, 1001)
$list = [WS]::GetDlgItem($hwnd, 1002)
$header = [WS]::SendMessageW($list, 0x101F, [IntPtr]0, [IntPtr]0)

# find nav entry whose text contains "process"
$navCount = [WS]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
$target = -1
for ($i = 0; $i -lt $navCount; $i++) {
  $t = NavTextAt $nav $i
  if ($t -match "进程|process") { $target = $i; Write-Output "nav[$i] = $t"; break }
}
if ($target -lt 0) { Write-Output "[FAIL] process module not found in nav"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; exit 1 }

# select it
[WS]::SendMessageW($nav, 0x0186, [IntPtr]$target, [IntPtr]0) | Out-Null
[WS]::PostMessageW($hwnd, 0x0111, [IntPtr](1001 -bor (1 -shl 16)), $nav) | Out-Null
Start-Sleep -Milliseconds 1500

# scan and poll until rows appear
$btnRun = [WS]::GetDlgItem($hwnd, 1010)
[WS]::PostMessageW($hwnd, 0x0111, [IntPtr]1010, $btnRun) | Out-Null
$rows = 0
for ($i = 0; $i -lt 30; $i++) {
  Start-Sleep -Milliseconds 400
  $rows = [WS]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32()
  if ($rows -gt 0) { break }
}
Write-Output "process rows: $rows"
if ($rows -le 0) { Write-Output "[FAIL] scan produced no rows"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; exit 1 }
$first1 = Get-LvItemText $p.Id $list 0 0
Write-Output "first row (before sort): '$first1'"
Shot $hwnd "new-9-process.png"

# real click on header column 0
[WS]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
$hr = New-Object WS+RECT
[WS]::GetWindowRect($header, [ref]$hr) | Out-Null
$cx = $hr.Left + 150; $cy = $hr.Top + 10
[WS]::SetCursorPos($cx, $cy) | Out-Null
[WS]::mouse_event(2, $cx, $cy, 0, 0) | Out-Null   # LEFTDOWN
Start-Sleep -Milliseconds 80
[WS]::mouse_event(4, $cx, $cy, 0, 0) | Out-Null   # LEFTUP
Start-Sleep -Milliseconds 1000
$first2 = Get-LvItemText $p.Id $list 0 0
Write-Output "first row (asc): '$first2'"
Shot $hwnd "new-10-sorted.png"

# click again to reverse
[WS]::SetCursorPos($cx, $cy) | Out-Null
[WS]::mouse_event(2, $cx, $cy, 0, 0) | Out-Null
Start-Sleep -Milliseconds 80
[WS]::mouse_event(4, $cx, $cy, 0, 0) | Out-Null
Start-Sleep -Milliseconds 1000
$first3 = Get-LvItemText $p.Id $list 0 0
Write-Output "first row (desc): '$first3'"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$ok = ($first1 -ne $first2) -and ($first1 -ne $first3) -and ($first2 -ne $first3)
if ($ok) { Write-Output "[OK] column sorting works (3 distinct orders)" }
else { Write-Output "[FAIL] sorting had no effect: '$first1' / '$first2' / '$first3'" }
