<#
  Asks a running fh1.exe for its diagnostics of the current frame and screenshots its window. For spots the
  unattended test does not reach (the user drives there, stops and says "now").

    powershell -ExecutionPolicy Bypass -File tools\capture_now.ps1 -Name road1

  The game must have been started by tools\run_native_capture.bat (native renderer: dump of every picture of
  the frame in FH1-recomp\dump_resolved*, one-frame trace in the log) or tools\run_emulated_capture.bat
  (emulated GPU through RenderDoc: build_logs\rdc-spot-*.rdc). Both triggers are written; each game only
  looks for its own. Screenshot: build_logs\spot-<Name>-<date>.png.
#>
param([string]$Name = "spot")
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$ExeDir = Join-Path $Repo "fh1\out\win-release"
$p = Get-Process fh1 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw "fh1.exe is not running" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Fh1Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
[Fh1Win]::SetProcessDPIAware() | Out-Null
$h = $p.MainWindowHandle
$r = New-Object Fh1Win+RECT
[Fh1Win]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Fh1Win]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$png = Join-Path $Top ("build_logs\spot-$Name-{0}.png" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "screenshot: $png"

# Native renderer (working folder = FH1-recomp): dump first, the trace of the same moment with it.
Set-Content -Path (Join-Path $Top "dump_now") -Value "1"
Set-Content -Path (Join-Path $Top "trace_now") -Value "1"
# Emulated GPU under RenderDoc (working folder = the exe's).
Set-Content -Path (Join-Path $ExeDir "capture_now") -Value "1"
Start-Sleep -Seconds 4
foreach ($f in @((Join-Path $Top "dump_now"), (Join-Path $Top "trace_now"), (Join-Path $ExeDir "capture_now"))) {
  if (Test-Path $f) { Remove-Item $f } else { Write-Host "taken: $(Split-Path -Leaf $f)" }
}
