<#
  Installs everything needed to build and run the Forza Horizon port on a fresh Windows 10/11 PC,
  using winget (built into Windows 11):

    Git, CMake, Ninja, Python 3.13, LLVM (clang), Visual Studio 2022 Build Tools (C++ workload),
    VC++ 2015-2022 redistributable, GitHub CLI

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

  Safe to run again: packages already installed are skipped. Windows asks for administrator
  permission (UAC) for some installers. The graphics driver (NVIDIA/AMD/Intel) is not installed
  here; get it from the vendor's site.
#>
$ErrorActionPreference = "Stop"

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
  throw "winget not found. Install 'App Installer' from the Microsoft Store, then run this again."
}

function Install([string]$id, [string[]]$extra = @()) {
  Write-Host ""
  Write-Host "==== $id" -ForegroundColor Cyan
  $argv = @("install", "--id", $id, "--exact", "--silent", "--accept-package-agreements",
            "--accept-source-agreements") + $extra
  & winget @argv
  # 0 = installed; -1978335189 (0x8A15002B) = already installed, no upgrade available.
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
    Write-Warning "winget returned $LASTEXITCODE for $id"
  }
}

Install "Git.Git"
Install "Kitware.CMake"
Install "Ninja-build.Ninja"
Install "Python.Python.3.13"
Install "LLVM.LLVM"
Install "GitHub.cli"
Install "Microsoft.VCRedist.2015+.x64"
# C++ compiler libraries and the Windows SDK that clang links against.
Install "Microsoft.VisualStudio.2022.BuildTools" @("--override",
  "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended")

# LLVM's installer does not add itself to PATH; tools/build_windows.ps1 finds it anyway, but a
# PATH entry lets clang be used from any terminal.
$llvm = "C:\Program Files\LLVM\bin"
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ((Test-Path $llvm) -and ($userPath -notlike "*$llvm*")) {
  [Environment]::SetEnvironmentVariable("Path", "$userPath;$llvm", "User")
  Write-Host "Added $llvm to your PATH."
}

Write-Host ""
Write-Host "==== Done. Close this window and open a new one so the new PATH is picked up." -ForegroundColor Green
Write-Host "Next: put the ISO and build_fh1.bat / run_fh1.bat (from fh1-recomp\tools) in one folder,"
Write-Host "then run build_fh1.bat."
