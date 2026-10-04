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
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Library packer: shaders/fh1_pack_library.exe CONTAINERS SPIRV OUT.nfsp
& clang++ -std=c++23 -O2 "-I$Repo\sdk\thirdparty\xxHash" -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX `
  "$S\fh1_pack_library.cpp" "$Repo\fh1\src\native\fh1_shader_library.cpp" -o "$S\fh1_pack_library.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Zip method 21 decoder (XMemCompress LZX) for the tracks' archives: libmspack's lzxd.c.
$M = "$Repo\sdk\thirdparty\libmspack\libmspack\mspack"
& clang -c -O2 -D_CRT_SECURE_NO_WARNINGS "-I$M" "$M\lzxd.c" -o "$S\lzxd.o"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& clang++ -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS "-I$M" "$Repo\tools\fh1_lzx_decode.cpp" "$S\lzxd.o" `
  -o "$S\fh1_lzx_decode.exe"
exit $LASTEXITCODE
