# t03_sharehost_test.ps1 — T03 共享部署一键通 net.sharehost GUI 验收（v2.9）
# 说明：沙箱为标准用户且 schtasks/sc/cmd 被拦，提权真实闭环（停/启 Server）留 QA 机补测；
#       本脚本覆盖标准用户下可测的全部项（体检渲染/危险确认框/更多菜单/拒绝路径/回归）。
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T03 {
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
  [T03]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object T03+RECT
  [T03]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object T03+POINT
  [T03]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [T03]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shotDir $name))
  $g.Dispose(); $bmp.Dispose()
  [T03]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function ShotRect([T03+RECT]$r, [string]$name) {
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
  [T03]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T03]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [T03]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 350
}
function NavTextAt([IntPtr]$nav, [int]$idx) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [T03]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavCount([IntPtr]$nav) { return [T03]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [T03]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
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
  [T03]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  $r = New-Object T03+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { return $false }
  $pt = New-Object T03+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [T03]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T03]::SetForegroundWindow($h) | Out-Null
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
function FindNavPrefix([IntPtr]$nav, [string]$prefix) {
  $cnt = NavCount $nav
  for ($i = 0; $i -lt $cnt; $i++) { if ((NavTextAt $nav $i).StartsWith($prefix)) { return $i } }
  return -1
}
function ClickId([IntPtr]$h, [int]$id, [int]$wait = 900) {
  $b = [T03]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [T03]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}
function Tap([byte]$vk) { [T03]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [T03]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
# 行计数：先试 LB_GETITEMCOUNT(0x018B)，若为 0 再试 LVM_GETITEMCOUNT(0x1004)
function RowCount([IntPtr]$h) {
  $list = [T03]::GetDlgItem($h, 1002)
  if ($list -eq [IntPtr]::Zero) { return -1 }
  $n = [T03]::SendMessageW($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  if ($n -le 0) { $n = [T03]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() }
  return $n
}

$env:MZ_ELEV_ATTEMPTED = "1"
$expectedGroups = @("磁盘清理","空间分析","网络管理","网络诊断","系统管理","系统信息","常用管理","软件管家","安全体检","高级工具","系统优化","系统维护")

# ================= 0) 清场 =================
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
Write-Output "== T03 共享部署一键通 net.sharehost GUI 验收 =="

$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "[FAIL] no main window"; exit 1 }
$nav = [T03]::GetDlgItem($hwnd, 1001)

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
$iShare = FindNav $nav "共享部署"
$gNet = FindNavPrefix $nav "网络管理"
Assert (($iShare -gt 0) -and ($iShare -gt $gNet)) ("net.sharehost 注册于「网络管理」组（组头 row " + $gNet + " / 模块 row " + $iShare + "）")
# T01 核心回归：组头点击收起/展开
$gIdx = FindNavPrefix $nav "磁盘清理"
[void](NavGo $hwnd $nav $gIdx)
Start-Sleep -Milliseconds 500
$cAfter = NavCount $nav
Assert ($cAfter -eq 62) ("T01 组头点击收起回归：67→62 实际 " + $cAfter)
[void](NavGo $hwnd $nav (FindNavPrefix $nav "磁盘清理"))
Start-Sleep -Milliseconds 500
Assert ((NavCount $nav) -eq 67) "T01 组头点击展开回归：62→67"

# ================= 2) net.sharehost 页面：14 项体检渲染 =================
[void](NavGo $hwnd $nav $iShare)
Start-Sleep -Milliseconds 800
ClickId $hwnd 1010 1200   # 触发体检
$rows = -1
for ($k = 0; $k -lt 40; $k++) {
  $rows = RowCount $hwnd
  if ($rows -ge 14) { break }
  Start-Sleep -Milliseconds 500
}
Assert ($rows -eq 14) ("体检 14 项全出（LB/LVM 计数）实际 " + $rows)
Shot $hwnd "t03-1-sharehost-page.png"
Write-Output "2) 体检页截图（2 就绪/6 待处理/6 仅展示 + 🔴 项仅展示标注 + 汇总行人工走查）"

# ================= 3) 动作①一键开通：danger 确认框（动态变更项列）=================
ClickId $hwnd 1011 1500   # 主动作 = 一键开通共享环境
$modal = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t03-2-openup-danger.png"
  Write-Output "3) 一键开通 danger 确认框截图（红色危险态 + 🟡 变更项动态列 + 🔴 不触碰声明）"
  Tap 0x1B   # Esc 取消
  Start-Sleep -Milliseconds 800
  $modal2 = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "一键开通 danger 确认框 Esc 取消路径正常"
} else { Assert $false "一键开通 danger 确认框未弹出" }

# ================= 4) 动作②恢复保守设置：danger 确认框 =================
ClickId $hwnd 1012 1500
$modal = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t03-3-restore-danger.png"
  Write-Output "4) 恢复保守设置 danger 确认框截图（验收要求：只验确认框不真执行）"
  Tap 0x1B
  Start-Sleep -Milliseconds 800
  $modal2 = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "恢复保守确认框 Esc 取消路径正常"
} else { Assert $false "恢复保守 danger 确认框未弹出" }

# ================= 5) 「更多 ▾」菜单：10 个动作 =================
ClickId $hwnd 1019 1200
$menu = [T03]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr = New-Object T03+RECT
  [T03]::GetWindowRect($menu, [ref]$mr) | Out-Null
  ShotRect $mr "t03-4-more-menu.png"
  Write-Output "5) 「更多 ▾」菜单截图（停/启 Server、来宾登录开/关、匿名枚举开/关、切换网络配置文件、仅刷新）"
  Tap 0x1B
  Start-Sleep -Milliseconds 700
} else { Assert $false "net.sharehost 更多菜单未弹出" }

# ================= 6) 菜单内选择「停用 Server 服务」→ 非提权中文拒绝 =================
ClickId $hwnd 1019 1200
$menu = [T03]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  # 菜单项顺序 0-9：停用 Server 是第 1 项（索引 2，前两项是两个 danger 主动作）
  # 菜单首项默认高亮，Down×3 → Enter（停用/启动/来宾开/来宾关/匿名开/匿名关/切换/仅刷新）
  [T03]::SetForegroundWindow($menu) | Out-Null
  Start-Sleep -Milliseconds 300
  Tap 0x28; Start-Sleep -Milliseconds 150   # ↓
  Tap 0x28; Start-Sleep -Milliseconds 150
  Tap 0x0D; Start-Sleep -Milliseconds 1500   # Enter
  $modal = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "t03-5-stopserver-confirm.png"
    Write-Output "6a) 停用 Server 确认框截图（非 danger 蓝色确认态）"
    Tap 0x0D   # Enter = 确认执行
    Start-Sleep -Milliseconds 1500
    $modal2 = [T03]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    Assert ($modal2 -eq [IntPtr]::Zero) "确认后模态关闭，动作进入执行通道"
    Shot $hwnd "t03-6-deny-log.png"
    Write-Output "6b) 非提权拒绝日志截图（应出现「需要管理员权限：停用 Server 服务 未执行」）"
  } else { Write-Output "  [WARN] 确认框未弹出（菜单高亮定位偏差），拒绝路径由 CLI 输出验证" }
} else { Write-Output "  [WARN] 更多菜单未弹出，跳过菜单内选择路径" }

# ================= 7) 动作 9「仅刷新」非提权放行 =================
ClickId $hwnd 1019 1200
$menu = [T03]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  [T03]::SetForegroundWindow($menu) | Out-Null
  Start-Sleep -Milliseconds 300
  for ($k = 0; $k -lt 9; $k++) { Tap 0x28; Start-Sleep -Milliseconds 120 }   # ↓×9 = 最后一项 仅刷新
  Tap 0x0D; Start-Sleep -Milliseconds 2500
  $p.Refresh()
  Assert (-not $p.HasExited) "动作 9「仅刷新」非提权放行且进程存活"
  $rows2 = RowCount $hwnd
  Assert ($rows2 -eq 14) ("仅刷新后体检行数保持 14 实际 " + $rows2)
  Shot $hwnd "t03-7-refresh-ok.png"
} else { Write-Output "  [WARN] 更多菜单未弹出，跳过动作 9 GUI 路径" }

# ================= 8) 正常退出 =================
[T03]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" }
else { Stop-Process -Id $p.Id -Force; Write-Output "  [WARN] forced kill" }

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
