@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Start-IsaacDiagnostics.ps1" %*
