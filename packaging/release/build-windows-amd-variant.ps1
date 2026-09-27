param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$DistDirectory,
    [string]$PrebuiltValidatorDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$Name = "amd-rx7000-rx9000"
$Release = Get-Content (Join-Path $RepoRoot "packaging/release/manifest.json") -Raw |
    ConvertFrom-Json
$BuildDirectory = Join-Path $RepoRoot "build/distribution-$Name"
$TemporaryDist = Join-Path $RepoRoot "build/distribution-$Name-dist"
$Options = @{
    BuildDirectory = $BuildDirectory
    DistDirectory = $TemporaryDist
    RuntimeDirectory = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/x64-windows/bin"
    Flavor = $Name
    HipArchitectures = ($Release.distribution.amd_gfx -join ";")
    HipPlatform = "amd"
    ExternalQmlModule = $true
    CommonQmlDirectory = Join-Path $RepoRoot "build/distribution-universal/bin/qml/ForeverTAS"
    PrebuiltValidatorDirectory = $PrebuiltValidatorDirectory
}
& (Join-Path $RepoRoot "packaging/windows/build-portable.ps1") @Options

$InstallerName = "ForeverTAS-$Version-windows-$Name-x86_64-Setup.exe"
foreach ($Filename in @($InstallerName, "$InstallerName.sha256")) {
    $Source = Join-Path $TemporaryDist $Filename
    if (-not (Test-Path $Source -PathType Leaf)) { throw "Missing $Filename" }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $DistDirectory $Filename) -Force
}
$Evidence = [ordered]@{
    sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $DistDirectory $InstallerName)).Hash.ToLowerInvariant()
    binary_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $BuildDirectory "bin/ForeverTAS.exe")).Hash.ToLowerInvariant()
    runtime_template = $Name
}
$Evidence | ConvertTo-Json -Compress |
    Set-Content -Encoding UTF8 (Join-Path $TemporaryDist "variant-evidence.json")
