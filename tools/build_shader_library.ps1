<#
  Builds FH1's shader library for the native renderer (docs/native-renderer-fh1.md, step N0), from
  the extracted disc. Output: build_logs\shaders\fh1_shaders.nfsp (game-derived: never in git).
    powershell -ExecutionPolicy Bypass -File tools\build_shader_library.ps1
  Steps: build the tools (tools\build_shader_tools.ps1), cut the containers out of the .fxobj files,
  translate them to HLSL (XenosRecomp), compile to SPIR-V (DXC with SPIR-V support, from
  FH1-recomp\tools_dxc - the Windows SDK's dxc.exe has none), pack.
#>
param([string]$Dxc = "", [string]$Merge = "", [string]$Image = "")
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$Out = Join-Path $Top "build_logs\shaders"
if (-not $Dxc) { $Dxc = Join-Path $Top "tools_dxc\bin\x64\dxc.exe" }
if (-not (Test-Path $Dxc)) { throw "DXC with SPIR-V not found at $Dxc (official release zip from github.com/microsoft/DirectXShaderCompiler)" }

& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_shader_tools.ps1")
if ($LASTEXITCODE -ne 0) { throw "building the shader tools failed" }
foreach ($d in "containers", "hlsl", "spirv") { $p = Join-Path $Out $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
$lib = Join-Path $Out "fh1_shaders.nfsp"
if (Test-Path $lib) { Remove-Item -Force $lib }

# The tracks' shader files are inside their bin.zip archives (zip method 21, LZX).
$unpacked = Join-Path $Out "unpacked"
if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
& python (Join-Path $PSScriptRoot "fh1_unpack_archives.py") (Join-Path $Top "game_root") $unpacked
$mergeArgs = @("--also", $unpacked)
if ($Merge) { $mergeArgs += @("--merge", $Merge) }
# The engine's own shaders (~480: post-processing, UI...) are inside default.xex, which is encrypted
# and compressed on disc. The loaded image written by a run with
#   --fh1_dump_image=<FH1-recomp>\build_logs\fh1_image.bin
# (early in boot, before anything touches shader data) is scanned for them.
if (-not $Image) { $Image = Join-Path $Top "build_logs\fh1_image.bin" }
if (Test-Path $Image) {
  $imageDir = Join-Path $Out "image"
  if (Test-Path $imageDir) { Remove-Item -Recurse -Force $imageDir }
  New-Item -ItemType Directory -Force $imageDir | Out-Null
  Copy-Item $Image (Join-Path $imageDir "image.bin")
  $mergeArgs += @("--merge", $imageDir)
} else {
  Write-Warning "No game image at ${Image} - the ~480 shaders inside default.xex are left out (run the game once with --fh1_dump_image=${Image})"
}
& python (Join-Path $PSScriptRoot "fh1_extract_shaders.py") (Join-Path $Top "game_root") (Join-Path $Out "containers") @mergeArgs
& python (Join-Path $PSScriptRoot "fh1_translate_shaders.py") (Join-Path $Out "containers") (Join-Path $Out "hlsl")
& python (Join-Path $PSScriptRoot "fh1_compile_shaders.py") (Join-Path $Out "hlsl") (Join-Path $Out "spirv") $Dxc
& (Join-Path $Repo "shaders\fh1_empaquetar.exe") (Join-Path $Out "containers") (Join-Path $Out "spirv") $lib
if ($LASTEXITCODE -ne 0) { throw "packing failed" }
