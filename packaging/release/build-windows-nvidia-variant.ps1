param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$TemplateArchive,
    [Parameter(Mandatory = $true)][string]$DistDirectory,
    [string]$PrebuiltValidatorDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ($Name -notmatch '^nvidia-sm([0-9]+)$') { throw "Invalid NVIDIA flavor" }
$Sm = $Matches[1]
$BuildDirectory = Join-Path $RepoRoot "build/distribution-$Name"
$TemporaryDist = Join-Path $RepoRoot "build/distribution-$Name-dist"
$Options = @{
    BuildDirectory = $BuildDirectory
    DistDirectory = $TemporaryDist
    RuntimeDirectory = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/x64-windows/bin"
    Flavor = $Name
    CudaArchitectures = "$Sm-real;$Sm-virtual"
    HipArchitectures = $Sm
    HipPlatform = "nvidia"
    ExternalQmlModule = $true
    CommonQmlDirectory = Join-Path $RepoRoot "build/distribution-universal/bin/qml/ForeverTAS"
    PrebuiltValidatorDirectory = $PrebuiltValidatorDirectory
    BuildOnly = $true
}
& (Join-Path $RepoRoot "packaging/windows/build-portable.ps1") @Options
& (Join-Path $RepoRoot "packaging/windows/package-from-template.ps1") `
    -BuildDirectory $BuildDirectory -TemplateArchive $TemplateArchive `
    -Flavor $Name -DistDirectory $TemporaryDist

$Executable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
$Cubins = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $Executable 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "CUDA inspection failed for $Name" }
$Architectures = @([regex]::Matches($Cubins, 'sm_([0-9]+)\.cubin') |
    ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
if ($Architectures.Count -ne 1 -or $Architectures[0] -ne $Sm) {
    throw "$Name has unexpected CUDA cubins: $($Architectures -join ', ')"
}
$InstallerName = "ForeverTAS-$Version-windows-$Name-x86_64-Setup.exe"
foreach ($Filename in @($InstallerName, "$InstallerName.sha256")) {
    $Source = Join-Path $TemporaryDist $Filename
    if (-not (Test-Path $Source -PathType Leaf)) { throw "Missing $Filename" }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $DistDirectory $Filename) -Force
}
$Evidence = [ordered]@{
    sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $DistDirectory $InstallerName)).Hash.ToLowerInvariant()
    binary_sha256 = (Get-FileHash -Algorithm SHA256 $Executable).Hash.ToLowerInvariant()
    runtime_template = "nvidia-sm75"
}
$Evidence | ConvertTo-Json -Compress |
    Set-Content -Encoding UTF8 (Join-Path $TemporaryDist "variant-evidence.json")
