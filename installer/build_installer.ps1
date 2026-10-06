<#
  Builds installer\out\FH1Installer.exe with the C# compiler that is part of Windows (.NET Framework 4.8):
  nothing to install. The program runs on any Windows 10 / 11.
    powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
#>
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
$Csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $Csc)) { throw "csc.exe not found at $Csc" }
$Out = Join-Path $Here "out"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Exe = Join-Path $Out "FH1Installer.exe"
$Sources = Get-ChildItem (Join-Path $Here "*.cs") | ForEach-Object { $_.FullName }
& $Csc /nologo /target:winexe /platform:x64 /optimize+ /warn:4 "/out:$Exe" `
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll $Sources
if ($LASTEXITCODE -ne 0) { throw "the C# compiler failed" }
Write-Host ("Built: {0} ({1:N0} bytes)" -f $Exe, (Get-Item $Exe).Length)
