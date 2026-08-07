[CmdletBinding()]
param(
    [string]$Manifest = "",
    [switch]$LastResortRebuildCache,
    [switch]$ConfirmCacheRecoveryExhausted
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($Manifest)) {
    $Manifest = Join-Path $PSScriptRoot "manifest.json"
}
$Release = Get-Content (Resolve-Path $Manifest) -Raw | ConvertFrom-Json
$ExpectedCubinArchitectures = "61,62,70,72,75,80,86,87,89,90,100,101,120"
$ExpectedCudaArchitectures = "61-real;62-real;70-real;72-real;75-real;80-real;86-real;87-real;89-real;90-real;100-real;101-real;120-real;120-virtual"
$ExpectedArchitectureKey = "sm61-sm62-sm70-sm72-sm75-sm80-sm86-sm87-sm89-sm90-sm100-sm101-sm120-ptx120"
if ($Release.cuda.version -ne "12.8.1" -or
        ($Release.cuda.architectures -join ",") -cne
            $ExpectedCubinArchitectures -or
        $Release.cuda.ptx_architecture -ne 120 -or
        $Release.cuda.cmake_architectures -cne
            $ExpectedCudaArchitectures -or
        $Release.cuda.architecture_key -cne $ExpectedArchitectureKey -or
        $Release.cuda.split_compile_jobs -ne 4) {
    throw "The manifest does not match the validated CUDA release contract"
}
if ($Release.sources.forevervalidator.commit -cnotmatch
        '^[0-9a-f]{40}$' -or
        $Release.sources.forevervalidator.commit -cne
            $Release.cuda.search_object_source_commit) {
    throw "ForeverValidator release identities must be one exact lowercase SHA"
}

. C:\Tools\Enter-BuildEnv.ps1
$env:CUDA_VERSION = $Release.cuda.version
$env:FOREVERTAS_VERSION = $Release.release.version
$env:CUDA_ARCHITECTURES = $Release.cuda.cmake_architectures
$env:CUDA_ARCHITECTURE_KEY = $Release.cuda.architecture_key
$env:FOREVERVALIDATOR_COMMIT = $Release.sources.forevervalidator.commit
$env:FOREVERVALIDATOR_CUDA_SEARCH_SOURCE_COMMIT = $Release.cuda.search_object_source_commit
$env:VCPKG_COMMIT = $Release.toolchains.windows.vcpkg_commit
$env:FOREVERTAS_CACHE_ROOT = $Release.cache.windows
$env:FOREVERVALIDATOR_CUDA_SPLIT_COMPILE_JOBS = [string]$Release.cuda.split_compile_jobs

if ($LastResortRebuildCache -ne $ConfirmCacheRecoveryExhausted) {
    throw "A full cache rebuild requires both last-resort confirmation switches"
}
if ($LastResortRebuildCache -and (Test-Path $env:FOREVERTAS_CACHE_ROOT)) {
    Remove-Item $env:FOREVERTAS_CACHE_ROOT -Recurse -Force
}
New-Item -ItemType Directory -Force $env:FOREVERTAS_CACHE_ROOT | Out-Null

. (Join-Path $PSScriptRoot "ensure-windows-cuda.ps1")
. (Join-Path $PSScriptRoot "ensure-windows-dependencies.ps1")

$Sccache = Get-Command sccache.exe -ErrorAction SilentlyContinue
if (-not $Sccache) {
    throw "sccache.exe is missing; rebuild the VM with the current provisioner"
}
$env:SCCACHE_PATH = $Sccache.Source

& (Join-Path $PSScriptRoot "package-windows.ps1")
if ($LASTEXITCODE -ne 0) { throw "Windows packaging failed" }
if (-not (Test-Path (Join-Path $RepoRoot "dist/cuda-fatbinary-windows.json"))) {
    throw "Windows CUDA fatbinary evidence is missing"
}
Write-Host "PASS Windows local release build ($($(if ($LastResortRebuildCache) { 'last-resort-rebuilt' } else { 'warm' })) cache)"
