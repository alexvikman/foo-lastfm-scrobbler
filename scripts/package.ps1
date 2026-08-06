[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$componentPath = Join-Path $repositoryRoot "out\$Configuration\foo_lastfm_scrobbler.dll"
$packagePath = Join-Path $repositoryRoot "out\$Configuration\foo_lastfm_scrobbler.fb2k-component"
$licensePath = Join-Path $repositoryRoot 'LICENSE'
$thirdPartyPath = Join-Path $repositoryRoot 'THIRD_PARTY.md'
$sdkLicensePath = Join-Path $repositoryRoot 'LICENSES\foobar2000-sdk.txt'
$pfcLicensePath = Join-Path $repositoryRoot 'LICENSES\pfc.txt'

$packageFiles = @(
    @{ Path = $componentPath; Entry = 'x64/foo_lastfm_scrobbler.dll' },
    @{ Path = $licensePath; Entry = 'x64/LICENSE.txt' },
    @{ Path = $thirdPartyPath; Entry = 'x64/THIRD_PARTY.md' },
    @{ Path = $sdkLicensePath; Entry = 'x64/LICENSES/foobar2000-sdk.txt' },
    @{ Path = $pfcLicensePath; Entry = 'x64/LICENSES/pfc.txt' }
)

foreach ($file in $packageFiles) {
    if (-not (Test-Path -LiteralPath $file.Path)) {
        throw "Package input was not found at $($file.Path)."
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (Test-Path -LiteralPath $packagePath) {
    Remove-Item -LiteralPath $packagePath -Force
}

$archive = [System.IO.Compression.ZipFile]::Open(
    $packagePath,
    [System.IO.Compression.ZipArchiveMode]::Create
)
try {
    foreach ($file in $packageFiles) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.Path,
            $file.Entry,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $archive.Dispose()
}

Write-Host "Package complete: $packagePath"
