[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$sdkVersion = '2025-03-07'
$sdkUrl = "https://www.foobar2000.org/downloads/SDK-$sdkVersion.7z"
$expectedHash = 'CCDA3C5840E66E0E28A7E4FE36407C4E78581AA30C40C362A188FCBAAE799A3E'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependencyDirectory = Join-Path $repositoryRoot '.deps'
$archivePath = Join-Path $dependencyDirectory "foobar2000-SDK-$sdkVersion.7z"
$sdkDirectory = Join-Path $dependencyDirectory 'sdk'
$sdkHeader = Join-Path $sdkDirectory 'foobar2000\SDK\foobar2000.h'

if ((Test-Path $sdkHeader) -and -not $Force) {
    Write-Host "foobar2000 SDK $sdkVersion is already available."
    exit 0
}

New-Item -ItemType Directory -Force -Path $dependencyDirectory | Out-Null

if ($Force -or -not (Test-Path $archivePath)) {
    Write-Host "Downloading foobar2000 SDK $sdkVersion..."
    Invoke-WebRequest -Uri $sdkUrl -OutFile $archivePath
}

$actualHash = (Get-FileHash -Path $archivePath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    throw "SDK checksum mismatch. Expected $expectedHash but received $actualHash."
}

if (Test-Path $sdkDirectory) {
    Remove-Item -LiteralPath $sdkDirectory -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $sdkDirectory | Out-Null
tar -xf $archivePath -C $sdkDirectory
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $sdkHeader)) {
    throw 'The foobar2000 SDK archive could not be extracted.'
}

Write-Host "foobar2000 SDK $sdkVersion is ready."

