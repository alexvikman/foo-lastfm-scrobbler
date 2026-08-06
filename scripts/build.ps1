[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

# Some hosts inject both Path and PATH into the Windows environment block.
# MSBuild's .NET Framework tool runner treats those case-insensitive names as a
# duplicate dictionary key. Recreate the process-local variable once so cl.exe
# can be launched reliably; the user and machine environment are not changed.
$processPath = $env:Path
[Environment]::SetEnvironmentVariable('PATH', $null, [EnvironmentVariableTarget]::Process)
[Environment]::SetEnvironmentVariable('Path', $null, [EnvironmentVariableTarget]::Process)
[Environment]::SetEnvironmentVariable('Path', $processPath, [EnvironmentVariableTarget]::Process)

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$solutionPath = Join-Path $repositoryRoot 'foo_lastfm_scrobbler.sln'
$sdkHeader = Join-Path $repositoryRoot '.deps\sdk\foobar2000\SDK\foobar2000.h'
$localCredentials = Join-Path $repositoryRoot 'src\auth\api_credentials.local.h'
$credentialSource = Join-Path $repositoryRoot 'src\auth\app_credentials.cpp'

if (-not (Test-Path $sdkHeader)) {
    throw 'foobar2000 SDK is missing. Run scripts\bootstrap-sdk.ps1 first.'
}

# MSBuild cannot track a header that did not exist when __has_include was last
# evaluated. Force this one small translation unit to rebuild whenever local
# credentials are present so an old credential-free object cannot be reused.
if (Test-Path $localCredentials) {
    [System.IO.File]::SetLastWriteTimeUtc($credentialSource, [DateTime]::UtcNow)
}

$vsWhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
)
$vsWhere = $vsWhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vsWhere) {
    throw 'Visual Studio Installer could not be found.'
}

$installationPath = & $vsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installationPath) {
    throw 'A Visual Studio installation with MSBuild could not be found.'
}

$msBuild = Join-Path $installationPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msBuild)) {
    throw "MSBuild was not found at $msBuild."
}

Write-Host "Building $Configuration x64 with MSVC v143..."
& $msBuild $solutionPath /m:1 /nr:false /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /p:PlatformToolset=v143 /verbosity:minimal
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

if (-not $SkipTests) {
    $testExecutable = Join-Path $repositoryRoot "out\$Configuration\foo_lastfm_scrobbler_tests.exe"
    if (-not (Test-Path $testExecutable)) {
        throw "Test executable was not produced at $testExecutable."
    }

    Write-Host 'Running core tests...'
    & $testExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE."
    }
}

$componentPath = Join-Path $repositoryRoot "out\$Configuration\foo_lastfm_scrobbler.dll"
if (-not (Test-Path $componentPath)) {
    throw "Component DLL was not produced at $componentPath."
}

Write-Host "Build complete: $componentPath"
