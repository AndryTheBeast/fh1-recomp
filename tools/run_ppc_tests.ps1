<#
  Builds and runs the PPC instruction test suite (sdk/tests/ppc/asm, from upstream rexglue-sdk):
  every test states input registers and the correct output registers, is assembled with the
  upstream PowerPC binutils, translated by rexglue recompile-tests and checked with Catch2.
  A failing test = an instruction our translator gets wrong.

    powershell -ExecutionPolicy Bypass -File tools\run_ppc_tests.ps1 [-Filter "[instr_vmsum3fp128]"]

  Needs the binutils (powerpc-none-elf-as/ld/nm.exe + cygwin DLLs) from upstream
  tools/binutils in -ToolsDir (default: ppc_binutils next to the repository; not in git).
  Output: <top>\build_logs\ppc_tests.log
#>
param(
  [string]$ToolsDir = "",
  [string]$Filter = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Root
if (-not $ToolsDir) { $ToolsDir = Join-Path $Top "ppc_binutils" }
if (-not (Test-Path "$ToolsDir\powerpc-none-elf-as.exe")) { throw "PPC binutils not found in $ToolsDir" }
$Log = Join-Path $Top "build_logs\ppc_tests.log"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& (Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (Test-Path "C:\Program Files\LLVM\bin\clang.exe") { $env:Path = "C:\Program Files\LLVM\bin;" + $env:Path }

# Same build folder and flags as tools/build_windows.ps1's code generator step.
$HostDir = Join-Path $Root "out\host"
$Tools = $ToolsDir -replace '\\', '/'
& cmake -S "$Root\sdk" -B $HostDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_C_FLAGS=-mavx2" "-DCMAKE_CXX_FLAGS=-mavx2" `
  -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DREXGLUE_BUILD_TESTS=ON "-DPPC_TOOLS_DIR=$Tools" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& cmake --build $HostDir --target ppc_tests
if ($LASTEXITCODE -ne 0) { throw "ppc_tests build failed" }

$Exe = Get-ChildItem -Path "$Root\sdk\out" -Recurse -Filter ppc_tests.exe | Select-Object -First 1
$argv = @("--reporter", "compact")
if ($Filter) { $argv = @($Filter) + $argv }
& $Exe.FullName @argv 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $Log | Select-Object -Last 40
Write-Host "Full output: $Log"
