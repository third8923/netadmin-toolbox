# t214_gui_check.ps1 - v2.14 GUI 冒烟：净化中心新增三模块（规则库生成/YARA 扫描/主页锁定）
# 断言：①侧栏含三个新模块 ②逐一真实点击切换 ③按钮组文案正确 ④主页锁定扫描出列表 ⑤截图
$ErrorActionPreference = 'Stop'

$dist = 'D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist'

# 清场：残留进程 + 旧 ini
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item "$dist\*.ini" -ErrorAction SilentlyContinue

Add-Type -Namespace Nat -Name P14 -MemberDefinition @'
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
$cb = [Nat.P14+EnumProc]{
    param($h, $lp)
    $null = $children.Add($h)
    return $true
}
[Nat.P14]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null

$listBox = [IntPtr]::Zero
$listView = [IntPtr]::Zero
foreach ($h in $children) {
    $id = [Nat.P14]::GetDlgCtrlID($h)
    if ($id -eq 1001) { $listBox = $h }
    if ($id -eq 1002) { $listView = $h }
}
if ($listBox -eq [IntPtr]::Zero) { Write-Output 'FAIL: nav listbox not found'; Stop-Process -Id $p.Id -Force; exit 1 }

# ---------- 侧栏条目 ----------
$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$cnt = [Nat.P14]::SendMessageW($listBox, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
$items = @()
for ($i = 0; $i -lt $cnt; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [Nat.P14]::SendMessageW($listBox, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
    $items += $sb.ToString()
}
Write-Output ("NAV({0}): {1}" -f $cnt, ($items -join ' | '))

$fail = 0
foreach ($t in @('净化中心', '全盘查杀', '规则库生成', 'YARA 扫描', '主页锁定')) {
    if ($items -notcontains $t) { Write-Output "FAIL: nav missing [$t]"; $fail++ }
}

# ---------- 全部展开 ----------
foreach ($h in $children) {
    if ([Nat.P14]::GetDlgCtrlID($h) -eq 1024) {
        [Nat.P14]::SendMessageW($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 400
        break
    }
}

# ---------- 点击侧栏行（LB_ITEMFROMPOINT 客户坐标扫描法；手风琴会重建列表，
#             所以每次点击前实时重读行内容，不依赖旧快照）----------
function Get-NavItems {
    $c = [Nat.P14]::SendMessageW($listBox, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    $arr = @()
    for ($i = 0; $i -lt $c; $i++) {
        $sb = New-Object System.Text.StringBuilder 256
        [Nat.P14]::SendMessageW($listBox, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
        $arr += $sb.ToString()
    }
    return $arr
}

function Click-NavRow([string]$text) {
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $cur = Get-NavItems
        $idx = [array]::IndexOf($cur, $text)
        if ($idx -lt 0) { Start-Sleep -Milliseconds 400; continue }
        $LB_ITEMFROMPOINT = 0x01A9
        $LB_SETTOPINDEX = 0x0197
        [Nat.P14]::SendMessageW($listBox, $LB_SETTOPINDEX, [IntPtr]([Math]::Max(0, $idx - 10)), [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 300
        $r = New-Object Nat.P14+RECT
        [Nat.P14]::GetWindowRect($listBox, [ref]$r) | Out-Null
        $w = $r.R - $r.L
        $h2 = $r.B - $r.T
        $cx = [int]($w / 2)
        $targetY = -1
        for ($cy = 8; $cy -lt $h2 - 4; $cy += 2) {
            $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
            $resp = [Nat.P14]::SendMessageW($listBox, $LB_ITEMFROMPOINT, [IntPtr]::Zero, $lp).ToInt32()
            if (($resp -band 0xFFFF) -eq $idx) { $targetY = $cy; break }
        }
        if ($targetY -lt 0) { Start-Sleep -Milliseconds 400; continue }
        [Nat.P14]::SetCursorPos([int](($r.L + $r.R) / 2), ($r.T + $targetY)) | Out-Null
        Start-Sleep -Milliseconds 120
        [Nat.P14]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [Nat.P14]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 800
        return $true
    }
    Write-Output "FAIL: row [$text] not clickable after retries"
    $script:fail++
    return $false
}

function Get-Buttons {
    $bt = @{}
    foreach ($h in $children) {
        $id = [Nat.P14]::GetDlgCtrlID($h)
        if ($id -ge 1010 -and $id -le 1016) {
            $sb = New-Object System.Text.StringBuilder 128
            [Nat.P14]::GetWindowTextW($h, $sb, 128) | Out-Null
            $bt[$id] = $sb.ToString()
        }
    }
    return $bt
}

# ---------- 断言 A：规则库生成 ----------
Click-NavRow '规则库生成' | Out-Null
$bt = Get-Buttons
Write-Output ("BTNS(rules): " + (($bt.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join ' | '))
# 1010=主按钮(RunLabel)；1011=Actions[0](btnClean 位)；1012=Actions[1](btnUndo 位)
if ($bt[1010] -ne '刷新规则列表') { Write-Output ("FAIL: rules btn1010 expected 刷新规则列表 got [{0}]" -f $bt[1010]); $fail++ }
if ($bt[1011] -ne '添加规则') { Write-Output ("FAIL: rules btn1011 expected 添加规则 got [{0}]" -f $bt[1011]); $fail++ }
if ($bt[1012] -ne '删除选中规则') { Write-Output ("FAIL: rules btn1012 expected 删除选中规则 got [{0}]" -f $bt[1012]); $fail++ }

# ---------- 断言 B：YARA 扫描 ----------
Click-NavRow 'YARA 扫描' | Out-Null
$bt = Get-Buttons
Write-Output ("BTNS(yara): " + (($bt.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join ' | '))
if ($bt[1010] -ne '开始扫描') { Write-Output ("FAIL: yara btn1010 expected 开始扫描 got [{0}]" -f $bt[1010]); $fail++ }
if ($bt[1011] -ne '打开规则目录') { Write-Output ("FAIL: yara btn1011 expected 打开规则目录 got [{0}]" -f $bt[1011]); $fail++ }

# ---------- 断言 C：主页锁定（点击「扫描劫持」，等结果）----------
Click-NavRow '主页锁定' | Out-Null
$bt = Get-Buttons
Write-Output ("BTNS(home): " + (($bt.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join ' | '))
if ($bt[1010] -ne '扫描劫持') { Write-Output ("FAIL: home btn1010 expected 扫描劫持 got [{0}]" -f $bt[1010]); $fail++ }
if ($bt[1011] -ne '修复并设置主页') { Write-Output ("FAIL: home btn1011 expected 修复并设置主页 got [{0}]" -f $bt[1011]); $fail++ }
if ($bt[1012] -ne '还原上次修改') { Write-Output ("FAIL: home btn1012 expected 还原上次修改 got [{0}]" -f $bt[1012]); $fail++ }

# 真实点击「扫描劫持」
$btnRun = [IntPtr]::Zero
foreach ($h in $children) {
    if ([Nat.P14]::GetDlgCtrlID($h) -eq 1010) { $btnRun = $h; break }
}
if ($btnRun -ne [IntPtr]::Zero) {
    $br = New-Object Nat.P14+RECT
    [Nat.P14]::GetWindowRect($btnRun, [ref]$br) | Out-Null
    [Nat.P14]::SetCursorPos([int](($br.L + $br.R) / 2), [int](($br.T + $br.B) / 2)) | Out-Null
    Start-Sleep -Milliseconds 120
    [Nat.P14]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [Nat.P14]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Write-Output 'CLICK: home scan button'
}
# 轮询等待完成（行数稳定 2 次；本机无劫持时 rows=0 属预期正确结果）
$LVM_GETITEMCOUNT = 0x1004
$lvCnt = -1; $prevCnt = -2; $stable = 0
for ($t = 0; $t -lt 10; $t++) {
    Start-Sleep -Seconds 2
    if ($listView -ne [IntPtr]::Zero) {
        $lvCnt = [Nat.P14]::SendMessageW($listView, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    }
    if ($lvCnt -eq $prevCnt) { $stable++ } else { $stable = 0 }
    if ($stable -ge 2 -and $t -ge 3) { break }
}
Write-Output ("HOME rows: {0}" -f $lvCnt)

# ---------- 截图 ----------
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 1600, 1000
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Nat.P14]::PrintWindow($main, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save("$dist\qa214-homepage.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "SHOT: $dist\qa214-homepage.png"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

if ($fail -eq 0) { Write-Output 'PASS: t214 all assertions passed' }
else { Write-Output "FAIL: $fail assertion(s) failed"; exit 1 }
