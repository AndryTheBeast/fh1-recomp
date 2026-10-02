<#
  Builds the shader translator front-end (shaders/nfsmw_hlsl.cpp + XenosRecomp's recompiler) into
  shaders/fh1_hlsl.exe with clang++ in the Visual Studio environment. Native renderer step N0
  (docs/native-renderer-fh1.md). Needs only sources already in the repo (fmt, xxHash in sdk/thirdparty).
    powershell -ExecutionPolicy Bypass -File tools\build_shader_tools.ps1
#>
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& (Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (Test-Path "C:\Program Files\LLVM\bin\clang.exe") { $env:Path = "C:\Program Files\LLVM\bin;" + $env:Path }
$S = Join-Path $Repo "shaders"
& clang++ -std=c++23 -O2 "-I$S" "-I$S\XenosRecomp" "-I$Repo\sdk\thirdparty\fmt\include" `
  "-I$Repo\sdk\thirdparty\xxHash" -include "$S\pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL `
  -DNFSMW_RECOMP -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -Wno-switch -Wno-unused-variable -fms-extensions `
  "$S\nfsmw_hlsl.cpp" "$S\XenosRecomp\shader_recompiler.cpp" -o "$S\fh1_hlsl.exe"
exit $LASTEXITCODE
