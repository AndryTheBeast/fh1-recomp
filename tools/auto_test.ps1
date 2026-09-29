<#
  Runs fh1.exe unattended for a fixed time, screenshots the game window at chosen moments, then
  closes it and prints the log's [fps] lines. Used to test changes without anyone at the PC.

    powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1 -Name rtv -Seconds 75 `
      -Shots 20,45,70 -ExtraArgs "--render_target_path_d3d12=rtv"

  Output (in build_logs\): test-<Name>-<date>.log and test-<Name>-<date>-<second>s.png.
  Only the game window's area is captured, and only while fh1.exe is running.
#>
param(
  [string]$Name = "run",
  [int]$Seconds = 60,
  [string]$Shots = "",
  [string]$ExtraArgs = "",
  # Scripted controller, passed as --fh1_autoplay (format in fh1/src/fh1_autoplay.h).
  [string]$Autoplay = ""
)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$Exe = Join-Path $Repo "fh1\out\win-release\fh1.exe"
$Logs = Join-Path $Top "build_logs"
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Log = Join-Path $Logs "test-$Name-$Stamp.log"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Fh1Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Fh1Win]::SetProcessDPIAware() | Out-Null

function Shot($proc, [string]$path) {
  $proc.Refresh()
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { Write-Host "no game window yet for $path"; return }
  [Fh1Win]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 300
  $r = New-Object Fh1Win+RECT
  [Fh1Win]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hgt = $r.B - $r.T
  if ($w -le 0 -or $hgt -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap $w, $hgt
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Write-Host "screenshot: $path"
}

# Comma/space-separated lists: "powershell -File" passes each parameter as one string.
$ShotList = @($Shots -split "[,\s]+" | Where-Object { $_ } | ForEach-Object { [int]$_ })
$argv = @("--game_data_root=$Top\game_root", "--log_file=$Log", "--log_level=debug") +
  @($ExtraArgs -split "\s+" | Where-Object { $_ })
if ($Autoplay) { $argv += "--fh1_autoplay=`"$Autoplay`"" }
$p = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory (Split-Path $Exe) -PassThru
$start = Get-Date
foreach ($s in ($ShotList | Sort-Object)) {
  $wait = $s - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  if ($p.HasExited) { break }
  Shot $p (Join-Path $Logs ("test-$Name-$Stamp-{0}s.png" -f $s))
}
$wait = $Seconds - ((Get-Date) - $start).TotalSeconds
if ($wait -gt 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

Write-Host "=== $Name  log: $Log"
if (Test-Path "$Log.crash.txt") { Write-Host "CRASHED:"; Get-Content "$Log.crash.txt" | Select-Object -First 12 }
Get-ChildItem "$Log*" -Exclude *.png, *.txt | Get-Content |
  Select-String "\[fps\]|DXGI adapter|\[video\] el juego abre" |
  ForEach-Object { $_.Line -replace '^\[\d+-\d+-\d+ ([\d:.]+)\] \[\w+\] \[\w+\] \[t\d+\] ', '$1 ' }
