<#
  Runs the unattended test as on a PC that has never run the game: the native renderer's own cache file
  (fh1\out\win-release\cache\fh1_native_pipelines.bin) and the AMD driver's Vulkan cache folder
  (%LOCALAPPDATA%\AMD\VkCache) are set aside for the run and put back after it, whatever happens. Nothing is
  deleted: what the run itself wrote is kept in build_logs\pipelines-backup-<date>\ and VkCache.test-<stamp>.

    powershell -ExecutionPolicy Bypass -File tools\fresh_pc_test.ps1 -Name fresh1 [-Seconds 100]

  The game must be closed. Only AMD's folder is handled; on another vendor only our file is set aside.
#>
param(
  [string]$Name = "fresh",
  [int]$Seconds = 100,
  [string]$Shots = "53,70,90",
  [string]$ExtraArgs = "--fh1_renderer=native",
  [string]$Autoplay = "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a;60+0.3=x;74+0.3=a"
)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Cache = Join-Path $Repo "fh1\out\win-release\cache\fh1_native_pipelines.bin"
$Backup = Join-Path $Top ("build_logs\pipelines-backup-" + (Get-Date -Format "yyyyMMdd"))
$Vk = Join-Path $env:LOCALAPPDATA "AMD\VkCache"
$VkKeep = "$Vk.keep"

if (Get-Process fh1 -ErrorAction SilentlyContinue) { throw "fh1.exe is running: close the game first." }
if (Test-Path $VkKeep) { throw "$VkKeep already exists: an earlier run was not put back. Look at it first." }
New-Item -ItemType Directory -Force $Backup | Out-Null
$CacheKeep = Join-Path $Backup "fh1_native_pipelines.keep-$Stamp.bin"
$hadCache = Test-Path $Cache
$hadVk = Test-Path $Vk
try {
  if ($hadCache) { Move-Item $Cache $CacheKeep }
  if ($hadVk) { Rename-Item $Vk $VkKeep }
  Write-Host "set aside: our cache file = $hadCache, the AMD driver's folder = $hadVk"
  & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "auto_test.ps1") -Name $Name -Seconds $Seconds `
    -Shots $Shots -ExtraArgs $ExtraArgs -Autoplay $Autoplay
}
finally {
  Start-Sleep -Seconds 2
  # What the run wrote is kept under another name, then the originals return.
  if (Test-Path $Cache) { Move-Item $Cache (Join-Path $Backup "fh1_native_pipelines.result-$Name-$Stamp.bin") }
  if ($hadCache) { Move-Item $CacheKeep $Cache }
  if ($hadVk) {
    if (Test-Path $Vk) { Rename-Item $Vk "$Vk.test-$Stamp" }
    Rename-Item $VkKeep $Vk
  }
  Write-Host "put back: our cache file $(if (Test-Path $Cache) { (Get-Item $Cache).Length } else { 'absent' }) bytes; AMD folder present = $(Test-Path $Vk), .keep left over = $(Test-Path $VkKeep)"
}
