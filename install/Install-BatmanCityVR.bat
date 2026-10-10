@echo off
REM Batman City VR installer: runs Install-BatmanCityVR.ps1 from this folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-BatmanCityVR.ps1" %*
pause
