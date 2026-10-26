# 主题切换回归：点选 4 张卡片 → 采样窗口底色 → 断言换色 + 弹窗关闭 + 重启持久化
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WT {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$dir = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$shot = Join-Path $dir "screenshots"
# ini 请用 bash 先删：PowerShell 在沙箱里可能无权删除

function Shot($hwnd, $name) {
  [WT]::SetWindowPos($hwnd, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 400
  $rc = New-Object WT+RECT
  [WT]::GetClientRect($hwnd, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $h = $rc.Bottom - $rc.Top
  $pt = New-Object WT+POINT
  [WT]::ClientToScreen($hwnd, [ref]$pt) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [WT]::PrintWindow($hwnd, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [WT]::SetWindowPos($hwnd, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Write-Output "  saved $name"
}

function BgOf($path) {
  $bmp = [System.Drawing.Bitmap]::FromFile($path)
  # 采页头下方的留白区（卡片以外的底），四套主题的 bg 各不相同
  $px = $bmp.GetPixel(620, 110)
  $bmp.Dispose()
  return "$($px.R),$($px.G),$($px.B)"
}

$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
$btnTheme = [WT]::GetDlgItem($hwnd, 1018)

# 780x560 弹窗内容区 {24,70,756,466}：gap=16、top=rc.top+34 → 卡片 358x173
$cardPt = @{ 1 = @(200, 190); 2 = @(575, 190); 3 = @(200, 380); 4 = @(575, 380) }
$names  = @{ 1 = "daylight"; 2 = "warmpaper"; 3 = "mistblue"; 4 = "deepspace" }
$expect = @{ 1 = "245,246,249"; 2 = "246,241,231"; 3 = "237,241,247"; 4 = "10,12,17" }

Shot $hwnd "theme-1-daylight.png"
$bg0 = BgOf (Join-Path $shot 'theme-1-daylight.png')
Write-Output "$(if ($bg0 -eq $expect[1]) {'[OK]  '} else {'[FAIL]'}) start bg=$bg0 expect=$($expect[1])"

foreach ($n in 1..4) {
  [WT]::PostMessageW($hwnd, 0x0111, [IntPtr]1018, $btnTheme) | Out-Null
  Start-Sleep -Milliseconds 900
  $modal = [WT]::FindWindowW("MzModalHost", [IntPtr]::Zero)
  if ($modal -eq [IntPtr]::Zero) { Write-Output "[FAIL] picker did not open"; break }
  if ($n -eq 1) { Shot $modal "theme-6-picker.png" }
  $lx = $cardPt[$n][0]; $ly = $cardPt[$n][1]
  $lp = [IntPtr](($ly -shl 16) -bor ($lx -band 0xFFFF))
  [WT]::PostMessageW($modal, 0x0201, [IntPtr]1, $lp) | Out-Null
  Start-Sleep -Milliseconds 900
  $stillOpen = [WT]::FindWindowW("MzModalHost", [IntPtr]::Zero) -ne [IntPtr]::Zero
  $file = "theme-$($n + 1)-$($names[$n]).png"
  Shot $hwnd $file
  $bg = BgOf (Join-Path $shot $file)
  $ok = ($bg -eq $expect[$n]) -and (-not $stillOpen)
  Write-Output "$(if ($ok) {'[OK]  '} else {'[FAIL]'}) card $n ($($names[$n])): bg=$bg expect=$($expect[$n]) modalClosed=$(-not $stillOpen)"
}

# 重启 → 持久化（最后一次选的是 deepspace）
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
$p2 = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p2.Refresh()
$hwnd2 = $p2.MainWindowHandle
Shot $hwnd2 "theme-9-restart-deepspace.png"
$bg = BgOf (Join-Path $shot 'theme-9-restart-deepspace.png')
$ok = $bg -eq "10,12,17"
Write-Output "$(if ($ok) {'[OK]  '} else {'[FAIL]'}) restart persisted deepspace: bg=$bg expect=10,12,17"

Stop-Process -Id $p2.Id -Force -ErrorAction SilentlyContinue
Write-Output "DONE"
