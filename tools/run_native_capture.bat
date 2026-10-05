@echo off
rem Diagnostic run: native renderer, waiting for "capture now" requests (fh1-recomp\tools\capture_now.ps1).
rem Looks like the normal native game; only the extra options differ.
cd /d "%~dp0"
call run_fh1.bat --fh1_renderer=native --fh1_dump_resolved_at_s=-1 --fh1_native_diag_frame_s=-1
