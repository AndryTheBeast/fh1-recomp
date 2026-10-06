<#
  Gathers what the installer puts into an installation folder, from this PC's build:
    installer\out\package\port    fh1.exe, its DLLs, the pipeline list (no .pdb, no shader library, no cache)
    installer\out\package\tools   the shader tools (translator, archive decoder, packer, DXC)
  While the installer does not download yet, it takes the folder "package" next to FH1Installer.exe. For a
  release the same two folders are zipped (fh1-win64.zip, fh1-shader-tools.zip).
    powershell -ExecutionPolicy Bypass -File installer\make_package.ps1
#>
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Top = Split-Path -Parent $Repo
$Build = Join-Path $Repo "fh1\out\win-release"
$Package = Join-Path $PSScriptRoot "out\package"
if (Test-Path $Package) { Remove-Item -Recurse -Force $Package }
$Port = New-Item -ItemType Directory -Force (Join-Path $Package "port")
$Tools = New-Item -ItemType Directory -Force (Join-Path $Package "tools")
foreach ($f in "fh1.exe", "rexruntime.dll", "rexgpu-xenos.dll", "fh1_XMediaFacade_default.dll",
               "fh1_SpeechFacade_default.dll", "fh1_pipelines.nfpl") {
  Copy-Item (Join-Path $Build $f) $Port
}
foreach ($f in "shaders\fh1_hlsl.exe", "shaders\fh1_lzx_decode.exe", "shaders\fh1_pack_library.exe",
               "shaders\XenosRecomp\shader_common.h") {
  Copy-Item (Join-Path $Repo $f) $Tools
}
foreach ($f in "dxc.exe", "dxcompiler.dll") { Copy-Item (Join-Path $Top "tools_dxc\bin\x64\$f") $Tools }
# Nothing given to other people may name this PC's user folder.
$needle = [Text.Encoding]::ASCII.GetBytes("Users\" + $env:USERNAME)
foreach ($file in Get-ChildItem $Package -Recurse -File) {
  $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($file.FullName))
  foreach ($form in ("Users\" + $env:USERNAME), ("Users/" + $env:USERNAME)) {
    if ($text.IndexOf($form, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
      Write-Warning "$($file.Name) contains a path with this PC's user name ($form)"
    }
  }
}
Get-ChildItem $Package -Recurse -File | ForEach-Object { "{0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($Package.Length + 1) }
"{0:N1} MB in all" -f ((Get-ChildItem $Package -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
