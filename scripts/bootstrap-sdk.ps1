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
    # Windows PowerShell 5.1 still offers SSL 3.0/TLS 1.0 unless told otherwise.
    # A value of 0 means the operating system policy is already in charge.
    if ([int][Net.ServicePointManager]::SecurityProtocol -ne 0) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    }

    # Download beside the final path and promote the file only once its
    # checksum matches, so an interrupted or altered download is never left
    # where a later run would trust it.
    $downloadPath = "$archivePath.download"
    Write-Host "Downloading foobar2000 SDK $sdkVersion..."
    try {
        Invoke-WebRequest -Uri $sdkUrl -OutFile $downloadPath -UseBasicParsing
        $downloadHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
        if ($downloadHash -ne $expectedHash) {
            throw "SDK checksum mismatch. Expected $expectedHash but received $downloadHash."
        }
        Move-Item -LiteralPath $downloadPath -Destination $archivePath -Force
    } finally {
        if (Test-Path -LiteralPath $downloadPath) {
            Remove-Item -LiteralPath $downloadPath -Force
        }
    }
}

# A cached archive may predate this run, so it is verified as well.
$actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    Remove-Item -LiteralPath $archivePath -Force
    throw "SDK checksum mismatch. Expected $expectedHash but received $actualHash. The cached archive was removed; run this script again."
}

if (Test-Path $sdkDirectory) {
    Remove-Item -LiteralPath $sdkDirectory -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $sdkDirectory | Out-Null

# Prefer the libarchive tar that ships with Windows over whatever is first on
# PATH; GNU tar from other toolchains cannot read 7z archives either.
$tar = 'tar'
if ($env:SystemRoot) {
    $systemTar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path -LiteralPath $systemTar) {
        $tar = $systemTar
    }
}
& $tar -xf $archivePath -C $sdkDirectory
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $sdkHeader)) {
    throw 'The foobar2000 SDK archive could not be extracted.'
}

Write-Host "foobar2000 SDK $sdkVersion is ready."

