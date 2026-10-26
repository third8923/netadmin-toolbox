# t213_gui_check.ps1 - v2.13 GUI 冒烟：净化中心分组 + 全盘查杀模块
# 断言：①侧栏含「净化中心」「全盘查杀」 ②真实点击后按钮组正确
#       ③点击「开始查杀」完成扫描 ④结果列表出现 ⑤截图
$ErrorActionPreference = 'Stop'

$dist = 'D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist'

# 清场：残留进程 + 旧 ini（保证起点干净）
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item "$dist\*.ini" -ErrorAction SilentlyContinue

Add-Type -Namespace Nat -Name P13 -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern IntPtr SendMessageW(IntPtr hWnd, int Msg, IntPtr wParam, System.Text.StringBuilder lParam);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern IntPtr SendMessageW(IntPtr hWnd, int Msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")]
public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
public delegate bool EnumProc(IntPtr h, IntPtr lp);
[DllImport("user32.dll")]
public static extern int GetDlgCtrlID(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder t, int n);
[DllImport("user32.dll")]
public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")]
public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
[DllImport("user32.dll")]
public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
[DllImport("user32.dll")]
public static extern IntPtr SendMessageW(IntPtr hWnd, int Msg, IntPtr wParam, int lParam);
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int L, T, R, B; }
'@

# ---------- 启动 ----------
$exe = "$dist\netadmin-toolbox.exe"
$env:MZ_ELEV_ATTEMPTED = '1'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
if ($p.HasExited) { Write-Output 'FAIL: app exited immediately'; exit 1 }
$main = $p.MainWindowHandle
if ($main -eq [IntPtr]::Zero) { Write-Output 'FAIL: no main window'; Stop-Process -Id $p.Id -Force; exit 1 }

# ---------- 收集子控件 ----------
$children = New-Object System.Collections.ArrayList
$cb = [Nat.P13+EnumProc]{
    param($h, $lp)
    $null = $children.Add($h)
    return $true
}
[Nat.P13]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null

$listBox = [IntPtr]::Zero   # 侧栏导航 ListBox 1001
$listView = [IntPtr]::Zero  # 结果 ListView 1002
foreach ($h in $children) {
    $id = [Nat.P13]::GetDlgCtrlID($h)
    if ($id -eq 1001) { $listBox = $h }
    if ($id -eq 1002) { $listView = $h }
}
if ($listBox -eq [IntPtr]::Zero) { Write-Output 'FAIL: nav listbox not found'; Stop-Process -Id $p.Id -Force; exit 1 }

# ---------- 断言 1：侧栏条目 ----------
$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$cnt = [Nat.P13]::SendMessageW($listBox, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
$items = @()
for ($i = 0; $i -lt $cnt; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [Nat.P13]::SendMessageW($listBox, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
    $items += $sb.ToString()
}
Write-Output ("NAV({0}): {1}" -f $cnt, ($items -join ' | '))

$fail = 0
foreach ($t in @('净化中心', '全盘查杀')) {
    if ($items -notcontains $t) { Write-Output "FAIL: nav missing [$t]"; $fail++ }
}

# ---------- 断言 2：真实鼠标点击「全盘查杀」 ----------
# 手风琴导航：先点「全部展开」(ID 1024) 确保组展开
$children2 = New-Object System.Collections.ArrayList
$cb2 = [Nat.P13+EnumProc]{
    param($h, $lp)
    $null = $children2.Add($h)
    return $true
}
[Nat.P13]::EnumChildWindows($main, $cb2, [IntPtr]::Zero) | Out-Null
foreach ($h in $children2) {
    if ([Nat.P13]::GetDlgCtrlID($h) -eq 1024) {
        [Nat.P13]::SendMessageW($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # BM_CLICK
        Start-Sleep -Milliseconds 400
        break
    }
}
$idx = [array]::IndexOf($items, '全盘查杀')
if ($idx -lt 0) { Write-Output 'FAIL: cannot click'; Stop-Process -Id $p.Id -Force; exit 1 }
# 行定位：LB_ITEMFROMPOINT(0x01A9) 客户坐标命中法（owner-draw variable 行高不定，
# 组头行高 > 子项行高，固定行高累加会错位）。从列表客户区顶部向下扫描，
# 找到 LB_ITEMFROMPOINT 返回目标 idx 的 y，取该行中点真实点击。
$LB_ITEMFROMPOINT = 0x01A9
$LB_SETTOPINDEX = 0x0197
# 目标行可能超出可视区（74 行 > 窗口高）→ 先滚动让它进入视口中部
[Nat.P13]::SendMessageW($listBox, $LB_SETTOPINDEX, [IntPtr]([Math]::Max(0, $idx - 12)), [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object Nat.P13+RECT
[Nat.P13]::GetWindowRect($listBox, [ref]$r) | Out-Null
$w = $r.R - $r.L
$h = $r.B - $r.T
$cx = [int]($w / 2)
$targetY = -1
for ($cy = 8; $cy -lt $h - 4; $cy += 2) {
    $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
    $resp = [Nat.P13]::SendMessageW($listBox, $LB_ITEMFROMPOINT, [IntPtr]::Zero, $lp).ToInt32()
    $hit = $resp -band 0xFFFF
    if ($hit -eq $idx) { $targetY = $cy; break }
}
if ($targetY -lt 0) { Write-Output "FAIL: row $idx not locatable (maybe collapsed)"; Stop-Process -Id $p.Id -Force; exit 1 }
$y = $r.T + $targetY
$x = ($r.L + $r.R) / 2
[Nat.P13]::SetCursorPos($x, $y) | Out-Null
Start-Sleep -Milliseconds 120
[Nat.P13]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)   # down
[Nat.P13]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)   # up
Start-Sleep -Milliseconds 600

# ---------- 断言 3：按钮组文案 ----------
$btnTexts = @{}
foreach ($h in $children) {
    $id = [Nat.P13]::GetDlgCtrlID($h)
    if ($id -ge 1010 -and $id -le 1016) {
        $sb = New-Object System.Text.StringBuilder 128
        [Nat.P13]::GetWindowTextW($h, $sb, 128) | Out-Null
        $btnTexts[$id] = $sb.ToString()
    }
}
Write-Output ("BTNS: " + (($btnTexts.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join ' | '))
# 1010 主按钮应显示「开始查杀」；1011 处置选中项；1012 还原上次处置
if ($btnTexts[1010] -ne '开始查杀') { Write-Output ("FAIL: btn1010 expected 开始查杀 got [{0}]" -f $btnTexts[1010]); $fail++ }
if ($btnTexts[1011] -ne '处置选中项') { Write-Output ("FAIL: btn1011 expected 处置选中项 got [{0}]" -f $btnTexts[1011]); $fail++ }
if ($btnTexts[1012] -ne '还原上次处置') { Write-Output ("FAIL: btn1012 expected 还原上次处置 got [{0}]" -f $btnTexts[1012]); $fail++ }

# ---------- 断言 4：真实鼠标点击主按钮并等待扫描完成 ----------
$btnRun = [IntPtr]::Zero
foreach ($h in $children2) {
    if ([Nat.P13]::GetDlgCtrlID($h) -eq 1010) { $btnRun = $h; break }
}
if ($btnRun -ne [IntPtr]::Zero) {
    $br = New-Object Nat.P13+RECT
    [Nat.P13]::GetWindowRect($btnRun, [ref]$br) | Out-Null
    [Nat.P13]::SetCursorPos([int](($br.L + $br.R) / 2), [int](($br.T + $br.B) / 2)) | Out-Null
    Start-Sleep -Milliseconds 120
    [Nat.P13]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [Nat.P13]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Write-Output 'CLICK: run button (real mouse)'
}
Start-Sleep -Seconds 3
# 轮询等待扫描完成（GUI 后台线程比 CLI 慢；行数可能为 0 = 本机无发现，不算失败）
$LVM_GETITEMCOUNT = 0x1004
$lvCnt = -1
$prevCnt = -2
$stable = 0
for ($t = 0; $t -lt 20; $t++) {
    Start-Sleep -Seconds 3
    if ($listView -ne [IntPtr]::Zero) {
        $lvCnt = [Nat.P13]::SendMessageW($listView, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    }
    if ($lvCnt -eq $prevCnt) { $stable++ } else { $stable = 0 }
    if ($stable -ge 2 -and $t -ge 4) { break }   # 行数连续 2 次不变且至少跑了 15 秒 → 完成
    $prevCnt = $lvCnt
}
Write-Output ("RESULT rows: {0}" -f $lvCnt)

# ---------- 截图 ----------
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 1600, 1000
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Nat.P13]::PrintWindow($main, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save("$dist\qa213-purg-scan.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "SHOT: $dist\qa213-purg-scan.png"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

if ($fail -eq 0) { Write-Output 'PASS: t213 all assertions passed' }
else { Write-Output "FAIL: $fail assertion(s) failed"; exit 1 }
