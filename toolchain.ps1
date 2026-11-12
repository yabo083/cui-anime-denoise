#requires -Version 7
<#
.SYNOPSIS
    Locate and activate the Windows C toolchain (MSVC x64 + cmake + ninja).

.DESCRIPTION
    Dot-source this file to activate the toolchain in the current PowerShell session:

        . "$PSScriptRoot/toolchain.ps1"

    Or run it standalone to see what it resolved:

        pwsh -NoProfile -File toolchain.ps1

    How vcvars is activated
    -----------------------
    vcvarsall.bat is a cmd.exe script and cannot be "called" from PowerShell, and its
    environment is lost as soon as the cmd process exits. The reliable trick (used here)
    is to run one cmd.exe that calls vcvarsall and then dumps its environment with `set`,
    then copy every NAME=VALUE line back into the PowerShell process environment. That is
    the same state a "Developer Command Prompt" would have, and it makes cmake invoked
    from PowerShell see cl.exe, link.exe, INCLUDE and LIB.

    For POSIX shells (git-bash / brush) do not try to import this environment: the
    captured PATH holds Windows entries, and neither "C:/x/y" nor "/c/x/y" entries
    resolve when exported that way. A POSIX shell should drive the build through
    pwsh instead:  pwsh -NoProfile -File build.ps1   (verified working).

.PARAMETER Vcvarsall
    Explicit path to vcvarsall.bat. Default: found through vswhere.

.PARAMETER Force
    Re-activate even when cl.exe is already on PATH.

.PARAMETER Quiet
    Do not print the resolution summary.
#>
[CmdletBinding()]
param(
    [string]$Vcvarsall,
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($path) { return ($path | Select-Object -First 1).Trim() }
        $path = & $vswhere -latest -products '*' -property installationPath 2>$null
        if ($path) { return ($path | Select-Object -First 1).Trim() }
    }
    # Known non-standard location on this machine.
    foreach ($candidate in @('E:\CodeTools\Microsoft Visual Studio', 'C:\Program Files\Microsoft Visual Studio\2022\Community')) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'VC\Auxiliary\Build\vcvarsall.bat')) { return $candidate }
    }
    return $null
}

function Get-ToolchainSummary {
    [pscustomobject]@{
        Compiler = (Get-Command cl.exe -ErrorAction SilentlyContinue)?.Source
        Linker   = (Get-Command link.exe -ErrorAction SilentlyContinue)?.Source
        CMake    = (Get-Command cmake.exe -ErrorAction SilentlyContinue)?.Source
        Ninja    = (Get-Command ninja.exe -ErrorAction SilentlyContinue)?.Source
    }
}

$alreadyActive = $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)

if (-not $alreadyActive -or $Force) {
    if (-not $Vcvarsall) {
        $vs = Find-VsInstall
        if (-not $vs) { throw 'Visual Studio with the C++ toolset was not found (vswhere found nothing).' }
        $Vcvarsall = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
        if (-not (Test-Path -LiteralPath $Vcvarsall)) {
            $Vcvarsall = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
        }
        if (-not (Test-Path -LiteralPath $Vcvarsall)) {
            throw "vcvarsall.bat / vcvars64.bat not found under '$vs'."
        }
    }

    # One cmd.exe: activate x64, then dump the resulting environment.
    $dump = & cmd.exe /c "call `"$Vcvarsall`" x64 >NUL && set"
    if ($LASTEXITCODE -ne 0 -or -not $dump) { throw "vcvarsall failed: '$Vcvarsall' x64" }

    $count = 0
    foreach ($line in $dump) {
        $line = $line.TrimEnd("`r")
        if ($line -notmatch '^([^=]+)=(.*)$') { continue }   # skips "*** Using ..." banners
        $name = $Matches[1]
        if ($name.StartsWith('=')) { continue }              # cmd's =C:=... drive cwd vars
        [Environment]::SetEnvironmentVariable($name, $Matches[2], 'Process')
        $count++
    }
    if ($count -eq 0) { throw "vcvarsall produced no environment: '$Vcvarsall'" }

    # cmake + ninja: prefer the copies bundled with Visual Studio, else one already on PATH.
    # vcvarsall.bat -> Build -> Auxiliary -> VC -> <VS>
    $vs = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Vcvarsall)))
    $cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    $ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $pathDirs = @()
    if (Test-Path -LiteralPath (Join-Path $cmakeDir 'cmake.exe')) { $pathDirs += $cmakeDir }
    if (Test-Path -LiteralPath (Join-Path $ninjaDir 'ninja.exe')) { $pathDirs += $ninjaDir }
    foreach ($dir in $pathDirs) {
        if (($env:PATH -split ';') -notcontains $dir) { $env:PATH = "$dir;$env:PATH" }
    }
}

if (-not $Quiet) {
    $summary = Get-ToolchainSummary
    Write-Host 'toolchain.ps1: MSVC x64 environment active'
    foreach ($k in 'Compiler', 'Linker', 'CMake', 'Ninja') {
        Write-Host ("  {0,-9} {1}" -f $k, $(if ($summary.$k) { $summary.$k } else { '<missing>' }))
    }
    Write-Host ("  version   MSVC {0} / Windows SDK {1}" -f $env:VCToolsVersion, $env:WindowsSDKVersion)
}
