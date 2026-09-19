@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Install-IsaacNetProbe.ps1" %*
pause
