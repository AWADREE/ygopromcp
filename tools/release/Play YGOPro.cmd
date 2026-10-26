@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0" || exit /b 1
"%~dp0YGOProUpdateLauncher.exe"
exit /b %errorlevel%
