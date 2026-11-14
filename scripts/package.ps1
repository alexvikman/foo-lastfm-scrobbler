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

# A release must not disclose the build machine's directory layout or account
# name, and it must carry every exploit mitigation the Release configuration
# asks for. Refuse to package a DLL that was linked any other way.
if ($Configuration -eq 'Release') {
    $componentBytes = [System.IO.File]::ReadAllBytes($componentPath)
    $componentText = [System.Text.Encoding]::GetEncoding(28591).GetString($componentBytes)
    if ($componentText -match '[A-Za-z]:\\[\x20-\x7e]+\.pdb') {
        throw "The component embeds an absolute PDB path ($($Matches[0])). Rebuild it with scripts\build.ps1 before packaging."
    }

    # PE32+ layout: the optional header follows the 24-byte signature and COFF
    # header. DllCharacteristics is 70 bytes into it, and the debug directory
    # entry 160 bytes into it.
    $peHeader = [System.BitConverter]::ToInt32($componentBytes, 0x3C)
    $optionalHeader = $peHeader + 24
    $dllCharacteristics = [System.BitConverter]::ToUInt16($componentBytes, $optionalHeader + 70)

    $missing = @()
    if (-not ($dllCharacteristics -band 0x0020)) { $missing += 'high-entropy ASLR' }
    if (-not ($dllCharacteristics -band 0x0040)) { $missing += 'ASLR' }
    if (-not ($dllCharacteristics -band 0x0100)) { $missing += 'DEP' }
    if (-not ($dllCharacteristics -band 0x4000)) { $missing += 'Control Flow Guard' }

    # CET compatibility is recorded in a debug directory entry of type 20.
    $sectionCount = [System.BitConverter]::ToUInt16($componentBytes, $peHeader + 6)
    $sectionTable = $optionalHeader + [System.BitConverter]::ToUInt16($componentBytes, $peHeader + 20)
    $debugAddress = [System.BitConverter]::ToUInt32($componentBytes, $optionalHeader + 160)
    $debugSize = [System.BitConverter]::ToUInt32($componentBytes, $optionalHeader + 164)
    $cetCompatible = $false
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $section = $sectionTable + 40 * $index
        $sectionAddress = [System.BitConverter]::ToUInt32($componentBytes, $section + 12)
        $sectionSize = [System.BitConverter]::ToUInt32($componentBytes, $section + 16)
        if ($debugAddress -lt $sectionAddress -or $debugAddress -ge $sectionAddress + $sectionSize) {
            continue
        }

        $debugOffset = $debugAddress - $sectionAddress +
            [System.BitConverter]::ToUInt32($componentBytes, $section + 20)
        for ($entry = $debugOffset; $entry -lt $debugOffset + $debugSize; $entry += 28) {
            if ([System.BitConverter]::ToUInt32($componentBytes, $entry + 12) -eq 20) {
                $data = [System.BitConverter]::ToUInt32($componentBytes, $entry + 24)
                $cetCompatible =
                    ([System.BitConverter]::ToUInt32($componentBytes, $data) -band 1) -ne 0
            }
        }
    }
    if (-not $cetCompatible) { $missing += 'CET shadow-stack compatibility' }

    if ($missing.Count -gt 0) {
        throw "The component was linked without: $($missing -join ', '). Rebuild it with scripts\build.ps1 before packaging."
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
