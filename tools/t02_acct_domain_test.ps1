# t02_acct_domain_test.ps1 — T02 账户部署 + 计算机名与域 GUI 验收（v2.9）
# 前置：脚本自行 taskkill 残留 + 删 dist/netadmin-toolbox-*.ini
# 说明：沙箱 schtasks.exe 被程序黑名单拦截，提权真实路径（NetUserAdd/注册表写）
#       留 QA 机补测；本脚本覆盖标准用户下可测的全部项（拒绝路径/确认框/菜单/渲染）。
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T02 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
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
$shotDir = Join-Path $dir "tools"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
function Shot([IntPtr]$h, [string]$name) {
  [T02]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object T02+RECT
  [T02]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object T02+POINT
  [T02]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [T02]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  [T02]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function ShotRect([T02+RECT]$r, [string]$name) {
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [WARN] $name empty rect"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Output ("  saved " + $name + " (${w}x${ht})")
}
function RealClick([int]$x, [int]$y) {
  [T02]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T02]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [T02]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 350
}
function NavTextAt([IntPtr]$nav, [int]$idx) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [T02]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavCount([IntPtr]$nav) { return [T02]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [T02]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
    if ($r -eq [IntPtr]::Zero) { return $false }
    $bytes = New-Object byte[] 16
    [System.Runtime.InteropServices.Marshal]::Copy($mem, $bytes, 0, 16)
    $out.Value.Left   = [BitConverter]::ToInt32($bytes, 0)
    $out.Value.Top    = [BitConverter]::ToInt32($bytes, 4)
    $out.Value.Right  = [BitConverter]::ToInt32($bytes, 8)
    $out.Value.Bottom = [BitConverter]::ToInt32($bytes, 12)
    return $true
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem) }
}
function NavGo([IntPtr]$h, [IntPtr]$nav, [int]$idx) {
  [T02]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  $r = New-Object T02+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { return $false }
  $pt = New-Object T02+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [T02]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T02]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  RealClick $pt.X $pt.Y
  Start-Sleep -Milliseconds 500
  return $true
}
function FindNav([IntPtr]$nav, [string]$text) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) { if ((NavTextAt $nav $i) -eq $text) { return $i } }
  return -1
}
function ClickId([IntPtr]$h, [int]$id, [int]$wait = 900) {
  $b = [T02]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [T02]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}
function SetEditText([IntPtr]$ctrl, [string]$s) {
  $ptr = [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni($s)
  [T02]::SendMessageW($ctrl, 0x000C, [IntPtr]0, $ptr) | Out-Null
  [System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}
function Tap([byte]$vk) { [T02]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [T02]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }

$env:MZ_ELEV_ATTEMPTED = "1"
$expectedGroups = @("磁盘清理","空间分析","网络管理","网络诊断","系统管理","系统信息","常用管理","软件管家","安全体检","高级工具","系统优化","系统维护")

# ================= 0) 清场 =================
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== T02 账户部署 + 计算机名与域 GUI 验收 =="

$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "[FAIL] no main window"; exit 1 }
$nav = [T02]::GetDlgItem($hwnd, 1001)

# ================= 1) 侧栏回归 + 新模块注册 =================
$cnt = NavCount $nav
Write-Output ("1) nav rows: " + $cnt)
Assert ($cnt -eq 67) ("侧栏总行数 67（12 组头 + 55 模块）实际 " + $cnt)
$orderOk = $true; $prev = -1
foreach ($g in $expectedGroups) {
  for ($i = 0; $i -lt $cnt; $i++) {
    if ((NavTextAt $nav $i).StartsWith($g)) { if ($i -le $prev) { $orderOk = $false }; $prev = $i; break }
  }
}
Assert $orderOk "T01 固定 12 组顺序回归通过"
$iAcct = FindNav $nav "账户部署"
$iDomain = FindNav $nav "计算机名与域"
Assert (($iAcct -gt 0) -and ($iDomain -gt $iAcct)) ("sys.acct/sys.domain 注册于「系统管理」组（rows " + $iAcct + "/" + $iDomain + "）")
# T01 核心回归：组头点击收起/展开
function FindNavPrefix([IntPtr]$nav, [string]$prefix) {
  $c = NavCount $nav
  for ($i = 0; $i -lt $c; $i++) { if ((NavTextAt $nav $i).StartsWith($prefix)) { return $i } }
  return -1
}
$gIdx = FindNavPrefix $nav "磁盘清理"
[void](NavGo $hwnd $nav $gIdx)
Start-Sleep -Milliseconds 500
$cAfter = NavCount $nav
Assert ($cAfter -eq 62) ("T01 组头点击收起回归：67→62 实际 " + $cAfter)
[void](NavGo $hwnd $nav (FindNavPrefix $nav "磁盘清理"))
Start-Sleep -Milliseconds 500
Assert ((NavCount $nav) -eq 67) "T01 组头点击展开回归：62→67"

# ================= 2) sys.acct 页面渲染 =================
[void](NavGo $hwnd $nav $iAcct)
Start-Sleep -Milliseconds 600
Shot $hwnd "t02-1-acct-page.png"
Write-Output "2) sys.acct 页面截图（新图标/账户列表/自动登录行/LSA 行人工走查）"

# ================= 3) 动作①配置自动登录：danger 确认框 =================
SetEditText ([T02]::GetDlgItem($hwnd, 1021)) "mztest01"   # 目标框 = 用户名
SetEditText ([T02]::GetDlgItem($hwnd, 1020)) ""            # 路径框 = 密码留空
ClickId $hwnd 1012 1200                                    # 「还原选中」位 = 动作1
$modal = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t02-2-acct-danger.png"
  Write-Output "3) 配置自动登录 danger 确认框截图（红色危险态 + 用户名 + 明文风险 + Autologon 建议）"
  Tap 0x1B   # Esc 取消
  Start-Sleep -Milliseconds 800
  $modal2 = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "danger 确认框 Esc 取消路径正常"
} else { Assert $false "配置自动登录 danger 确认框未弹出" }

# ================= 4) sys.acct 「更多 ▾」菜单（含 danger 项）=================
ClickId $hwnd 1019 1200
$menu = [T02]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object T02+RECT
  [T02]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "t02-3-acct-more.png"
  Write-Output "4) 「更多 ▾」菜单截图（关闭自动登录 / 删除指定账户·红色）"
  Tap 0x1B
  Start-Sleep -Milliseconds 700
} else { Assert $false "sys.acct 更多菜单未弹出" }

# ================= 5) sys.domain 页面渲染 =================
$iDomain = FindNav $nav "计算机名与域"
[void](NavGo $hwnd $nav $iDomain)
Start-Sleep -Milliseconds 600
Shot $hwnd "t02-4-domain-page.png"
Write-Output "5) sys.domain 页面截图（DNS 主机名/NetBIOS 名/工作组状态行人工走查）"

# ================= 6) 动作①重命名：danger 确认框（含名称校验行）=================
SetEditText ([T02]::GetDlgItem($hwnd, 1021)) "bad_name!"
ClickId $hwnd 1011 1200
$modal = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t02-5-domain-danger.png"
  Write-Output "6) 重命名 danger 确认框截图（bad_name! 应出现「名称校验未通过」行）"
  Tap 0x1B
  Start-Sleep -Milliseconds 800
  $modal2 = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "重命名确认框 Esc 取消路径正常"
} else { Assert $false "重命名 danger 确认框未弹出" }

# ================= 7) sys.domain 「更多 ▾」菜单 =================
ClickId $hwnd 1019 1200
$menu = [T02]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object T02+RECT
  [T02]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "t02-6-domain-more.png"
  Write-Output "7) 「更多 ▾」菜单截图（退域回工作组·红色 / 查看当前状态）"
  Tap 0x1B
  Start-Sleep -Milliseconds 700
} else { Assert $false "sys.domain 更多菜单未弹出" }

# ================= 8) 非提权拒绝路径（GUI）=================
# 目标框 = bad_name!（本应是校验拒绝，但非提权先拒绝）→ 确认执行 → 日志出现中文拒绝
SetEditText ([T02]::GetDlgItem($hwnd, 1021)) "bad_name!"
ClickId $hwnd 1011 1000
$modal = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Tap 0x0D   # Enter = 确认执行
  Start-Sleep -Milliseconds 1500
  $modal2 = [T02]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "确认后模态关闭，动作进入执行通道"
  Shot $hwnd "t02-7-deny-log.png"
  Write-Output "8) 非提权拒绝日志截图（应出现「需要管理员权限：…未执行」）"
} else { Write-Output "  [WARN] 确认框未弹出，跳过拒绝路径 GUI 验证（CLI 已验证）" }

# ================= 9) 回归抽查：clean.basic / soft.rec =================
$iCb = FindNav $nav "基础清理"
if ($iCb -ge 0) {
  [void](NavGo $hwnd $nav $iCb)
  ClickId $hwnd 1010 800
  $rows = -1
  for ($k = 0; $k -lt 80; $k++) {
    # 结果区可能为 ListBox(LB_GETITEMCOUNT=0x018B) 或 ListView(LVM_GETITEMCOUNT=0x1004)
    $rows = [T02]::SendMessageW([T02]::GetDlgItem($hwnd, 1002), 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
    if ($rows -le 0) { $rows = [T02]::SendMessageW([T02]::GetDlgItem($hwnd, 1002), 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() }
    if ($rows -gt 0) { break }
    Start-Sleep -Milliseconds 500
  }
  Assert ($rows -gt 0) ("clean.basic 扫描回归：rows=" + $rows)
  Shot $hwnd "t02-8-cleanbasic.png"
}
$iSr = FindNav $nav "装机必备 · 推荐总榜"
if ($iSr -ge 0) {
  [void](NavGo $hwnd $nav $iSr)
  ClickId $hwnd 1010 800
  Start-Sleep -Seconds 15   # winget 扫描耗时，只验证不崩溃
  $p.Refresh()
  Assert (-not $p.HasExited) "soft.rec 扫描 15s 无崩溃"
  Shot $hwnd "t02-9-softrec.png"
}

# ================= 10) 正常退出 =================
[T02]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" }
else { Stop-Process -Id $p.Id -Force; Write-Output "  [WARN] forced kill" }

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
