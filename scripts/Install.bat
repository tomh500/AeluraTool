@echo off
setlocal
rem Use Windows PowerShell's own modules even when launched from PowerShell 7.
set "PSModulePath="
set "installerPowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "installerPowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%installerPowerShell%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -NoPause %*
set "installerExitCode=%errorlevel%"
if not "%installerExitCode%"=="0" pause
exit /b %installerExitCode%
