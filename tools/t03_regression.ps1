# t03_regression.ps1 — T03 回归：T01 导航持久化/搜索核心项 + T02 sys.acct/sys.domain CLI 抽查
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T03R {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h, string s);
}
"@
function NavCount([IntPtr]$nav) { return [T03R]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function ReadIniNav() {
  $ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $ini)) { return "" }
  foreach ($ln in (Get-Content $ini -Encoding Default)) {
    if ($ln -like "navExpanded=*") { return $ln.Substring(12) }
  }
  return ""
}
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== T03 回归：T01 导航 + T02 CLI 抽查 =="

# ---- 1) 收起「磁盘清理」→ ini 占位/记录 → 重启保持 ----
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T03R]::GetDlgItem($hwnd, 1001)
$cnt0 = NavCount $nav
Assert ($cnt0 -eq 66) ("初始行数 66 实际 " + $cnt0)
# 点击磁盘清理组头（row 0）
$mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[void][T03R]::SendMessageW($nav, 0x0198, [IntPtr]0, $mem)
$b = New-Object byte[] 16
[System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
Add-Type -AssemblyName System.Drawing
# 用 mouse_event 真实点击
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T03RC {
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  public struct POINT { public int X, Y; }
}
"@
$pt = New-Object T03RC+POINT
$pt.X = [BitConverter]::ToInt32($b, 0) + 40
$pt.Y = [BitConverter]::ToInt32($b, 4) + ([BitConverter]::ToInt32($b, 12) - [BitConverter]::ToInt32($b, 4)) / 2
[T03RC]::ClientToScreen($nav, [ref]$pt) | Out-Null
[T03RC]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
[T03RC]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 120
[T03RC]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 70
[T03RC]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
Start-Sleep -Milliseconds 700
$cnt1 = NavCount $nav
Assert ($cnt1 -eq 61) ("组头点击收起 66→61 实际 " + $cnt1)
$iniVal = ReadIniNav
# 只收起 1 组时：ini 记录其余 11 个展开组（不含「磁盘清理」）——语义正确的持久化
$iniGroups = @(); if ($iniVal -and $iniVal -ne "-") { $iniGroups = $iniVal -split ";" }
Assert (($iniGroups.Count -eq 11) -and (-not $iniGroups.Contains("磁盘清理"))) ("ini：收起磁盘清理后记录其余 11 个展开组（" + $iniGroups.Count + " 项）实际 [" + $iniVal + "]")
[T03R]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# ---- 2) 重启后收起态保持 + 搜索强制展开 + 清空恢复 ----
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T03R]::GetDlgItem($hwnd, 1001)
$cnt2 = NavCount $nav
Assert ($cnt2 -eq 61) ("重启后收起态保持 61 实际 " + $cnt2)
$search = [T03R]::GetDlgItem($hwnd, 1022)
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("清理")
[T03R]::SendMessageW($search, 0x000C, [IntPtr]0, $ptr) | Out-Null
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 900
$cnt3 = NavCount $nav
Assert ($cnt3 -ge 5) ("收起态搜索「清理」强制展开 61→" + $cnt3)
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("")
[T03R]::SendMessageW($search, 0x000C, [IntPtr]0, $ptr) | Out-Null
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 900
$cnt4 = NavCount $nav
Assert ($cnt4 -eq 61) ("清空搜索恢复折叠记忆 61 实际 " + $cnt4)
[T03R]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" } else { Stop-Process -Id $p.Id -Force }

# ---- 3) T02 CLI 抽查 ----
Remove-Item Env:HTTP_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:http_proxy -ErrorAction SilentlyContinue
Remove-Item Env:https_proxy -ErrorAction SilentlyContinue
Remove-Item Env:ALL_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:all_proxy -ErrorAction SilentlyContinue
$b = Join-Path $dir "build"
$p1 = Start-Process $exe -ArgumentList "run","sys.acct" -RedirectStandardOutput "$b\t03_reg_acct.txt" -RedirectStandardError "$b\t03_reg_err.txt" -NoNewWindow -Wait -PassThru
$p2 = Start-Process $exe -ArgumentList "run","sys.domain" -RedirectStandardOutput "$b\t03_reg_domain.txt" -RedirectStandardError "$b\t03_reg_err2.txt" -NoNewWindow -Wait -PassThru
Assert (($p1.ExitCode -eq 0) -and ($p2.ExitCode -eq 0)) ("sys.acct / sys.domain CLI 抽查 exit=" + $p1.ExitCode + "/" + $p2.ExitCode)
$acctOut = (Get-Content "$b\t03_reg_acct.txt" -Encoding UTF8 | Measure-Object -Line).Lines
$domOut = (Get-Content "$b\t03_reg_domain.txt" -Encoding UTF8 | Measure-Object -Line).Lines
Assert (($acctOut -gt 3) -and ($domOut -gt 3)) ("sys.acct 输出 " + $acctOut + " 行 / sys.domain 输出 " + $domOut + " 行")

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
