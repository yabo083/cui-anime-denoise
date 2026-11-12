#requires -Version 7
<#
.SYNOPSIS
    Configure and build denoise_app (Ninja single-config, Release by default).

.DESCRIPTION
    Activates the MSVC x64 toolchain through toolchain.ps1, configures with CMake if the
    build directory is missing or the build type changed, then builds.

    Works from any shell, including POSIX bash:
        pwsh -NoProfile -File build.ps1

.EXAMPLE
    pwsh -NoProfile -File build.ps1

.EXAMPLE
    pwsh -NoProfile -File build.ps1 -Config Debug -Clean -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'
$stampFile = Join-Path $buildDir '.build-config'

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

# Activates MSVC x64 (vcvarsall capture) and puts cmake/ninja on this process's PATH.
. (Join-Path $root 'toolchain.ps1') -Quiet

$stamp = if (Test-Path -LiteralPath $stampFile) { (Get-Content -LiteralPath $stampFile -Raw).Trim() } else { '' }
$needConfigure = -not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -or $stamp -ne $Config

if ($needConfigure) {
    cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Set-Content -LiteralPath $stampFile -Value $Config -Encoding utf8NoBOM
}

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $buildDir 'denoise_app.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build reported success but '$exe' is missing." }

$size = [math]::Round((Get-Item -LiteralPath $exe).Length / 1KB)
Write-Host "`nBuilt $exe ($size KB, $Config)" -ForegroundColor Green

if ($Run) { & $exe }
