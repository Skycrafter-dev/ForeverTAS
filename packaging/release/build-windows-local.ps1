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
if ($Release.cuda.version -ne "12.8.1" -or
        $Release.cuda.split_compile_jobs -ne 4) {
    throw "The Windows release requires CUDA 12.8.1 and split-compile 4"
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
