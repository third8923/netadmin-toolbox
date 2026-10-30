# 真实点击切换模块独立截图：点击前/后两张，MD5 必然不同且页头随之变化
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CK {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
$ini = Join-Path $dir "dist\netadmin-toolbox-theme.ini"
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200
if (Test-Path $ini) { Remove-Item $ini -Force }
$env:MZ_ELEV_ATTEMPTED = "1"

function Shot($h, $name) {
  $r = New-Object CK+RECT
  [CK]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [CK]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  Write-Host "  saved $name (${w}x${ht})"
}

$p = Start-Process (Join-Path $dir "dist\netadmin-toolbox.exe") -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 15; $i++) {
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $c = $p.MainWindowHandle
  if ($c -ne [IntPtr]::Zero -and [CK]::GetDlgItem($c, 1001) -ne [IntPtr]::Zero) { $hwnd = $c; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "NO WINDOW"; exit 1 }
[CK]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 900

# 点击前：默认首模块（基础清理）
Shot $hwnd "vi-click-before.png"

# LB_GETITEMRECT 定位第 12 行（磁盘信息，位于「系统信息」组），真实鼠标点击
$nav = [CK]::GetDlgItem($hwnd, 1001)
$ir = New-Object CK+RECT
$ptrI = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(16)
[CK]::SendMessageW($nav, 0x0198, [IntPtr]12, $ptrI) | Out-Null   # LB_GETITEMRECT
$ir = [System.Runtime.InteropServices.Marshal]::PtrToStructure($ptrI, [type][CK+RECT])
[System.Runtime.InteropServices.Marshal]::FreeHGlobal($ptrI)
$nr = New-Object CK+RECT
[CK]::GetWindowRect($nav, [ref]$nr) | Out-Null
$cx = $nr.Left + (($ir.Right - $ir.Left) / 2)
$cy = $nr.Top + $ir.Top + (($ir.Bottom - $ir.Top) / 2)
Write-Host ("click target: item12 screen ({0},{1})" -f $cx, $cy)
[CK]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 300
[CK]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60
[CK]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 1500

# 点击后：应为「磁盘信息」页头
Shot $hwnd "vi-click-switch.png"
Get-Process -Id $p.Id -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $ini -Force -ErrorAction SilentlyContinue
Write-Host "DONE"
