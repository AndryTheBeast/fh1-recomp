@echo off
rem Downloads (or updates) the Forza Horizon port and builds it. Logs go to build_logs\.
cd /d "%~dp0"
where git >nul 2>nul || (echo Git is not installed or not on PATH. & pause & exit /b 1)
if not exist fh1-recomp\.git (
  git clone https://github.com/AndryTheBeast/fh1-recomp.git
) else (
  git -C fh1-recomp pull --ff-only
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fh1-recomp\tools\build_windows.ps1" -GameRoot "%~dp0game_root" -LogDir "%~dp0build_logs"
pause
