#requires -Version 7
<#
.SYNOPSIS
    Fetch the ONNX Runtime + DirectML dependencies the native engine needs.

.DESCRIPTION
    Downloads the two NuGet packages below into _dl/nuget/, verifies their
    pinned SHA-256 hashes, then extracts:

      third_party/onnxruntime/include/*.h   C API headers (committed)
      third_party/onnxruntime/LICENSE*      license/notices (committed)
      _dl/runtime/onnxruntime.dll           x64 runtime (not committed)
      _dl/runtime/onnxruntime_providers_shared.dll
      _dl/runtime/DirectML.dll              x64 (not committed)

    The app loads onnxruntime.dll dynamically at runtime (no import lib), so
    only the headers and the two DLLs are needed at build/run time.

    Run from anywhere; safe to re-run (cached downloads are re-verified):

        pwsh -NoProfile -File tools/fetch_deps.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dlDir = Join-Path $root '_dl\nuget'
$rtDir = Join-Path $root '_dl\runtime'
$includeDir = Join-Path $root 'third_party\onnxruntime\include'

$packages = @(
    @{
        Id      = 'Microsoft.ML.OnnxRuntime.DirectML'
        Version = '1.24.4'
        Sha256  = '57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681'
        File    = 'ortdml.nupkg'
    },
    @{
        Id      = 'Microsoft.AI.DirectML'
        Version = '1.15.4'
        Sha256  = '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9'
        File    = 'dml.nupkg'
    }
)

New-Item -ItemType Directory -Force $dlDir, $rtDir, $includeDir | Out-Null

foreach ($p in $packages) {
    $url = "https://www.nuget.org/api/v2/package/$($p.Id)/$($p.Version)"
    $dest = Join-Path $dlDir $p.File
    if (-not (Test-Path -LiteralPath $dest)) {
        Write-Host "downloading $($p.Id) $($p.Version) ..."
        Invoke-WebRequest -Uri $url -OutFile $dest
    }
    $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $p.Sha256) {
        Remove-Item -LiteralPath $dest -Force
        throw "checksum mismatch for $($p.File) from $url; deleted"
    }
    Write-Host "$($p.File) ok ($hash)"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Expand-Member([string]$pkg, [string]$member, [string]$outFile) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($pkg)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $member } | Select-Object -First 1
        if (-not $entry) { throw "$member not found in $pkg" }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $outFile, $true)
    } finally {
        $zip.Dispose()
    }
}

$ort = Join-Path $dlDir 'ortdml.nupkg'
$dml = Join-Path $dlDir 'dml.nupkg'

# Committed: C API headers + license texts.
$zip = [System.IO.Compression.ZipFile]::OpenRead($ort)
try {
    foreach ($e in $zip.Entries) {
        if ($e.FullName -like 'build/native/include/*.h' -and $e.Name) {
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile(
                $e, (Join-Path $includeDir $e.Name), $true)
        }
    }
} finally {
    $zip.Dispose()
}
Expand-Member $ort 'LICENSE'               (Join-Path $root 'third_party\onnxruntime\LICENSE.txt')
Expand-Member $ort 'ThirdPartyNotices.txt' (Join-Path $root 'third_party\onnxruntime\ThirdPartyNotices.txt')
Expand-Member $dml 'LICENSE.txt'           (Join-Path $root 'third_party\onnxruntime\LICENSE-DirectML.txt')
Expand-Member $dml 'ThirdPartyNotices.txt' (Join-Path $root 'third_party\onnxruntime\ThirdPartyNotices-DirectML.txt')

# Not committed: the runtime DLLs the build copies next to the exe.
Expand-Member $ort 'runtimes/win-x64/native/onnxruntime.dll'                  (Join-Path $rtDir 'onnxruntime.dll')
Expand-Member $ort 'runtimes/win-x64/native/onnxruntime_providers_shared.dll' (Join-Path $rtDir 'onnxruntime_providers_shared.dll')
Expand-Member $dml 'bin/x64-win/DirectML.dll'                                 (Join-Path $rtDir 'DirectML.dll')

Write-Host "headers -> $includeDir"
Write-Host "dlls    -> $rtDir"
