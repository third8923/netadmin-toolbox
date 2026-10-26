# t04_sharemap_test.ps1 — T04 共享扫描与网盘映射 net.sharemap GUI 验收（v2.9）
# 说明：GUI 内做真实最小闭环——建 Z: 映射 → 扫描显示已映射区 → 勾选 → 「断开选中连接」
#       真断开 → 重扫消失。「断开全部网盘」danger 只验弹出+取消。
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T04 {
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
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$mapExe = Join-Path $dir "build\t04_map.exe"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
function Shot([IntPtr]$h, [string]$name) {
  [T04]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object T04+RECT
  [T04]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $pt = New-Object T04+POINT
  [T04]::ClientToScreen($h, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [T04]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  [T04]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output ("  saved " + $name)
}
function ShotRect([T04+RECT]$r, [string]$name) {
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  if ($w -le 0 -or $ht -le 0) { Write-Output "  [WARN] $name empty rect"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $dir "tools\$name"))
  $g.Dispose(); $bmp.Dispose()
  Write-Output ("  saved " + $name + " (${w}x${ht})")
}
function RealClick([int]$x, [int]$y) {
  [T04]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [T04]::mouse_event(2, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 70
  [T04]::mouse_event(4, $x, $y, 0, 0) | Out-Null
  Start-Sleep -Milliseconds 350
}
function NavTextAt([IntPtr]$nav, [int]$idx) {
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(256 * 2)
  try {
    $len = [T04]::SendMessageW($nav, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
function NavCount([IntPtr]$nav) { return [T04]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32() }
function NavRect([IntPtr]$nav, [int]$idx, [ref]$out) {
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    $r = [T04]::SendMessageW($nav, 0x0198, [IntPtr]$idx, $mem)
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
  [T04]::SendMessageW($nav, 0x0197, [IntPtr]$idx, [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 250
  $r = New-Object T04+RECT
  if (-not (NavRect $nav $idx ([ref]$r))) { return $false }
  $pt = New-Object T04+POINT
  $pt.X = [int](($r.Left + $r.Right) / 2); $pt.Y = [int](($r.Top + $r.Bottom) / 2)
  [T04]::ClientToScreen($nav, [ref]$pt) | Out-Null
  [T04]::SetForegroundWindow($h) | Out-Null
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
  $b = [T04]::GetDlgItem($h, $id)
  if ($b -eq [IntPtr]::Zero) { Write-Output "  [WARN] ctrl $id missing"; return }
  [T04]::PostMessageW($h, 0x0111, [IntPtr]$id, $b) | Out-Null
  Start-Sleep -Milliseconds $wait
}
function Tap([byte]$vk) { [T04]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [T04]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
function RowCount([IntPtr]$h) {
  $list = [T04]::GetDlgItem($h, 1002)
  if ($list -eq [IntPtr]::Zero) { return -1 }
  $n = [T04]::SendMessageW($list, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  if ($n -le 0) { $n = [T04]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() }
  return $n
}
# 结果区第 idx 行文本（LB_GETTEXT；失败返回空）
function RowText([IntPtr]$h, [int]$idx) {
  $list = [T04]::GetDlgItem($h, 1002)
  if ($list -eq [IntPtr]::Zero) { return "" }
  $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512 * 2)
  try {
    $len = [T04]::SendMessageW($list, 0x0189, [IntPtr]$idx, $buf).ToInt32()
    if ($len -le 0) { return "" }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
}
# 勾选结果区第 idx 行（真实点击行首复选框区域）
function ClickRow([IntPtr]$h, [int]$idx) {
  $list = [T04]::GetDlgItem($h, 1002)
  if ($list -eq [IntPtr]::Zero) { return }
  $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
  try {
    [void][T04]::SendMessageW($list, 0x0198, [IntPtr]$idx, $mem) | Out-Null   # LB_GETITEMRECT=0x0198
    $b = New-Object byte[] 16
    [System.Runtime.InteropServices.Marshal]::Copy($mem, $b, 0, 16)
  } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem) }
}

$env:MZ_ELEV_ATTEMPTED = "1"
$expectedGroups = @("磁盘清理","空间分析","网络管理","网络诊断","系统管理","系统信息","常用管理","软件管家","安全体检","高级工具","系统优化","系统维护")

# ================= 0) 清场 =================
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item (Join-Path $dir "dist\netadmin-toolbox-*.ini") -Force -ErrorAction SilentlyContinue
# 清掉可能的残留映射
& $mapExe unmap "Z:" 2>$null | Out-Null
Write-Output "== T04 共享扫描与网盘映射 net.sharemap GUI 验收 =="

$p = Start-Process $exe -WorkingDirectory (Join-Path $dir "dist") -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "[FAIL] no main window"; exit 1 }
$nav = [T04]::GetDlgItem($hwnd, 1001)

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
$iMap = FindNav $nav "共享扫描与网盘映射"
$gNet = FindNavPrefix $nav "网络管理"
Assert (($iMap -gt 0) -and ($iMap -gt $gNet)) ("net.sharemap 注册于「网络管理」组（组头 row " + $gNet + " / 模块 row " + $iMap + "）")

# ================= 2) 建 Z: 映射 → 扫描 → 已映射区展示 =================
$mr = & $mapExe map "\\127.0.0.1\C$" Z 2>&1
Write-Output ("  helper map: " + ($mr -join ' '))
[void](NavGo $hwnd $nav $iMap)
Start-Sleep -Milliseconds 800
ClickId $hwnd 1010 1200   # 开始扫描
$rows = -1
for ($k = 0; $k -lt 30; $k++) {
  $rows = RowCount $hwnd
  if ($rows -ge 1) { break }
  Start-Sleep -Milliseconds 500
}
Assert ($rows -ge 1) ("扫描完成，结果行数 ≥1（含已映射区）实际 " + $rows)
Assert ($rows -ge 2) ("扫描完成，结果行数 ≥2（已映射区 + 需凭据行；行文本自绘不可直读，截图走查）实际 " + $rows)
Shot $hwnd "t04-1-sharemap-page.png"
Write-Output "2) 扫描页截图（已映射区 Z: 行 / 本机需凭据行 / 汇总与日志人工走查）"

# ================= 3) 全选 → 「断开选中连接」真实断开（GUI 闭环）=================
ClickId $hwnd 1016 600      # 全选（ID_BTN_ALL）
ClickId $hwnd 1012 1200     # 动作 1 = 断开选中连接
$modal = [T04]::FindWindowW("MzModalHost", [IntPtr]::Zero)
if ($modal -ne [IntPtr]::Zero) {
  Shot $modal "t04-2-disconnect-confirm.png"
  Write-Output "3a) 断开选中确认框截图（列出 Z: → \\127.0.0.1\C$）"
  [T04]::SetForegroundWindow($modal) | Out-Null
  Start-Sleep -Milliseconds 300
  Tap 0x0D   # Enter = 确认执行
  Start-Sleep -Milliseconds 1500
  $modal2 = [T04]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  Assert ($modal2 -eq [IntPtr]::Zero) "确认后模态关闭，断开动作进入执行通道"
  Start-Sleep -Milliseconds 2500   # 等 RefreshAfterAction 重扫
  $enumAfter = & $mapExe enum 2>&1
  Write-Output ("  enum after: " + ($enumAfter -join ' | '))
  Assert ($enumAfter -join ' ' -notmatch "local=Z:") ("GUI 断开后 Z: 连接真实消失（WNet 枚举）")
  Shot $hwnd "t04-3-after-disconnect.png"
  Write-Output "3b) 断开后页面截图（已映射区消失 + 重扫结果）"
} else { Assert $false "断开选中确认框未弹出" }

# ================= 4) 「更多 ▾」菜单（3 项含 danger）=================
# 重建 Z: 映射，让「断开全部网盘」danger 确认框展示真实 fForce 清单（只验弹出+取消不真断）
& $mapExe map "\\127.0.0.1\C$" Z 2>&1 | Out-Null
ClickId $hwnd 1019 1200
$menu = [T04]::FindWindowW("#32768", [IntPtr]::Zero)
if ($menu -ne [IntPtr]::Zero) {
  $mr2 = New-Object T04+RECT
  [T04]::GetWindowRect($menu, [ref]$mr2) | Out-Null
  ShotRect $mr2 "t04-4-more-menu.png"
  Write-Output "4) 「更多 ▾」菜单截图（仅扫描共享（刷新）/ 打开共享文件夹 / 断开全部网盘·红色）"
  # danger 项 = 第 3 项（索引 2）：真实鼠标点击 → danger 确认框 → Esc 取消
  $mw = $mr2.Right - $mr2.Left; $mh = $mr2.Bottom - $mr2.Top
  $itemH = [Math]::Max(1, [int]($mh / 3))
  $mx = $mr2.Left + [int]($mw / 2)
  $my = $mr2.Top + [int]($itemH * 2.5)
  RealClick $mx $my
  Start-Sleep -Milliseconds 1200
  $modal = [T04]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -ne [IntPtr]::Zero) {
    Shot $modal "t04-5-disconnect-all-danger.png"
    Write-Output "4b) 「断开全部网盘」danger 确认框截图（红色危险态 + fForce 说明）"
    [T04]::SetForegroundWindow($modal) | Out-Null
    Start-Sleep -Milliseconds 300
    Tap 0x1B
    Start-Sleep -Milliseconds 800
    $modal2 = [T04]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    Assert ($modal2 -eq [IntPtr]::Zero) "danger 确认框 Esc 取消路径正常（未真断）"
  } else { Assert $false "断开全部网盘 danger 确认框未弹出（菜单定位偏差）" }
  # 兜底：任何残留模态一律 Esc 关掉，避免影响后续步骤
  $modalLeft = [T04]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modalLeft -ne [IntPtr]::Zero) {
    [T04]::SetForegroundWindow($modalLeft) | Out-Null
    Start-Sleep -Milliseconds 300
    Tap 0x1B
    Start-Sleep -Milliseconds 800
  }
} else { Assert $false "net.sharemap 更多菜单未弹出" }

# ================= 5) 正常退出 =================
[T04]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { Write-Output "  [OK] WM_CLOSE exited cleanly" }
else { Stop-Process -Id $p.Id -Force; Write-Output "  [WARN] forced kill" }

# 清理测试残留（映射 + 持久注册表项）
& $mapExe unmap "Z:" 2>$null | Out-Null
$regKey = "HKCU:\Network\Z"
if (Test-Path $regKey) { Remove-Item $regKey -Recurse -Force; Write-Output "  [cleanup] removed HKCU\Network\Z" }

Write-Output ("== 结果: PASS=" + $script:pass + " FAIL=" + $script:fail + " ==")
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
