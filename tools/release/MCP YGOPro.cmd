@echo off
setlocal
cd /d "%~dp0"
call "%~dp0Check game data.cmd" 1>&2
if errorlevel 1 exit /b 1
"%~dp0YGOPro.exe" %*
exit /b %errorlevel%
