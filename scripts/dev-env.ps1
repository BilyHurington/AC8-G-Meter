# Imports the MSVC x64 build environment (cl, link, Windows SDK, VS-bundled CMake + Ninja)
# into the current PowerShell session. Dot-source it:  . .\scripts\dev-env.ps1
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 or its Build Tools" }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the MSVC x64 toolset found" }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
# vcvars64.bat calls vswhere by name, so it must be on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
Write-Host "MSVC environment loaded from $vsPath (Windows SDK $env:WindowsSDKVersion)"
