@echo off
rem Runs the Forza Horizon port. The first time, extracts the whole disc into game_root (about 8.5 GB).
rem Each run writes its log to build_logs\run-<date>.log so it can be checked afterwards.
cd /d "%~dp0"
if not exist game_root\media (
  echo Extracting the full game from the ISO into game_root - this takes a few minutes...
  for %%f in (*.iso) do powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fh1-recomp\tools\extract_xiso.ps1" -Iso "%%f" -Out game_root
)
if not exist build_logs mkdir build_logs
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%t
set EXE=fh1-recomp\fh1\out\win-release\fh1.exe
if not exist "%EXE%" (echo fh1.exe not found - run build_fh1.bat first. & pause & exit /b 1)
echo Starting fh1.exe - log: build_logs\run-%STAMP%.log
"%EXE%" --game_data_root="%~dp0game_root" --log_file="%~dp0build_logs\run-%STAMP%.log" --log_level=debug
echo fh1.exe exited with code %ERRORLEVEL%
pause
