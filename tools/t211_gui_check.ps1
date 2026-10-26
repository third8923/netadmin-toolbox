# t211_gui_check.ps1 - v2.11 GUI 冒烟：侧栏应出现「密码工具」4 个模块
$ErrorActionPreference = 'Stop'

# 清场：残留进程 + 旧 ini（保证起点干净）
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\*.ini" -ErrorAction SilentlyContinue

Add-Type -Namespace Nat -Name P11 -MemberDefinition @'
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
public static extern IntPtr FindWindowW(string cls, string title);
'@

$exe = 'D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\netadmin-toolbox.exe'
$env:MZ_ELEV_ATTEMPTED = '1'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
if ($p.HasExited) { Write-Output 'FAIL: app exited immediately'; exit 1 }

$main = $p.MainWindowHandle
if ($main -eq [IntPtr]::Zero) { Write-Output 'FAIL: no main window'; Stop-Process -Id $p.Id -Force; exit 1 }

# 找 ListBox（控件 ID 1001）
$listBox = [IntPtr]::Zero
$cb = [Nat.P11+EnumProc]{
    param($h, $lp)
    if ([Nat.P11]::GetDlgCtrlID($h) -eq 1001) { $script:listBox = $h; return $false }
    return $true
}
[Nat.P11]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null
if ($listBox -eq [IntPtr]::Zero) { Write-Output 'FAIL: listbox not found'; Stop-Process -Id $p.Id -Force; exit 1 }

$LB_GETCOUNT = 0x018B
$LB_GETTEXT = 0x0189
$cnt = [Nat.P11]::SendMessageW($listBox, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
$items = @()
for ($i = 0; $i -lt $cnt; $i++) {
    $sb = New-Object System.Text.StringBuilder 256
    [Nat.P11]::SendMessageW($listBox, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
    $items += $sb.ToString()
}
Write-Output ("ITEMS({0}):" -f $cnt)
$items | ForEach-Object { Write-Output ("  " + $_) }

$targets = @('星号密码查看器', 'ZIP 密码恢复', '凭据管理器', 'WiFi 密码查看')
$ok = $true
foreach ($t in $targets) {
    if ($items -notcontains $t) { Write-Output "FAIL: missing item [$t]"; $ok = $false }
}
if ($ok) { Write-Output 'PASS: password tools group visible' }

Stop-Process -Id $p.Id -Force
if ($ok) { exit 0 } else { exit 2 }
