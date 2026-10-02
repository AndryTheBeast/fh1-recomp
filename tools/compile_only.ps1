<#
  Compiles single SDK/app object files without linking (works while fh1.exe is running, when
  the full build cannot replace the exe or the GPU plugin DLL). Paths are ninja object targets
  relative to fh1\out\win-release, e.g.:
    powershell -ExecutionPolicy Bypass -File tools\compile_only.ps1 `
      rexglue-sdk/src/graphics/CMakeFiles/rexgpu-xenos.dir/pipeline/render_target/cache.cpp.obj
#>
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& (Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (Test-Path "C:\Program Files\LLVM\bin\clang.exe") { $env:Path = "C:\Program Files\LLVM\bin;" + $env:Path }
& ninja -C (Join-Path $Repo "fh1\out\win-release") @args
exit $LASTEXITCODE
