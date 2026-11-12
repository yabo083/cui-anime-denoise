#requires -Version 7
<#
.SYNOPSIS
    Build the release executable, stage the payload and compile the Inno Setup
    installer.

.DESCRIPTION
    1. Resolves the version (-Version, otherwise CMakeLists.txt's project VERSION).
    2. Locates ISCC.exe; exits with code 2 and installation instructions when
       Inno Setup 6 is not installed (nothing is installed automatically).
    3. Runs ..\build.ps1 (Release) unless -SkipBuild.
    4. Stages AnimeDenoise.exe, anime-denoise.exe, runtime\, the shipped subset
       of models\, assets\ and the docs into dist\stage and verifies the
       required files are present.
    5. Runs ISCC.exe with /DAppVersion=<version> and reports the setup size.

    Result: dist\AnimeDenoise-<version>-setup.exe

    PREREQUISITES (run once per machine, their outputs land in build\):
      pwsh -NoProfile -File tools\fetch_deps.ps1
        downloads the pinned ONNX Runtime + DirectML NuGet packages and
        esrgan_anime6b_x4.onnx into build\runtime and build\models.
      py tools\export_onnx.py (or the equivalent mise task)
        regenerates models\*.skel.onnx; the skeletons are committed, so this
        is only needed when the model set changes.

    models\ staging is selective on purpose: esrgan_anime6b_x4.onnx and the
    *.skel.onnx skeletons ship; the full-weight *.onnx blobs are export
    intermediates the engine never loads and would only bloat the payload.

.EXAMPLE
    pwsh -NoProfile -File installer\build_installer.ps1

.EXAMPLE
    pwsh -NoProfile -File installer\build_installer.ps1 -Version 1.2.0 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$installerDir = $PSScriptRoot
$root = Split-Path -Parent $installerDir
$buildScript = Join-Path $root 'build.ps1'
$cmakeLists = Join-Path $root 'CMakeLists.txt'
$buildDir = Join-Path $root 'build'
$exePath = Join-Path $buildDir 'AnimeDenoise.exe'
$distDir = Join-Path $root 'dist'
$stageDir = Join-Path $distDir 'stage'
$issPath = Join-Path $installerDir 'AnimeDenoise.iss'

function Get-ProjectVersion {
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $m = [regex]::Match($text, '(?m)^\s*VERSION\s+([0-9]+(?:\.[0-9]+){1,3})\s*$')
    if (-not $m.Success) {
        throw "Cannot find the project VERSION in $cmakeLists; pass -Version explicitly."
    }
    return $m.Groups[1].Value
}

function Find-Iscc {
    $candidates = @()
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    if ($env:ISCC) { $candidates += $env:ISCC }
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    }
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    }
    if ($env:LOCALAPPDATA) {
        $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return $c }
    }
    return $null
}

function New-StageDirectory {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

if (-not $Version) { $Version = Get-ProjectVersion }
if ($Version -notmatch '^[0-9]+(\.[0-9]+){1,3}$') {
    throw "Invalid version '$Version' (expected x.y.z)."
}

# --- ISCC first: fail fast with instructions instead of after a long build. --
$iscc = Find-Iscc
if (-not $iscc) {
    Write-Host ''
    Write-Host 'ISCC.exe (Inno Setup 6 compiler) was not found.' -ForegroundColor Yellow
    Write-Host 'Install Inno Setup 6, then re-run this script:'
    Write-Host ''
    Write-Host '  1. Download the installer from https://jrsoftware.org/isdl.php'
    Write-Host '  2. Install it (the default location is fine):'
    Write-Host '       %ProgramFiles(x86)%\Inno Setup 6\'
    Write-Host '  3. Either leave it there, add it to PATH, or point the ISCC'
    Write-Host '     environment variable at ISCC.exe.'
    Write-Host ''
    Write-Host 'This script does not install software for you.' -ForegroundColor Yellow
    exit 2
}

# --- Build ------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "Building Release via build.ps1 ..." -ForegroundColor Cyan
    & pwsh -NoProfile -File $buildScript
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Missing build output: $exePath (run without -SkipBuild)."
}

# --- Stage ------------------------------------------------------------------
Write-Host "Staging $Version into dist\stage ..." -ForegroundColor Cyan
New-StageDirectory -Path $stageDir

Copy-Item -LiteralPath $exePath -Destination (Join-Path $stageDir 'AnimeDenoise.exe')
$cliPath = Join-Path $buildDir 'anime-denoise.exe'
if (Test-Path -LiteralPath $cliPath -PathType Leaf) {
    Copy-Item -LiteralPath $cliPath -Destination (Join-Path $stageDir 'anime-denoise.exe')
}
Copy-Item -LiteralPath (Join-Path $buildDir 'runtime') -Destination (Join-Path $stageDir 'runtime') -Recurse

# models\: ship esrgan + the weightless skeletons; the full *.onnx blobs are
# export intermediates the engine never loads (CUGAN/APISR run skel + the
# user-downloaded .pth).
New-Item -ItemType Directory -Path (Join-Path $stageDir 'models') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir 'models\esrgan_anime6b_x4.onnx') `
           -Destination (Join-Path $stageDir 'models\esrgan_anime6b_x4.onnx')
Get-ChildItem -LiteralPath (Join-Path $buildDir 'models') -Filter '*.skel.onnx' -File |
    Copy-Item -Destination (Join-Path $stageDir 'models')

Copy-Item -LiteralPath (Join-Path $root 'assets') -Destination (Join-Path $stageDir 'assets') -Recurse

# licenses that live under third_party\ in the repo must ride along in the
# installed layout or the paths in THIRD-PARTY-NOTICES.txt dangle
$licDir = Join-Path $stageDir 'assets\licenses'
New-Item -ItemType Directory -Path $licDir -Force | Out-Null
$licenseFiles = @{
    'third_party\onnxruntime\LICENSE.txt'                      = 'LICENSE-onnxruntime.txt'
    'third_party\onnxruntime\ThirdPartyNotices.txt'            = 'ThirdPartyNotices-onnxruntime.txt'
    'third_party\onnxruntime\LICENSE-DirectML.txt'             = 'LICENSE-DirectML.txt'
    'third_party\onnxruntime\ThirdPartyNotices-DirectML.txt'   = 'ThirdPartyNotices-DirectML.txt'
    'third_party\pocketfft\LICENSE.md'                         = 'LICENSE-pocketfft.md'
    'third_party\CUI\LICENSE'                                  = 'LICENSE-CUI.txt'
}
foreach ($src in $licenseFiles.GetEnumerator()) {
    $from = Join-Path $root $src.Key
    if (Test-Path -LiteralPath $from -PathType Leaf) {
        Copy-Item -LiteralPath $from -Destination (Join-Path $licDir $src.Value)
    } else {
        Write-Host "  note: $($src.Key) not present, skipped" -ForegroundColor DarkGray
    }
}

foreach ($doc in @('README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.txt')) {
    $src = Join-Path $root $doc
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $stageDir $doc)
    } else {
        Write-Host "  note: $doc not present, skipped" -ForegroundColor DarkGray
    }
}

$required = @(
    (Join-Path $stageDir 'AnimeDenoise.exe'),
    (Join-Path $stageDir 'anime-denoise.exe'),
    (Join-Path $stageDir 'runtime\onnxruntime.dll'),
    (Join-Path $stageDir 'runtime\onnxruntime_providers_shared.dll'),
    (Join-Path $stageDir 'runtime\DirectML.dll'),
    (Join-Path $stageDir 'models\esrgan_anime6b_x4.onnx'),
    (Join-Path $stageDir 'models\cugan_up2x_denoise3x.skel.onnx'),
    (Join-Path $stageDir 'models\apisr_rrdb_x2.skel.onnx'),
    (Join-Path $stageDir 'assets'),
    (Join-Path $stageDir 'THIRD-PARTY-NOTICES.txt')
)
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missing.Count -gt 0) {
    throw ("Staging is incomplete; missing: " + ($missing -join ', '))
}
$assetCount = @(Get-ChildItem -LiteralPath (Join-Path $stageDir 'assets') -Recurse -File).Count
if ($assetCount -eq 0) {
    throw 'Staging is incomplete; assets\ is empty.'
}

# --- Compile ----------------------------------------------------------------
Write-Host "Compiling installer with $iscc ..." -ForegroundColor Cyan
Push-Location $installerDir
try {
    & $iscc "/DAppVersion=$Version" $issPath
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$setupPath = Join-Path $distDir "AnimeDenoise-$Version-setup.exe"
if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
    throw "ISCC reported success but $setupPath is missing."
}

$size = Get-Item -LiteralPath $setupPath
$sizeMb = [math]::Round($size.Length / 1MB, 2)
Write-Host ''
Write-Host ("Built {0} ({1} MB, {2:N0} bytes)" -f $setupPath, $sizeMb, $size.Length) -ForegroundColor Green
exit 0
