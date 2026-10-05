<#
  Finds which pixel shader paints something, at a spot only the user can reach. The game must run the native
  renderer with --fh1_native_diag_skip_ps=file (tools\run_native_skip.bat) and the user stays parked. For each
  entry of -List (one PS number or several joined with "+") the draws of those shaders are left out for a
  moment and the window is photographed: build_logs\skip-<Name>-<entry>.png ("none" = nothing left out).

    powershell -ExecutionPolicy Bypass -File tools\skip_cycle.ps1 -Name verge -List "1831,364,2412+2292"
#>
param([string]$Name = "skip", [string]$List = "", [double]$Wait = 2.5)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$File = Join-Path $Top "skip_ps.txt"
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
function Shot([string]$path) {
  $h = $p.MainWindowHandle
  $r = New-Object Fh1Win+RECT
  [Fh1Win]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [Fh1Win]::PrintWindow($h, $hdc, 2) | Out-Null
  $g.ReleaseHdc($hdc)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
foreach ($entry in @("none") + @($List -split "[,\s]+" | Where-Object { $_ })) {
  $text = if ($entry -eq "none") { "" } else { $entry -replace "\+", "," }
  Set-Content -Path $File -Value $text -Encoding ascii
  Start-Sleep -Milliseconds ([int]($Wait * 1000))
  Shot (Join-Path $Top "build_logs\skip-$Name-$entry.png")
  Write-Host "skip-$Name-$entry.png"
}
Set-Content -Path $File -Value "" -Encoding ascii
