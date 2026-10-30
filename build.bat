@echo off
setlocal
chcp 437 >nul

rem ============================================================
rem  NetAdmin Toolbox - build script
rem  Usage: build.bat [clean]
rem ============================================================

set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "BUILD=%ROOT%build"
set "DIST=%ROOT%dist"
set "OUT=netadmin-toolbox.exe"

set "MINGW=C:\Users\bhfadmin\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
set "GPP=%MINGW%\g++.exe"

if not exist "%GPP%" (
  echo [ERROR] MinGW not found: %GPP%
  echo Edit the MINGW variable in this script.
  exit /b 1
)

if /i "%1"=="clean" (
  if exist "%BUILD%" rmdir /s /q "%BUILD%"
  if exist "%DIST%" rmdir /s /q "%DIST%"
  echo Cleaned.
)

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%DIST%" mkdir "%DIST%"

echo [1/4] Compiling resources
"%MINGW%\windres.exe" -i "%SRC%\app.rc" -o "%BUILD%\app_res.o" -I"%SRC%"
if errorlevel 1 goto fail

echo [2/4] Collecting sources
set "FILES=%SRC%\main.cpp %SRC%\common.cpp %SRC%\lang.cpp %SRC%\procrun.cpp %SRC%\qr.cpp %SRC%\cleanresult.cpp %SRC%\regengine.cpp %SRC%\cleaner.cpp %SRC%\debloat.cpp %SRC%\winapp2.cpp %SRC%\simhash.cpp %SRC%\ruleset.cpp %SRC%\purgcore.cpp %SRC%\yara.cpp %SRC%\module.cpp %SRC%\cli.cpp"
for %%f in ("%SRC%\ui\*.cpp") do call set "FILES=%%FILES%% %%f"
for %%f in ("%SRC%\modules\*.cpp") do call set "FILES=%%FILES%% %%f"

echo [3/4] Compiling and linking
"%GPP%" -std=c++20 -O2 -s -w -DUNICODE -D_UNICODE -municode -mwindows -I"%SRC%" -I"%ROOT%data" -o "%DIST%\%OUT%" %FILES% "%BUILD%\app_res.o" -static -static-libgcc -static-libstdc++ -lcomctl32 -ladvapi32 -lshell32 -lshlwapi -lcrypt32 -lole32 -loleaut32 -ltaskschd -lnetapi32 -lwevtapi -lpowrprof -luuid -luser32 -lgdi32 -lgdiplus -lwindowscodecs -lpsapi -lws2_32 -liphlpapi -lwlanapi -ldnsapi -licmp -ldwmapi -luxtheme -lmpr -lwintrust -lwinhttp
if errorlevel 1 goto fail

echo [4/4] Verifying output
if not exist "%DIST%\%OUT%" goto fail

echo.
echo ============================================
echo   BUILD SUCCESS
echo ============================================
echo   Output: %DIST%\%OUT%
exit /b 0

:fail
echo.
echo [FAILED] Build error.
exit /b 1
