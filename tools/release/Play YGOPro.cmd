@echo off
setlocal
cd /d "%~dp0"
call "%~dp0Check game data.cmd"
if errorlevel 1 (
  echo.
  echo See GAME-DATA-SETUP.txt in this folder.
  pause
  exit /b 1
)
"%~dp0YGOPro.exe" %*
exit /b %errorlevel%
