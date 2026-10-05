@echo off
rem Diagnostic run: native renderer that can leave out the draws of chosen shaders while it runs
rem (fh1-recomp\tools\skip_cycle.ps1), plus the "capture now" options. Parts of the picture can vanish for a
rem moment while a test cycle runs: that is the test, not the build.
cd /d "%~dp0"
type nul > skip_ps.txt
call run_fh1.bat --fh1_renderer=native --fh1_native_diag_skip_ps=file --fh1_dump_resolved_at_s=-1 --fh1_native_diag_frame_s=-1
