@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0" 1>&2 || exit /b 1
"%~dp0YGOProUpdateLauncher.exe" --mcp
exit /b %errorlevel%
