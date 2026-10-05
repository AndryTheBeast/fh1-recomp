<#
  Unattended capture of the cockpit (first-person) view at the festival. The game remembers the camera of the
  last run, so the number of RB presses to reach the cockpit is not fixed: this script presses RB every 8 s,
  screenshots each view, recognises the cockpit by its dark roof lining at the top of the picture and asks the
  running game for its diagnostics at that moment (the same triggers as tools\capture_now.ps1).

    powershell -ExecutionPolicy Bypass -File tools\view_capture.ps1 -Name cock -Native
    powershell -ExecutionPolicy Bypass -File tools\view_capture.ps1 -Name cock            (emulated, RenderDoc)

  Native: dump in fh1\out\win-release\dump_resolved and a [trace] frame in the log. Emulated:
  build_logs\rdc-view-<Name>-<date>_capture*.rdc. Screenshots: build_logs\view-<Name>-<date>-<second>s.png.
  -Native -RenderDoc: a RenderDoc capture of the native renderer's own frame (--fh1_native_renderdoc) and a trace.
  -NoCapture only takes the screenshots (a quick before / after check of the picture).
#>
param(
  [string]$Name = "view",
  [switch]$Native,
  [switch]$NoCapture,
  # With -Native: a RenderDoc capture of the native renderer's frame instead of the picture dump.
  [switch]$RenderDoc,
  [string]$ExtraArgs = "",
  [int]$First = 58
)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$ExeDir = Join-Path $Repo "fh1\out\win-release"
$Exe = Join-Path $ExeDir "fh1.exe"
$Logs = Join-Path $Top "build_logs"
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Log = Join-Path $Logs "view-$Name-$Stamp.log"

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

# Screenshots the window and returns the mean brightness of the strip where the cockpit has its roof lining
# and every other view has sky (35-65 % of the width, 10-14 % of the height).
function ShotRoof($proc, [string]$path) {
  $proc.Refresh()
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { return 255 }
  $r = New-Object Fh1Win+RECT
  [Fh1Win]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hgt = $r.B - $r.T
  if ($w -le 0 -or $hgt -le 0) { return 255 }
  $bmp = New-Object System.Drawing.Bitmap $w, $hgt
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [Fh1Win]::PrintWindow($h, $hdc, 2) | Out-Null
  $g.ReleaseHdc($hdc)
  $sum = 0; $n = 0
  for ($y = [int]($hgt * 0.10); $y -lt [int]($hgt * 0.14); $y += 6) {
    for ($x = [int]($w * 0.35); $x -lt [int]($w * 0.65); $x += 12) {
      $c = $bmp.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3; $n++
    }
  }
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  $mean = [int]($sum / [Math]::Max($n, 1))
  Write-Host "screenshot: $path  (roof strip $mean)"
  return $mean
}

$presses = @(0..6 | ForEach-Object { "{0}+0.2=rb" -f ($First + 8 * $_) }) -join ";"
$auto = "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a;$presses"
$argv = @("--game_data_root=$Top\game_root", "--log_file=$Log", "--log_level=debug", "--fh1_autoplay=`"$auto`"") +
  @($ExtraArgs -split "\s+" | Where-Object { $_ })
if ($Native) { $argv += "--fh1_renderer=native" }
if ($Native -and -not $NoCapture -and -not $RenderDoc) {
  $argv += @("--fh1_dump_resolved_at_s=-1", "--fh1_native_diag_frame_s=-1")
}
if ($NoCapture -or ($Native -and -not $RenderDoc)) {
  $p = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory $ExeDir -PassThru
} else {
  # Through RenderDoc: the emulated GPU, or the native renderer with -Native -RenderDoc (a trace with it).
  $argv += $(if ($Native) { @("--fh1_native_renderdoc=true", "--fh1_native_diag_frame_s=-1") } else { "--renderdoc_capture_seconds=-1" })
  $rdArgv = @("capture", "-d", $ExeDir, "-c", (Join-Path $Logs "rdc-view-$Name-$Stamp"), $Exe) + $argv
  $launch = Get-Date
  Start-Process -FilePath "C:\Program Files\RenderDoc\renderdoccmd.exe" -ArgumentList $rdArgv -WorkingDirectory $ExeDir | Out-Null
  $p = $null
  for ($i = 0; $i -lt 60 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process fh1 -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $launch.AddSeconds(-2) } |
      Sort-Object StartTime -Descending | Select-Object -First 1
  }
  if (-not $p) { throw "RenderDoc did not start fh1.exe" }
}
$start = Get-Date
$found = $false
foreach ($i in 0..6) {
  $s = $First + 8 * $i + 5
  $wait = $s - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  if ($p.HasExited) { break }
  $roof = ShotRoof $p (Join-Path $Logs "view-$Name-$Stamp-${s}s.png")
  if ($roof -lt 30) {
    $found = $true
    Write-Host "cockpit view at $s s"
    if (-not $NoCapture) {
      $triggers = @("dump_now", "trace_now", "capture_now") | ForEach-Object { Join-Path $ExeDir $_ }
      foreach ($f in $triggers) { Set-Content -Path $f -Value "1" }
      # The next RB press is 3 s away: the dump of one frame is written within it, the files after it.
      Start-Sleep -Seconds 14
      foreach ($f in $triggers) { if (Test-Path $f) { Remove-Item $f } else { Write-Host "taken: $(Split-Path -Leaf $f)" } }
    }
    break
  }
}
if (-not $found) { Write-Host "the cockpit view was not recognised" }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2
Write-Host "=== $Name  log: $Log"
if (Test-Path "$Log.crash.txt") { Write-Host "CRASHED:"; Get-Content "$Log.crash.txt" | Select-Object -First 12 }
