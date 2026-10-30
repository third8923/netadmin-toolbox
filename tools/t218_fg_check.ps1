# 启动 netadmin-toolbox 并验证主窗是否自动成为前台窗口（v2.18 启动激活修复验证）
$ErrorActionPreference = "Stop"
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"

Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process "$dir\dist\netadmin-toolbox.exe" -PassThru
Start-Sleep -Seconds 5
$p.Refresh()
$hwnd = $p.MainWindowHandle

$src = @'
using System;
using System.Runtime.InteropServices;
public static class FU {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
$tp = Add-Type -TypeDefinition $src -PassThru
$fgReal = [FU]::GetForegroundWindow()

Write-Output ("MainWindowHandle: 0x{0:X}" -f $hwnd.ToInt64())
Write-Output ("ForegroundWindow: 0x{0:X}" -f $fgReal.ToInt64())
Write-Output ("IsForeground: {0}" -f ($hwnd -eq $fgReal))

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
