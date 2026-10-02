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
  [string]$Autoplay = "",
  # Autoplay script file (tools/autoplay/*.txt, format in fh1/src/fh1_autoplay.h). Its "shot NAME"
  # lines produce test-<Name>-<date>-NAME.png; "quit" ends the run early.
  [string]$Script = "",
  # Run on a fresh copy of this user data folder (saves, profile) instead of the real one, via
  # --user_data_root: e.g. -SaveFrom ..\build_logs\testsaves\paintshop. The real save is never used.
  [string]$SaveFrom = "",
  # Launch through RenderDoc (renderdoccmd capture). Combine with
  # -ExtraArgs "--renderdoc_capture_seconds=120,150" to record single frames unattended;
  # captures land in build_logs\rdc-<Name>-<date>_frame*.rdc.
  [switch]$RenderDoc
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
if ($SaveFrom) {
  $SaveCopy = Join-Path $Logs "testsave-$Name-$Stamp"
  Copy-Item -Recurse -Force (Resolve-Path $SaveFrom).Path $SaveCopy
  $argv += "--user_data_root=`"$SaveCopy`""
  Write-Host "test save: $SaveCopy (copied from $SaveFrom)"
}
$ReqDir = $null
if ($Script) {
  $ScriptPath = (Resolve-Path $Script).Path
  $ReqDir = Join-Path $Logs "autoplay-$Name-$Stamp"
  New-Item -ItemType Directory -Force $ReqDir | Out-Null
  $argv += "--fh1_autoplay_file=`"$ScriptPath`""
  $argv += "--fh1_autoplay_dir=`"$ReqDir`""
}
if ($RenderDoc) {
  $RdCmd = "C:\Program Files\RenderDoc\renderdoccmd.exe"
  $rdArgv = @("capture", "-d", (Split-Path $Exe), "-c", (Join-Path $Logs "rdc-$Name-$Stamp"), $Exe) + $argv
  $launchTime = Get-Date
  # renderdoccmd stays alive as long as the game does, so do not -Wait for it: poll for fh1.
  Start-Process -FilePath $RdCmd -ArgumentList $rdArgv -WorkingDirectory (Split-Path $Exe) | Out-Null
  $p = $null
  for ($i = 0; $i -lt 60 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process fh1 -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $launchTime.AddSeconds(-2) } |
      Sort-Object StartTime -Descending | Select-Object -First 1
  }
  if (-not $p) { throw "RenderDoc did not start fh1.exe" }
} else {
  $p = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory (Split-Path $Exe) -PassThru
}
$start = Get-Date
# Script requests: shot-NAME.req -> screenshot, quit.req -> stop. Polled while waiting.
$quit = $false
function Serve-Requests {
  if (-not $ReqDir) { return }
  foreach ($req in (Get-ChildItem $ReqDir -Filter *.req -ErrorAction SilentlyContinue)) {
    $n = $req.BaseName
    if ($n -like "shot-*") {
      Shot $p (Join-Path $Logs ("test-$Name-$Stamp-{0}.png" -f $n.Substring(5)))
    } elseif ($n -eq "quit") {
      $script:quit = $true
    }
    Remove-Item $req.FullName -Force -ErrorAction SilentlyContinue
  }
}
function Wait-Until([double]$until) {
  while (((Get-Date) - $start).TotalSeconds -lt $until -and -not $p.HasExited -and -not $script:quit) {
    Serve-Requests
    Start-Sleep -Milliseconds 100
  }
}
foreach ($s in ($ShotList | Sort-Object)) {
  Wait-Until $s
  if ($p.HasExited -or $quit) { break }
  Shot $p (Join-Path $Logs ("test-$Name-$Stamp-{0}s.png" -f $s))
}
Wait-Until $Seconds
Serve-Requests
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

Write-Host "=== $Name  log: $Log"
if (Test-Path "$Log.crash.txt") { Write-Host "CRASHED:"; Get-Content "$Log.crash.txt" | Select-Object -First 12 }
# Logs rotate at 5 MB into test-...-<stamp>.1.log, .2.log: read every part, oldest first.
Get-ChildItem (Join-Path $Logs "test-$Name-$Stamp*.log") | Sort-Object LastWriteTime | Get-Content |
  Select-String "\[fps\]|DXGI adapter|\[video\] el juego abre|\[autoplay\]|fh1_autoplay" |
  ForEach-Object { $_.Line -replace '^\[\d+-\d+-\d+ ([\d:.]+)\] \[\w+\] \[\w+\] \[t\d+\] ', '$1 ' }
