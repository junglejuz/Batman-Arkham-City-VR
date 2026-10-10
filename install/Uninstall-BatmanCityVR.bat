@echo off
REM Batman City VR uninstaller (lives in the game folder after installing).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-BatmanCityVR.ps1" %*
pause
if not exist "%~dp0Uninstall-BatmanCityVR.ps1" (goto) 2>nul & del "%~f0"
