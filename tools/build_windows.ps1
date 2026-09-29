<#
  Builds the Forza Horizon port on Windows, start to finish:
    1. checks the tools (Git, CMake, Ninja, Python, LLVM clang, Visual Studio Build Tools)
    2. fetches the SDK's third-party sources
    3. builds the code generator (rexglue.exe)
    4. copies default.xex from your game_root into fh1\assets\game_root
    5. translates the game to C++ (codegen)
    6. compiles fh1.exe

    powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 -GameRoot C:\path\to\game_root [-Config Release] [-SkipCodegen]

  Every step is logged to -LogDir (default: the folder above the repository, in build_logs).
#>
param(
  [string]$GameRoot = "",
  [ValidateSet("Release", "RelWithDebInfo", "Debug")][string]$Config = "Release",
  [string]$LogDir = "",
  [switch]$SkipCodegen
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $GameRoot) { $GameRoot = Join-Path (Split-Path -Parent $Root) "game_root" }
if (-not $LogDir) { $LogDir = Join-Path (Split-Path -Parent $Root) "build_logs" }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
Start-Transcript -Path (Join-Path $LogDir "build-$Stamp.log") | Out-Null

function Step($text) { Write-Host ""; Write-Host "==== $text" -ForegroundColor Cyan }
function Run([string]$exe, [string[]]$argv, [string]$log) {
  Write-Host "> $exe $($argv -join ' ')"
  # Windows PowerShell turns a native tool's stderr lines into errors; with "Stop" the first
  # compiler warning would abort the build. The exit code is what decides success.
  $saved = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    if ($log) {
      & $exe @argv 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath (Join-Path $LogDir $log)
    } else {
      & $exe @argv
    }
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $saved }
  if ($code -ne 0) { throw "$exe failed (exit code $code). See $LogDir\$log" }
}

try {
  Step "Checking tools"
  Write-Host ("CPU: " + (Get-CimInstance Win32_Processor | Select-Object -First 1).Name)
  Write-Host ("RAM: {0:N0} GB" -f ((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB))

  # LLVM's installer does not add itself to PATH by default.
  if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
    if (Test-Path "C:\Program Files\LLVM\bin\clang.exe") { $env:Path += ";C:\Program Files\LLVM\bin" }
  }
  # Visual Studio developer environment: clang needs MSVC's libraries and the Windows SDK.
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "Visual Studio Build Tools not found (no vswhere.exe)." }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw "Visual Studio is installed but without the C++ tools (VCTools workload)." }
  Write-Host "Visual Studio: $vs"
  $devShell = Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1"
  & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
  # The dev shell puts its own clang/cmake first on PATH; prefer the standalone LLVM.
  if (Test-Path "C:\Program Files\LLVM\bin\clang.exe") { $env:Path = "C:\Program Files\LLVM\bin;" + $env:Path }

  foreach ($t in "git", "cmake", "ninja", "python", "clang") {
    $c = Get-Command $t -ErrorAction SilentlyContinue
    if (-not $c) { throw "'$t' not found. Install it (see README) and open a new window." }
    Write-Host ("{0,-7} {1}" -f $t, $c.Source)
  }
  $clangVer = (& clang --version | Select-Object -First 1)
  Write-Host $clangVer
  if ($clangVer -match "version (\d+)\." -and [int]$Matches[1] -lt 20) { throw "clang 20 or newer is needed." }

  $Xex = Join-Path $GameRoot "default.xex"
  if (-not (Test-Path $Xex)) { throw "default.xex not found in $GameRoot. Run tools\extract_xex.bat next to your ISO first." }

  # AVX2 for the SDK's byte-swap and vector code (ReXGlue, like Xenia, expects it).
  $Flags = "-mavx2"

  Step "Fetching third-party sources"
  Run python @("$Root\tools\fetch_thirdparty.py") "fetch_thirdparty.log"

  Step "Building the code generator"
  $HostDir = Join-Path $Root "out\host"
  Run cmake @("-S", "$Root\sdk", "-B", $HostDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
      "-DCMAKE_C_FLAGS=$Flags", "-DCMAKE_CXX_FLAGS=$Flags", "-DREXGLUE_ENABLE_TRACY=OFF", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5") "host_configure.log"
  Run cmake @("--build", $HostDir, "--target", "rexglue") "host_build.log"
  $Rexglue = Get-ChildItem -Path "$Root\sdk\out" -Recurse -Filter rexglue.exe | Select-Object -First 1
  if (-not $Rexglue) { throw "rexglue.exe was not produced." }
  Write-Host "rexglue: $($Rexglue.FullName)"

  Step "Copying the game executables"
  $Assets = Join-Path $Root "fh1\assets\game_root"
  New-Item -ItemType Directory -Force -Path $Assets | Out-Null
  # default.xex plus the modules it loads at run time (fh1_manifest.toml [[modules]]).
  foreach ($x in "default.xex", "XMediaFacade_default.xex", "SpeechFacade_default.xex") {
    $src = Join-Path $GameRoot $x
    if (-not (Test-Path $src)) { throw "$x not found in $GameRoot. Extract the full disc first (run_fh1.bat does it)." }
    Copy-Item $src (Join-Path $Assets $x) -Force
  }

  if (-not $SkipCodegen) {
    Step "Translating the game to C++ (a few minutes)"
    Push-Location (Join-Path $Root "fh1")
    try { Run $Rexglue.FullName @("codegen", "fh1_manifest.toml") "codegen.log" } finally { Pop-Location }
  }

  Step "Compiling fh1.exe ($Config) - this is the long part"
  $AppDir = Join-Path $Root "fh1\out\win-$($Config.ToLower())"
  Run cmake @("-S", "$Root\fh1", "-B", $AppDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config",
      "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
      "-DCMAKE_C_FLAGS=$Flags", "-DCMAKE_CXX_FLAGS=$Flags", "-DREXGLUE_ENABLE_TRACY=OFF",
      "-DREXSDK_DIR=$Root\sdk", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5") "app_configure.log"
  Run cmake @("--build", $AppDir) "app_build.log"

  $Exe = Get-ChildItem -Path $AppDir, "$Root\sdk\out" -Recurse -Filter fh1.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  Step "Done"
  if ($Exe) { Write-Host "Built: $($Exe.FullName)" -ForegroundColor Green } else { Write-Host "Build finished but fh1.exe was not found." -ForegroundColor Yellow }
}
catch {
  Write-Host ""
  Write-Host "BUILD FAILED: $_" -ForegroundColor Red
  Write-Host "Logs: $LogDir"
  exit 1
}
finally {
  Stop-Transcript | Out-Null
}
