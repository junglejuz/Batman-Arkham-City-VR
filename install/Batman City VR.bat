@echo off
REM Batman City VR - launcher. Lives next to BatmanAC.exe (the installer puts it there).
REM Needs: Steam running and your PC VR software running (it must be the active OpenXR runtime).
REM VR render size comes from %LOCALAPPDATA%\Batman_City\launch.ini (set in the F10 menu,
REM "VR resolution"); default 2064x2208 = Quest 3 native through Virtual Desktop.
REM The mod lifts Windows' monitor-size cap, so sizes above the desktop are honoured.
setlocal
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
start "" BatmanAC.exe -windowed ResX=%RX% ResY=%RY%
