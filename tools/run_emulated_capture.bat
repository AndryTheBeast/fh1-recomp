@echo off
rem Diagnostic run: emulated GPU (the reference picture) started through RenderDoc, waiting for "capture now"
rem requests (fh1-recomp\tools\capture_now.ps1). Captures land in build_logs\rdc-spot-<date>_*.rdc.
cd /d "%~dp0"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%t
set EXEDIR=%~dp0fh1-recomp\fh1\out\win-release
"C:\Program Files\RenderDoc\renderdoccmd.exe" capture -d "%EXEDIR%" -c "%~dp0build_logs\rdc-spot-%STAMP%" "%EXEDIR%\fh1.exe" --game_data_root="%~dp0game_root" --log_file="%~dp0build_logs\run-%STAMP%.log" --log_level=debug --renderdoc_capture_seconds=-1
