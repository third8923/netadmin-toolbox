# T05 发版自查：net.sharemap CLI 抽查
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools\t05_sharemap_cli.ps1
$dir = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $dir "dist\netadmin-toolbox.exe"
$out = Join-Path $dir "build\t05_sharemap.txt"
$err = Join-Path $dir "build\t05_sharemap_err.txt"
Remove-Item Env:http_proxy -ErrorAction SilentlyContinue
Remove-Item Env:HTTP_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:https_proxy -ErrorAction SilentlyContinue
Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:all_proxy -ErrorAction SilentlyContinue
Remove-Item Env:ALL_PROXY -ErrorAction SilentlyContinue
$p = Start-Process $exe -ArgumentList "run","net.sharemap" -WorkingDirectory (Join-Path $dir "dist") `
    -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -Wait -PassThru
Write-Output "exit=$($p.ExitCode)"
$lines = Get-Content $out -Encoding UTF8
Write-Output "output lines: $($lines.Count)"
$lines | ForEach-Object { Write-Output $_ }
