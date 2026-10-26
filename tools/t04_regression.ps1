# t04_regression.ps1 — T04 回归：T01 导航核心项 + T02/T03 CLI 抽查 + 拒绝输出干净性
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
public class T04R {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  public struct POINT { public int X, Y; }
}
"@
function NavCount([IntPtr]$nav) { return [T04R]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function ReadIniNav() {
  $ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
  if (-not (Test-Path $ini)) { return "" }
  foreach ($ln in (Get-Content $ini -Encoding Default)) {
    if ($ln -like "navExpanded=*") { return $ln.Substring(12) }
  }
  return ""
}
function ClickNavRow([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  [void][T04R]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
  $b = New-Object byte[] 16
  [System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
  $pt = New-Object T04R+POINT
  $pt.X = [BitConverter]::ToInt32($b, 0) + 40
  $pt.Y = [BitConverter]::ToInt32($b, 4) + ([BitConverter]::ToInt32($b, 12) - [BitConverter]::ToInt32($b, 4)) / 2
  [T04R]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T04R]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  [T04R]::SetCursorPos($pt.X, $pt.Y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T04R]::mouse_event(2, $pt.X, $pt.Y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [T04R]::mouse_event(4, $pt.X, $pt.Y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 600
}
$env:MZ_ELEV_ATTEMPTED = "1"
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== T04 回归：T01 导航 + T02/T03 CLI 抽查 + 拒绝输出干净性 =="

# ---- 1) T01 导航核心（67 行基线）----
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T04R]::GetDlgItem($hwnd, 1001)
$cnt0 = NavCount $nav
Assert ($cnt0 -eq 67) ("初始行数 67 实际 " + $cnt0)
# 组头点击收起（带验证重试，防启动期点击竞态）
$cnt1 = 0
for ($a = 0; $a -lt 4; $a++) {
  ClickNavRow $hwnd $nav 0   # 磁盘清理组头
  Start-Sleep -Milliseconds 600
  $cnt1 = NavCount $nav
  if ($cnt1 -eq 62) { break }
  if ($cnt1 -eq 67) { continue }   # 未生效 → 重试
  break
}
Assert ($cnt1 -eq 62) ("组头点击收起 67→62 实际 " + $cnt1)
$iniVal = ReadIniNav
$iniGroups = @(); if ($iniVal -and $iniVal -ne "-") { $iniGroups = $iniVal -split ";" }
Assert (($iniGroups.Count -eq 11) -and (-not $iniGroups.Contains("磁盘清理"))) ("ini：收起磁盘清理后记录其余 11 个展开组 实际 " + $iniGroups.Count + " 项")
[T04R]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# ---- 2) 重启保持 + 搜索强制展开/清空恢复 ----
$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
$nav = [T04R]::GetDlgItem($hwnd, 1001)
$cnt2 = NavCount $nav
Assert ($cnt2 -eq 62) ("重启后收起态保持 62 实际 " + $cnt2)
$search = [T04R]::GetDlgItem($hwnd, 1022)
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("清理")
[T04R]::SendMessageW($search, 0x000C, [IntPtr]0, $ptr) | Out-Null
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 900
$cnt3 = NavCount $nav
Assert ($cnt3 -ge 5) ("收起态搜索「清理」强制展开 62→" + $cnt3)
$ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("")
[T04R]::SendMessageW($search, 0x000C, [IntPtr]0, $ptr) | Out-Null
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
Start-Sleep -Milliseconds 900
$cnt4 = NavCount $nav
Assert ($cnt4 -eq 62) ("清空搜索恢复折叠记忆 62 实际 " + $cnt4)
[T04R]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" } else { Stop-Process -Id $p.Id -Force }

# ---- 3) CLI 抽查（清代理变量）----
Remove-Item Env:HTTP_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:http_proxy -ErrorAction SilentlyContinue
Remove-Item Env:https_proxy -ErrorAction SilentlyContinue
Remove-Item Env:ALL_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:all_proxy -ErrorAction SilentlyContinue
$b = Join-Path $dir "build"
$p1 = Start-Process $exe -ArgumentList "run","sys.acct" -RedirectStandardOutput "$b\t04_reg_acct.txt" -RedirectStandardError "$b\t04_reg_e1.txt" -NoNewWindow -Wait -PassThru
$p2 = Start-Process $exe -ArgumentList "run","sys.domain" -RedirectStandardOutput "$b\t04_reg_domain.txt" -RedirectStandardError "$b\t04_reg_e2.txt" -NoNewWindow -Wait -PassThru
$p3 = Start-Process $exe -ArgumentList "run","net.sharehost" -RedirectStandardOutput "$b\t04_reg_sharehost.txt" -RedirectStandardError "$b\t04_reg_e3.txt" -NoNewWindow -Wait -PassThru
Assert (($p1.ExitCode -eq 0) -and ($p2.ExitCode -eq 0) -and ($p3.ExitCode -eq 0)) ("sys.acct / sys.domain / net.sharehost CLI 抽查 exit=" + $p1.ExitCode + "/" + $p2.ExitCode + "/" + $p3.ExitCode)
$sh = Get-Content "$b\t04_reg_sharehost.txt" -Encoding UTF8
Assert (($sh | Measure-Object -Line).Lines -ge 14) ("net.sharehost 体检行数 ≥14 实际 " + ($sh | Measure-Object -Line).Lines)

# ---- 4) 拒绝输出干净性（cli.cpp 无重复拼接）----
$p4 = Start-Process $exe -ArgumentList "run","net.sharehost","--action","0" -RedirectStandardOutput "$b\t04_reg_deny.txt" -RedirectStandardError "$b\t04_reg_e4.txt" -NoNewWindow -Wait -PassThru
$deny = Get-Content "$b\t04_reg_deny.txt" -Encoding UTF8
$dup = $deny | Where-Object { $_ -match "需要管理员权限需要管理员权限" }
Assert (($deny | Measure-Object -Line).Lines -ge 4) ("拒绝输出行数正常（" + ($deny | Measure-Object -Line).Lines + " 行）")
Assert ($null -eq $dup) "拒绝输出无「需要管理员权限」重复拼接（每行独立干净）"
$deny | ForEach-Object { "    |" + $_ } | Select-Object -First 6

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
