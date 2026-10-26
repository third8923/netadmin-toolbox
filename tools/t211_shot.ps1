# t211_shot.ps1 - v2.11 截图：主界面侧栏（密码工具组）
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Nat -Name Shot -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
[DllImport("user32.dll")]
public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
public struct RECT { public int L, T, R, B; }
'@

Get-Process netadmin-toolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\*.ini" -ErrorAction SilentlyContinue

$exe = 'D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\dist\netadmin-toolbox.exe'
$env:MZ_ELEV_ATTEMPTED = '1'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output 'FAIL: no window'; exit 1 }

$r = New-Object Nat.Shot+RECT
[Nat.Shot]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Nat.Shot]::PrintWindow($h, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$out = 'D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0\screenshots\v211_main.png'
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "SAVED $out ${w}x${ht}"
Stop-Process -Id $p.Id -Force
