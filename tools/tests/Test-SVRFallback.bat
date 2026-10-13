@echo off
REM SVRFALLBACK test (2026-10-13): starts Arkham City the way a Pimax PC does - the only runtime it can see is a
REM 64-bit one (here Virtual Desktop's 64-bit manifest), so the 32-bit game cannot load it. Expected in the log:
REM "no 32-bit OpenXR runtime reachable" -> "trying SteamVR's" -> "instance created on runtime 'SteamVR/OpenXR'".
REM Needs SteamVR installed; VD streaming SteamVR to the headset. This launch only - nothing on the PC changes.
setlocal
set "XR_RUNTIME_JSON=C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json"
set "RX=2064"
set "RY=2208"
set "INI=%LOCALAPPDATA%\Batman_City\launch.ini"
if exist "%INI%" (
  for /f "tokens=1,2 delims==" %%A in ('findstr /b /i "ResX= ResY=" "%INI%"') do (
    if /i "%%A"=="ResX" set "RX=%%B"
    if /i "%%A"=="ResY" set "RY=%%B"
  )
)
cd /d "E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32"
start "" BatmanAC.exe -windowed ResX=%RX% ResY=%RY%
