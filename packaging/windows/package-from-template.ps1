param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$TemplateArchive,
    [Parameter(Mandatory = $true)][string]$Flavor,
    [Parameter(Mandatory = $true)][string]$DistDirectory,
    [switch]$AssemblyOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($Flavor -notmatch '^nvidia-sm[0-9]+$') {
    throw "Only matching NVIDIA compute builds can use the NVIDIA template"
}
$TemplateArchive = (Resolve-Path $TemplateArchive).Path
$Executable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
$Worker = Join-Path $BuildDirectory "bin/forevertas-simulation-debug-worker.exe"
if (-not (Test-Path $Executable -PathType Leaf) -or
        -not (Test-Path $Worker -PathType Leaf)) {
    throw "The compute build is missing an executable"
}
$Cache = Get-Content (Join-Path $BuildDirectory "CMakeCache.txt") -Raw
if ($Cache -notmatch "(?m)^FOREVERTAS_UPDATE_ASSET_ID:INTERNAL=(windows-$Flavor-x86_64)\r?$") {
    throw "Compute build has the wrong update asset identity"
}
$AssetId = $Matches[1]
$Staging = Join-Path $BuildDirectory "runtime-template-stage"
try {
    Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $Staging | Out-Null
    Expand-Archive -LiteralPath $TemplateArchive -DestinationPath $Staging
    $Roots = @(Get-ChildItem -LiteralPath $Staging -Directory)
    if ($Roots.Count -ne 1) { throw "The template archive has no single root" }
    $Root = $Roots[0].FullName
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $Root "ForeverTAS.exe") -Force
    Copy-Item -LiteralPath $Worker -Destination (Join-Path $Root "forevertas-simulation-debug-worker.exe") -Force
    New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
    $Archive = Join-Path $DistDirectory ([IO.Path]::GetFileName($TemplateArchive))
    Remove-Item $Archive -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $Root -DestinationPath $Archive -CompressionLevel Optimal
    if ($AssemblyOnly) { return }
    & (Join-Path $PSScriptRoot "test-portable.ps1") -Archive $Archive
    & (Join-Path $PSScriptRoot "build-installer.ps1") `
        -Archive $Archive -AssetId $AssetId -DistDirectory $DistDirectory
} finally {
    Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
}
