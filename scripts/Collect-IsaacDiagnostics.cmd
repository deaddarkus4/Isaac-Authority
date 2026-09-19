@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Collect-IsaacDiagnostics.ps1" %*
if errorlevel 1 echo Diagnostic collection failed. Copy the error message for troubleshooting.
pause
