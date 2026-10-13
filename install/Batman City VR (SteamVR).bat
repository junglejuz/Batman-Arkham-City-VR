@echo off
REM Batman City - VR launcher through STEAMVR (2026-10-10). Lives next to BatmanAC.exe.
REM The same as "Batman City VR.bat", but this one game launch uses SteamVR instead of the PC's
REM normal VR runtime. Nothing on the PC is changed (AKN AKVR-Launch-VR-EngineStereo-SteamVR.bat).
REM Needs: Steam running, SteamVR installed, the headset connected to SteamVR.
REM The mod sees SteamVR and uses its standing (STAGE) space - SteamVR's seated space is upside down on some PCs.
setlocal
tasklist /FI "IMAGENAME eq BatmanAC.exe" 2>nul | %SystemRoot%\System32\find.exe /I "BatmanAC.exe" >nul
if not errorlevel 1 (
    echo Batman: Arkham City is already running. Close it, then start it again.
    pause
    exit /b 1
)
REM SteamVR's 32-bit OpenXR runtime (Arkham City is a 32-bit game): steamxr_win32.json in the SteamVR folder.
set "SVRJSON="
set "STEAMDIR="
REM First the SteamVR install SteamVR itself treats as active (its openvrpaths.vrpath "runtime" entry) - 2026-10-14, a
REM player with two SteamVR installs; then Steam's own folder, then the default one.
set "SVRDIR="
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "try { (Get-Content -Raw (Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath') | ConvertFrom-Json).runtime[0] } catch { }"`) do set "SVRDIR=%%p"
if defined SVRDIR if exist "%SVRDIR%\steamxr_win32.json" set "SVRJSON=%SVRDIR%\steamxr_win32.json"
for /f "tokens=2,*" %%a in ('reg query "HKCU\SOFTWARE\Valve\Steam" /v SteamPath 2^>nul ^| %SystemRoot%\System32\find.exe /I "SteamPath"') do set "STEAMDIR=%%b"
if defined STEAMDIR set "STEAMDIR=%STEAMDIR:/=\%"
if not defined SVRJSON if defined STEAMDIR if exist "%STEAMDIR%\steamapps\common\SteamVR\steamxr_win32.json" set "SVRJSON=%STEAMDIR%\steamapps\common\SteamVR\steamxr_win32.json"
if not defined SVRJSON if exist "%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\steamxr_win32.json" set "SVRJSON=%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\steamxr_win32.json"
if not defined SVRJSON (
    echo SteamVR's 32-bit OpenXR runtime was not found. Install or update SteamVR from Steam, start it once, then try again.
    pause
    exit /b 1
)
set "RX=2064"
set "RY=2208"
set "INI=%LOCALAPPDATA%\Batman_City\launch.ini"
if exist "%INI%" (
  for /f "tokens=1,2 delims==" %%A in ('findstr /b /i "ResX= ResY=" "%INI%"') do (
    if /i "%%A"=="ResX" set "RX=%%B"
    if /i "%%A"=="ResY" set "RY=%%B"
  )
)
cd /d "%~dp0"
set "XR_RUNTIME_JSON=%SVRJSON%"
start "" BatmanAC.exe -windowed ResX=%RX% ResY=%RY%
