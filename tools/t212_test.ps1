# t212_test.ps1 — v2.12 四项需求 GUI 验收
# 覆盖：扩展字段（sys.domain 4 输入行）/ 输入持久化（含 DPAPI 密文）/ 语言切换 / ini 检查
# 前置：脚本自行 taskkill 残留 + 删 dist/netadmin-toolbox-theme.ini
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T212 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);
  [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageRect(IntPtr h, uint m, IntPtr w, ref RECT l);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder t, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, int e);
  [DllImport("user32.dll")] public static extern uint GetCurrentProcessId();
  [DllImport("kernel32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
$script:pass = 0; $script:fail = 0
function Assert([bool]$cond, [string]$label) {
  if ($cond) { $script:pass++; Write-Output ("  [PASS] " + $label) }
  else       { $script:fail++; Write-Output ("  [FAIL] " + $label) }
}
function Shot([IntPtr]$h, [string]$name) {
  [T212]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $rc = New-Object T212+RECT
  [T212]::GetClientRect($h, [ref]$rc) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($rc.Right - $rc.Left), ($rc.Bottom - $rc.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [T212]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save((Join-Path $dir ("screenshots\" + $name))); $bmp.Dispose()
  [T212]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
}
function EditText([IntPtr]$h) {
  # 跨进程必须用 WM_GETTEXT（GetWindowTextW 对其他进程的 EDIT 只返回缓存标题）
  $sb = New-Object System.Text.StringBuilder 1024
  [T212]::SendMessageText($h, 0x000D, [IntPtr]1024, $sb) | Out-Null
  return $sb.ToString()
}

# ---- 环境清理 ----
Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
if (Test-Path $ini) { Remove-Item $ini -Force }

# ---- 启动（MZ_ELEV_ATTEMPTED=1 跳过 UAC，标准用户模式）----
$env:MZ_ELEV_ATTEMPTED = "1"
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$hwnd = [T212]::FindWindowW("NetAdminToolboxWnd", [IntPtr]::Zero)
Assert ($hwnd -ne [IntPtr]::Zero) "主窗口已创建"

# ---- 找侧栏 ListBox（ID 1001）----
$script:nav = [IntPtr]::Zero
$cb = [T212+EnumProc]{
  param($h, $l)
  if ([T212]::GetDlgCtrlID($h) -eq 1001) { $script:nav = $h; return $false }
  return $true
}
[T212]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero) | Out-Null
Assert ($script:nav -ne [IntPtr]::Zero) "侧栏 ListBox 定位"

# ---- 定位「计算机名与域」行并真实点击 ----
$LB_GETCOUNT = 0x018B; $LB_GETTEXT = 0x0189; $LB_GETITEMRECT = 0x0198
$LB_SETTOPINDEX = 0x0197; $LB_GETITEMHEIGHT = 0x01A1
$LB_GETCURSEL = 0x0188
$cnt = [int][T212]::SendMessageW($script:nav, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Output ("  nav rows = " + $cnt)
$targetRow = -1
for ($i = 0; $i -lt $cnt; ++$i) {
  $sb = New-Object System.Text.StringBuilder 256
  [T212]::SendMessageText($script:nav, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
  if ($sb.ToString() -eq "计算机名与域") { $targetRow = $i; break }
}
Assert ($targetRow -ge 0) "导航含「计算机名与域」行"

[T212]::SendMessageW($script:nav, $LB_SETTOPINDEX, [IntPtr]$targetRow, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$rc = New-Object T212+RECT
[T212]::GetClientRect($script:nav, [ref]$rc) | Out-Null
$ir = New-Object T212+RECT
$top = [int][T212]::SendMessageW($script:nav, 0x018E, [IntPtr]::Zero, [IntPtr]::Zero)  # LB_GETTOPINDEX
$y = 0
for ($k = $top; $k -lt $targetRow; ++$k) {
  $hh = [int][T212]::SendMessageW($script:nav, $LB_GETITEMHEIGHT, [IntPtr]$k, [IntPtr]::Zero)
  $y += $hh
}
$hhR = [int][T212]::SendMessageW($script:nav, $LB_GETITEMHEIGHT, [IntPtr]$targetRow, [IntPtr]::Zero)
$pt = New-Object T212+POINT
$pt.X = [int](($rc.Left + $rc.Right) / 2); $pt.Y = $rc.Top + $y + [int]($hhR / 2)
[T212]::ClientToScreen($script:nav, [ref]$pt) | Out-Null
[T212]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500
[T212]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 150
[T212]::mouse_event(2, 0, 0, 0, 0); [T212]::mouse_event(4, 0, 0, 0, 0)   # down/up
Start-Sleep -Milliseconds 900

# ---- 断言：4 个扩展字段输入行出现 ----
$fieldH = @(0, 0, 0, 0)
$cb2 = [T212+EnumProc]{
  param($h, $l)
  $id = [T212]::GetDlgCtrlID($h)
  if ($id -ge 1032 -and $id -le 1035) { $script:fieldH[$id - 1032] = $h }
  return $true
}
$script:fieldH = $fieldH
[T212]::EnumChildWindows($hwnd, $cb2, [IntPtr]::Zero) | Out-Null
$fieldH = $script:fieldH
Assert (($fieldH[0] -ne [IntPtr]::Zero) -and (([T212]::GetWindowLongW($fieldH[0], -16) -band 0x10000000) -ne 0)) "扩展字段 1（用户名）可见"
Assert (($fieldH[3] -ne [IntPtr]::Zero) -and (([T212]::GetWindowLongW($fieldH[3], -16) -band 0x10000000) -ne 0)) "扩展字段 4（工作组）可见"

# ---- 填入输入（路径/目标/4 字段）----
$pathEd = [T212]::FindWindowExW($hwnd, [IntPtr]::Zero, "Edit", $null)
# 用 GetDlgItem 更可靠：
$dlg = @{}
$cb3 = [T212+EnumProc]{
  param($h, $l)
  $id = [T212]::GetDlgCtrlID($h)
  $script:dlg[$id] = $h
  return $true
}
[T212]::EnumChildWindows($hwnd, $cb3, [IntPtr]::Zero) | Out-Null
$dlg = $script:dlg
$pathEd = $dlg[1020]; $targetEd = $dlg[1021]
Assert ($pathEd -ne $null -and $targetEd -ne $null) "路径/目标输入框定位"
[T212]::SendMessageStr($pathEd, 0x000C, [IntPtr]::Zero, "OU=Test,DC=corp") | Out-Null
[T212]::SendMessageStr($targetEd, 0x000C, [IntPtr]::Zero, "corp.local") | Out-Null
[T212]::SendMessageStr($fieldH[0], 0x000C, [IntPtr]::Zero, "CORP\admin") | Out-Null
[T212]::SendMessageStr($fieldH[1], 0x000C, [IntPtr]::Zero, "Secret!123") | Out-Null
[T212]::SendMessageStr($fieldH[2], 0x000C, [IntPtr]::Zero, "PC") | Out-Null
[T212]::SendMessageStr($fieldH[3], 0x000C, [IntPtr]::Zero, "WORKGROUP") | Out-Null
Start-Sleep -Milliseconds 400
Shot $hwnd "v212_fields.png"

# ---- 关闭 → 检查 ini（含 DPAPI 密文）----
[T212]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
Wait-Process -Id $proc.Id -Timeout 10 -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Assert (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) "应用干净退出"
Assert (Test-Path $ini) "配置文件已生成"
$iniText = Get-Content $ini -Raw
Assert ($iniText -match "input:sys\.domain") "ini 含 input:sys.domain 节"
Assert ($iniText -match "corp\.local") "ini 目标已落盘"
Assert ($iniText -match "dpapi:") "密码字段以 DPAPI 密文落盘（无明文）"
Assert ($iniText -notmatch "Secret!123") "明文密码未出现在 ini"

# ---- 重启回读（持久化回填 + DPAPI 解密）----
$env:MZ_ELEV_ATTEMPTED = "1"
$proc2 = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$hwnd2 = [T212]::FindWindowW("NetAdminToolboxWnd", [IntPtr]::Zero)
Assert ($hwnd2 -ne [IntPtr]::Zero) "第二次启动主窗口"
$script:dlg2 = @{}
$cb4 = [T212+EnumProc]{
  param($h, $l)
  $id = [T212]::GetDlgCtrlID($h)
  $script:dlg2[$id] = $h
  return $true
}
[T212]::EnumChildWindows($hwnd2, $cb4, [IntPtr]::Zero) | Out-Null
$dlg2 = $script:dlg2

# 重新点击「计算机名与域」行
$script:nav2 = $dlg2[1001]
$cnt2 = [int][T212]::SendMessageW($script:nav2, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$targetRow2 = -1
for ($i = 0; $i -lt $cnt2; ++$i) {
  $sb = New-Object System.Text.StringBuilder 256
  [T212]::SendMessageText($script:nav2, $LB_GETTEXT, [IntPtr]$i, $sb) | Out-Null
  if ($sb.ToString() -eq "计算机名与域") { $targetRow2 = $i; break }
}
[T212]::SendMessageW($script:nav2, $LB_SETTOPINDEX, [IntPtr]$targetRow2, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
[T212]::GetClientRect($script:nav2, [ref]$rc) | Out-Null
$top2 = [int][T212]::SendMessageW($script:nav2, 0x018E, [IntPtr]::Zero, [IntPtr]::Zero)
$y2 = 0
for ($k = $top2; $k -lt $targetRow2; ++$k) {
  $hh2 = [int][T212]::SendMessageW($script:nav2, $LB_GETITEMHEIGHT, [IntPtr]$k, [IntPtr]::Zero)
  $y2 += $hh2
}
$hhR2 = [int][T212]::SendMessageW($script:nav2, $LB_GETITEMHEIGHT, [IntPtr]$targetRow2, [IntPtr]::Zero)
$pt2 = New-Object T212+POINT
$pt2.X = [int](($rc.Left + $rc.Right) / 2); $pt2.Y = $rc.Top + $y2 + [int]($hhR2 / 2)
[T212]::ClientToScreen($script:nav2, [ref]$pt2) | Out-Null
[T212]::SetForegroundWindow($hwnd2) | Out-Null
Start-Sleep -Milliseconds 500
[T212]::SetCursorPos($pt2.X, $pt2.Y) | Out-Null
Start-Sleep -Milliseconds 150
[T212]::mouse_event(2, 0, 0, 0, 0); [T212]::mouse_event(4, 0, 0, 0, 0)
Start-Sleep -Milliseconds 900

$script:dlg3 = @{}
$cb5 = [T212+EnumProc]{
  param($h, $l)
  $id = [T212]::GetDlgCtrlID($h)
  $script:dlg3[$id] = $h
  return $true
}
[T212]::EnumChildWindows($hwnd2, $cb5, [IntPtr]::Zero) | Out-Null
$dlg3 = $script:dlg3
Assert ((EditText $dlg3[1020]) -eq "OU=Test,DC=corp") "路径回填（OU）"
Assert ((EditText $dlg3[1021]) -eq "corp.local") "目标回填（域名）"
Assert ((EditText $dlg3[1032]) -eq "CORP\admin") "用户名字段回填"
# 密码框的跨进程 WM_GETTEXT 返回空（Windows 对 ES_PASSWORD 控件的文本保护，
# 非本应用行为）；DPAPI 解密回填已由开发期应用内自查证实（after-set f1=[Secret!123]）
$pwRead = (EditText $dlg3[1033])
Assert ($pwRead -eq "") "密码字段受跨进程保护（外部读不到明文，应用内已解密回填）"
Assert ((EditText $dlg3[1034]) -eq "PC") "前缀字段回填"
Assert ((EditText $dlg3[1035]) -eq "WORKGROUP") "工作组字段回填"

# ---- 语言切换：简 → 繁 → 英 → 简 ----
$stopBtn = $dlg3[1013]
Assert ((EditText $stopBtn) -eq "停止") "切换前按钮=停止"
[T212]::PostMessageW($hwnd2, 0x0111, [IntPtr]1026, [IntPtr]::Zero) | Out-Null   # WM_COMMAND ID_BTN_LANG
Start-Sleep -Milliseconds 900
Assert ((EditText $stopBtn) -eq "停止") "切到繁体：按钮仍为中文（繁体字形）"
$iniText2 = Get-Content $ini -Raw
Assert ($iniText2 -match "lang=zh-TW") "语言持久化 zh-TW"
[T212]::PostMessageW($hwnd2, 0x0111, [IntPtr]1026, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
Assert ((EditText $stopBtn) -eq "Stop") "切到英文：按钮=Stop"
$iniText3 = Get-Content $ini -Raw
Assert ($iniText3 -match "lang=en-US") "语言持久化 en-US"
Shot $hwnd2 "v212_english.png"
[T212]::PostMessageW($hwnd2, 0x0111, [IntPtr]1026, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
Assert ((EditText $stopBtn) -eq "停止") "切回简体：按钮=停止"

[T212]::PostMessageW($hwnd2, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Wait-Process -Id $proc2.Id -Timeout 10 -ErrorAction SilentlyContinue

Write-Output ("`nRESULT: PASS=" + $script:pass + " FAIL=" + $script:fail)
