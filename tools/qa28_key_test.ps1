# qa28_key_test.ps1 — 诊断 Ctrl+T / Ctrl+= 是否生效（以 ini 为准）
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class K28 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$env:MZ_ELEV_ATTEMPTED = "1"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
$hwnd = $p.MainWindowHandle
Write-Output "hwnd: $hwnd"

function IniDump([string]$tag) {
  foreach ($f in (Get-ChildItem (Join-Path $dir "dist") -Filter "netadmin-toolbox-*.ini")) {
    $txt = Get-Content $f.FullName -Raw
    Write-Output "  [$tag] $($f.Name): $($txt -replace "\r?\n", ' | ')"
  }
}
IniDump "launch"

[K28]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 800
[K28]::keybd_event(0x11,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0x54,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0x54,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 1200
IniDump "after Ctrl+T x1"

[K28]::keybd_event(0x11,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0xBB,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0xBB,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 90
[K28]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 1200
IniDump "after Ctrl+= x1"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
