@echo off
rem Extracts default.xex from the Forza Horizon ISO in this folder (nothing leaves your PC).
cd /d "%~dp0"
for %%f in (*.iso) do powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0extract_xiso.ps1" -Iso "%%f" -Out game_root -OnlyXex
pause
