# v2.18 图标+前台修复 GUI 截图验证
$ErrorActionPreference = "Stop"
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$out = "$dir\screenshots"

Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$env:MZ_ELEV_ATTEMPTED = "1"
Start-Process "$dir\dist\netadmin-toolbox.exe"
Start-Sleep -Seconds 5

$p = Get-Process -Name "netadmin-toolbox" | Select-Object -First 1
$hwnd = $p.MainWindowHandle

$src = @'
using System;
using System.Runtime.InteropServices;
public static class IC {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
Add-Type -TypeDefinition $src | Out-Null
Add-Type -AssemblyName System.Drawing

[IC]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 800
$bmp = New-Object System.Drawing.Bitmap 1500, 1020
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[IC]::PrintWindow($hwnd, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$bmp.Save("$out\v218-icon-fix.png")
$g.Dispose(); $bmp.Dispose()
Write-Output "saved v218-icon-fix.png"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
