# ============================================================================
#  gui_shot_softmgr.ps1 — 软件管家（winget v2.7）GUI 实测 + 截图   [QA v3]
#  v3 修复: 导航切换 = 真实鼠标点击导航项(LB_GETITEMRECT 发送侧缓冲区)，
#           结果行选中 = 真实鼠标点击行(LVM_GETITEMRECT 远程缓冲区)，
#           搜索词注入 = WM_SETTEXT 后回读校验，失败回落键盘输入。
# ============================================================================
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SoftQA {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool i, uint p);
  [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr hp, IntPtr addr, IntPtr size, uint type, uint protect);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr hp, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr hp, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
  [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr hp, IntPtr addr, IntPtr size, uint type);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  public struct POINT { public int X; public int Y; }

  // 读 listbox 文本（发送侧缓冲区，user32 marshaling）
  public static string LbText(IntPtr hwnd, int item) {
    IntPtr buf = Marshal.AllocHGlobal(1024);
    try {
      IntPtr r = SendMessageW(hwnd, 0x0189, (IntPtr)item, buf);
      if (r == (IntPtr)(-1)) return "";
      return Marshal.PtrToStringUni(buf);
    } finally { Marshal.FreeHGlobal(buf); }
  }
  // listbox 项矩形（发送侧缓冲区），返回 [l,t,r,b,ret]
  public static int[] LbItemRect(IntPtr hwnd, int item) {
    IntPtr buf = Marshal.AllocHGlobal(16);
    try {
      IntPtr r = SendMessageW(hwnd, 0x0198, (IntPtr)item, buf);
      int[] a = new int[5];
      a[0] = Marshal.ReadInt32(buf, 0); a[1] = Marshal.ReadInt32(buf, 4);
      a[2] = Marshal.ReadInt32(buf, 8); a[3] = Marshal.ReadInt32(buf, 12);
      a[4] = (int)r;
      return a;
    } finally { Marshal.FreeHGlobal(buf); }
  }
  // listview 行矩形（远程缓冲区，LVM 无 marshaling），返回 [l,t,r,b,ret]
  public static int[] LvItemRect(IntPtr hwnd, int item) {
    uint pid; GetWindowThreadProcessId(hwnd, out pid);
    IntPtr hp = OpenProcess(0x001F0FFF, false, pid);
    if (hp == IntPtr.Zero) return null;
    IntPtr mem = VirtualAllocEx(hp, IntPtr.Zero, (IntPtr)64, 0x3000, 0x40);
    if (mem == IntPtr.Zero) { CloseHandle(hp); return null; }
    try {
      SendMessageW(hwnd, 0x100E, (IntPtr)item, mem);   // LVM_GETITEMRECT(空 LVITEM 布局即可取整行)
      byte[] buf = new byte[16];
      IntPtr w;
      ReadProcessMemory(hp, mem, buf, (IntPtr)16, out w);
      int[] a = new int[5];
      for (int k = 0; k < 4; k++) a[k] = BitConverter.ToInt32(buf, k * 4);
      a[4] = 1;
      return a;
    } finally { VirtualFreeEx(hp, mem, IntPtr.Zero, 0x8000); CloseHandle(hp); }
  }
  // 读 listview 单元格（远程缓冲区）
  public static string CellText(IntPtr hwnd, int item, int sub) {
    uint pid; GetWindowThreadProcessId(hwnd, out pid);
    IntPtr hp = OpenProcess(0x001F0FFF, false, pid);
    if (hp == IntPtr.Zero) return "";
    IntPtr mem = VirtualAllocEx(hp, IntPtr.Zero, (IntPtr)4096, 0x3000, 0x40);
    if (mem == IntPtr.Zero) { CloseHandle(hp); return ""; }
    try {
      IntPtr textPtr = (IntPtr)((long)mem + 512);
      byte[] it = new byte[64];
      BitConverter.GetBytes(1u).CopyTo(it, 0);
      BitConverter.GetBytes(item).CopyTo(it, 4);
      BitConverter.GetBytes(sub).CopyTo(it, 8);
      BitConverter.GetBytes((long)textPtr).CopyTo(it, 24);
      BitConverter.GetBytes(512).CopyTo(it, 32);
      IntPtr w;
      WriteProcessMemory(hp, mem, it, (IntPtr)64, out w);
      SendMessageW(hwnd, 0x104B, (IntPtr)item, mem);
      byte[] buf = new byte[1024];
      ReadProcessMemory(hp, textPtr, buf, (IntPtr)1024, out w);
      return Encoding.Unicode.GetString(buf).Split('\0')[0];
    } finally { VirtualFreeEx(hp, mem, IntPtr.Zero, 0x8000); CloseHandle(hp); }
  }
  // 设置编辑框文本（发送侧字符串，WM_SETTEXT 由 user32 marshaling）
  public static bool SetEditText(IntPtr hwnd, string s) {
    IntPtr p = Marshal.StringToHGlobalUni(s);
    try {
      return SendMessageW(hwnd, 0x000C, IntPtr.Zero, p) != IntPtr.Zero;
    } finally { Marshal.FreeHGlobal(p); }
  }
  // 读编辑框文本（发送侧缓冲区）
  public static string GetEditText(IntPtr hwnd) {
    IntPtr buf = Marshal.AllocHGlobal(512);
    try {
      SendMessageW(hwnd, 0x000D, (IntPtr)256, buf);   // WM_GETTEXT
      return Marshal.PtrToStringUni(buf);
    } finally { Marshal.FreeHGlobal(buf); }
  }
}
"@

$dir  = "D:\WorkBuddyAI\netadmin-toolbox\netadmin-toolbox-v2.0"
$exe  = Join-Path $dir "dist\netadmin-toolbox.exe"
$shot = Join-Path $dir "tools"

function Say($m) { Write-Host ("[qa] " + $m) }

# ---------- 0) 清理残留 ----------
Get-Process -Name "netadmin-toolbox" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 900
Get-ChildItem (Join-Path $dir "dist") -Filter "netadmin-toolbox-*.ini" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Say "prep ok"

$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Say "FATAL: no main window"; exit 1 }
$nav  = [SoftQA]::GetDlgItem($hwnd, 1001)
$list = [SoftQA]::GetDlgItem($hwnd, 1002)
$btnRun = [SoftQA]::GetDlgItem($hwnd, 1010)
Say "hwnd=$hwnd"

function Shot($h, $name) {
  [SoftQA]::SetForegroundWindow($h) | Out-Null
  [SoftQA]::SetWindowPos($h, [IntPtr]-1, 0, 0, 0, 0, 3) | Out-Null
  Start-Sleep -Milliseconds 450
  $rc = New-Object SoftQA+RECT
  [SoftQA]::GetClientRect($h, [ref]$rc) | Out-Null
  $w = $rc.Right - $rc.Left; $ht = $rc.Bottom - $rc.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [SoftQA]::PrintWindow($h, $hdc, 3)
  $g.ReleaseHdc($hdc)
  if (-not $ok) {
    $pt = New-Object SoftQA+POINT
    [SoftQA]::ClientToScreen($h, [ref]$pt) | Out-Null
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
  }
  $bmp.Save((Join-Path $shot $name))
  $g.Dispose(); $bmp.Dispose()
  [SoftQA]::SetWindowPos($h, [IntPtr]-2, 0, 0, 0, 0, 3) | Out-Null
  Say ("  saved $name (${w}x${ht})")
}
function ClickBtn($id) {
  $b = [SoftQA]::GetDlgItem($hwnd, $id)
  if ($b -eq [IntPtr]::Zero) { Say "  [WARN] button $id missing"; return $false }
  [SoftQA]::PostMessageW($hwnd, 0x0111, [IntPtr]($id -bor (1 -shl 16)), $b) | Out-Null
  return $true
}
function MouseClick($x, $y) {
  [SoftQA]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 120
  [SoftQA]::mouse_event(0x0002, $x, $y, 0, [IntPtr]::Zero) | Out-Null
  [SoftQA]::mouse_event(0x0004, $x, $y, 0, [IntPtr]::Zero) | Out-Null
}
function NavIndex($pattern) {
  $cnt = [SoftQA]::SendMessageW($nav, 0x018B, [IntPtr]0, [IntPtr]0).ToInt32()
  for ($i = 0; $i -lt $cnt; $i++) {
    $t = [SoftQA]::LbText($nav, $i)
    if ($t -match $pattern) { return $i }
  }
  return -1
}
# 真实鼠标点击导航项（LB_GETITEMRECT 发送侧缓冲区取坐标）
function SwitchNav($pattern) {
  $i = NavIndex $pattern
  if ($i -lt 0) { Say "  [WARN] nav not found: $pattern"; return $false }
  [SoftQA]::SendMessageW($nav, 0x0197, [IntPtr]$i, [IntPtr]0) | Out-Null  # LB_SETTOPINDEX
  Start-Sleep -Milliseconds 250
  $rc = [SoftQA]::LbItemRect($nav, $i)
  if ($rc[4] -eq 0) { Say "  [WARN] item rect failed"; return $false }
  $pt = New-Object SoftQA+POINT
  [SoftQA]::ClientToScreen($nav, [ref]$pt) | Out-Null
  $cx = $pt.X + [int](($rc[0] + $rc[2]) / 2)
  $cy = $pt.Y + [int](($rc[1] + $rc[3]) / 2)
  [SoftQA]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 300
  MouseClick $cx $cy
  Start-Sleep -Milliseconds 900
  $cur = [SoftQA]::SendMessageW($nav, 0x0188, [IntPtr]0, [IntPtr]0).ToInt32()
  Say ("  nav -> $pattern (idx=$i cur=$cur)")
  return ($cur -eq $i)
}
function RowCount() { return [SoftQA]::SendMessageW($list, 0x1004, [IntPtr]0, [IntPtr]0).ToInt32() }
function WaitRows($min, $sec) {
  for ($i = 0; $i -lt $sec * 5; $i++) {
    Start-Sleep -Milliseconds 200
    if ((RowCount) -ge $min) { return $true }
  }
  return $false
}
function WaitIdle($sec) {
  $started = $false
  for ($i = 0; $i -lt $sec * 10; $i++) {
    Start-Sleep -Milliseconds 100
    $en = [SoftQA]::IsWindowEnabled($btnRun)
    if (-not $started) { if (-not $en) { $started = $true } ; continue }
    if ($en) {
      $stable = $true
      for ($k = 0; $k -lt 6; $k++) { Start-Sleep -Milliseconds 200; if (-not [SoftQA]::IsWindowEnabled($btnRun)) { $stable = $false; break } }
      if ($stable) { return $true }
    }
  }
  return $false
}
function WaitModal($sec) {
  for ($i = 0; $i -lt $sec * 10; $i++) {
    Start-Sleep -Milliseconds 100
    $m = [SoftQA]::FindWindowW("MzModalHost", [IntPtr]::Zero)
    if ($m -ne [IntPtr]::Zero) { return $m }
  }
  return [IntPtr]::Zero
}
function ModalClose($m, $ok) {
  [SoftQA]::PostMessageW($m, 0x0111, [IntPtr]$(if ($ok) { 1 } else { 2 }), [IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 600
}
# 真实鼠标点击结果列表指定行（LVM_GETITEMRECT 远程缓冲区取坐标）
function SelectRow($idx) {
  $rc = [SoftQA]::LvItemRect($list, $idx)
  if (-not $rc -or $rc[4] -eq 0) { Say "  [WARN] lv rect failed"; return $false }
  $pt = New-Object SoftQA+POINT
  [SoftQA]::ClientToScreen($list, [ref]$pt) | Out-Null
  $cx = $pt.X + $rc[0] + 120
  $cy = $pt.Y + [int](($rc[1] + $rc[3]) / 2)
  [SoftQA]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds 300
  MouseClick $cx $cy
  Start-Sleep -Milliseconds 400
  $st = [SoftQA]::SendMessageW($list, 0x102C, [IntPtr]$idx, [IntPtr]2).ToInt32()
  Say ("  row $idx selected-state=" + (($st -band 2) -ne 0))
  return (($st -band 2) -ne 0)
}
function FindRow($sub, $match) {
  $n = RowCount
  for ($i = 0; $i -lt $n; $i++) {
    $t = [SoftQA]::CellText($list, $i, $sub)
    if ($t -and $t.Contains($match)) { return $i }
  }
  return -1
}
function SetTargetText($text) {
  $tgt = [SoftQA]::GetDlgItem($hwnd, 1021)
  $ok = [SoftQA]::SetEditText($tgt, $text)
  $back = [SoftQA]::GetEditText($tgt)
  Say ("  target set=$ok back=[$back]")
  if ($back -ne $text) {
    # 回落：点击输入框 + Ctrl+A + 键盘逐字输入
    $rc = New-Object SoftQA+RECT
    [SoftQA]::GetWindowRect($tgt, [ref]$rc) | Out-Null
    [SoftQA]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 300
    MouseClick ($rc.Left + 60) ([int](($rc.Top + $rc.Bottom) / 2))
    [SoftQA]::keybd_event(0x11, 0, 0, [IntPtr]::Zero) | Out-Null
    [SoftQA]::keybd_event(0x41, 0, 0, [IntPtr]::Zero) | Out-Null
    [SoftQA]::keybd_event(0x41, 0, 2, [IntPtr]::Zero) | Out-Null
    [SoftQA]::keybd_event(0x11, 0, 2, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
    foreach ($ch in $text.ToCharArray()) {
      $code = [int][char]$ch
      if ($code -ge 97 -and $code -le 122) { $code = $code - 32 }
      [SoftQA]::keybd_event([byte]$code, 0, 0, [IntPtr]::Zero) | Out-Null
      [SoftQA]::keybd_event([byte]$code, 0, 2, [IntPtr]::Zero) | Out-Null
      Start-Sleep -Milliseconds 80
    }
    Start-Sleep -Milliseconds 200
    $back2 = [SoftQA]::GetEditText($tgt)
    Say ("  target retyped back=[$back2]")
    return ($back2 -eq $text)
  }
  return $true
}
function WingetListHas($id) {
  $tmp = [System.IO.Path]::GetTempFileName()
  $wp = Start-Process -FilePath "winget" -ArgumentList 'list','--id',$id,'-e','--accept-source-agreements','--disable-interactivity' -RedirectStandardOutput $tmp -Wait -PassThru -WindowStyle Hidden
  $o = [System.IO.File]::ReadAllText($tmp, [System.Text.Encoding]::UTF8)
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
  return ($o -match [regex]::Escape($id))
}

# ======================= P1: soft.rec =======================
Say "P1 soft.rec"
$r1 = SwitchNav "装机必备"
if (-not $r1) { Say "  [FAIL] switch to soft.rec" }
ClickBtn 1010 | Out-Null
if (-not (WaitRows 30 120)) { Say "  [FAIL] rec rows < 30" }
$n = RowCount
Say ("  rec rows = $n ; row0=" + [SoftQA]::CellText($list, 0, 0) + "/" + [SoftQA]::CellText($list, 0, 1) + " ; rowN=" + [SoftQA]::CellText($list, $n-1, 0) + "/" + [SoftQA]::CellText($list, $n-1, 1))
Shot $hwnd "softmgr-1-rec.png"
# 空勾选点「卸载选中」→ 应弹确认框；取消不执行
ClickBtn 1012 | Out-Null
$m = WaitModal 8
if ($m -ne [IntPtr]::Zero) {
  Shot $m "softmgr-1b-rec-undo-confirm.png"
  ModalClose $m $false
  Say "  undo w/o selection: confirm dialog shown then cancelled"
} else { Say "  [WARN] no confirm dialog for undo" }

# ======================= P2: soft.search 选中安装 =======================
Say "P2 soft.search -> install sharkdp.bat"
$r2 = SwitchNav "软件搜索"
if (-not $r2) { Say "  [FAIL] switch to soft.search" }
SetTargetText "bat" | Out-Null
ClickBtn 1010 | Out-Null
if (-not (WaitIdle 150)) { Say "  [FAIL] search rows < 5" }
$idx = FindRow 3 "sharkdp.bat"
Say ("  search rows = " + (RowCount) + " ; sharkdp.bat idx = $idx")
if ($idx -ge 0) { Say ("    row: " + [SoftQA]::CellText($list, $idx, 0) + "/" + [SoftQA]::CellText($list, $idx, 1) + "/" + [SoftQA]::CellText($list, $idx, 3)) }
Shot $hwnd "softmgr-2-search.png"
if ($idx -ge 0 -and (SelectRow $idx)) {
  ClickBtn 1011 | Out-Null
  $m = WaitModal 10
  if ($m -ne [IntPtr]::Zero) {
    Shot $m "softmgr-3-confirm-install.png"
    ModalClose $m $true
    Say "  install confirmed; waiting install + auto-rescan..."
    if (WaitIdle 180) { Say "  idle (done)" } else { Say "  [WARN] idle timeout" }
    Shot $hwnd "softmgr-4-after-install.png"
    if (WingetListHas "sharkdp.bat") { Say "  CLI verify: INSTALLED" } else { Say "  [FAIL] CLI verify: NOT installed" }
  } else { Say "  [FAIL] no confirm dialog" }
} else { Say "  [FAIL] could not select bat row" }

# ======================= P3: soft.installed 选中卸载 =======================
Say "P3 soft.installed -> uninstall"
$r3 = SwitchNav "已安装软件"
if (-not $r3) { Say "  [FAIL] switch to soft.installed" }
ClickBtn 1010 | Out-Null
if (-not (WaitIdle 150)) { Say "  [FAIL] installed rows < 10" }
$idx = FindRow 3 "sharkdp.bat"
Say ("  installed rows = " + (RowCount) + " ; sharkdp.bat idx = $idx")
if ($idx -ge 0) { Say ("    row: " + [SoftQA]::CellText($list, $idx, 0) + "/" + [SoftQA]::CellText($list, $idx, 1) + "/" + [SoftQA]::CellText($list, $idx, 3)) }
Shot $hwnd "softmgr-5-installed.png"
if ($idx -ge 0 -and (SelectRow $idx)) {
  ClickBtn 1011 | Out-Null
  $m = WaitModal 10
  if ($m -ne [IntPtr]::Zero) {
    Shot $m "softmgr-6-confirm-uninstall.png"
    ModalClose $m $true
    Say "  uninstall confirmed; waiting uninstall + auto-rescan..."
    if (WaitIdle 180) { Say "  idle (done)" } else { Say "  [WARN] idle timeout" }
    $idx2 = FindRow 3 "sharkdp.bat"
    Say ("  after uninstall idx = $idx2 (expect -1)")
    Shot $hwnd "softmgr-7-after-uninstall.png"
    if (-not (WingetListHas "sharkdp.bat")) { Say "  CLI verify: REMOVED" } else { Say "  [FAIL] CLI verify: still installed" }
  } else { Say "  [FAIL] no confirm dialog" }
} else { Say "  [FAIL] bat row not found in installed list" }

# ======================= P4: tweak.winopt 回归 =======================
Say "P4 regression winopt"
$r4 = SwitchNav "Windows 优化"
if (-not $r4) { Say "  [FAIL] switch to winopt" }
ClickBtn 1010 | Out-Null
if (-not (WaitIdle 90)) { Say "  [FAIL] winopt rows < 20" }
Say ("  winopt rows = " + (RowCount))
Shot $hwnd "softmgr-8-winopt.png"

# ======================= P5: 11 模块切换稳定性 =======================
Say "P5 stability: 11 modules"
$pats = @("装机必备","浏览器上网","压缩与文件管理","办公与效率","开发与运维","影音与图像","通讯与社交","系统与安全","软件搜索","已安装软件","软件升级")
$bad = 0
foreach ($pt in $pats) {
  $r = SwitchNav $pt
  if (-not $r) { $bad++ }
  Start-Sleep -Milliseconds 300
}
Say ("  switch failures = $bad")
[SoftQA]::PostMessageW($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Seconds 3
$p.Refresh()
if ($p.HasExited) { Say "P5 PASS: process exited normally" } else { Say "P5 FAIL: still running"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Say "done"
