$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($env:FOREVERTAS_CACHE_ROOT)) {
    throw "FOREVERTAS_CACHE_ROOT is required"
}
if ([string]::IsNullOrWhiteSpace($env:VCPKG_COMMIT)) {
    throw "VCPKG_COMMIT is required"
}

$CacheRoot = $env:FOREVERTAS_CACHE_ROOT
$VcpkgRoot = Join-Path $CacheRoot "vcpkg"
$BinaryCache = Join-Path $CacheRoot "vcpkg-binary-cache"
$SccacheDirectory = Join-Path $CacheRoot "sccache-windows"
$SearchCache = Join-Path $CacheRoot "cuda-search-windows"
New-Item -ItemType Directory -Force `
    -Path $CacheRoot, $BinaryCache, $SccacheDirectory, $SearchCache | Out-Null

if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone vcpkg" }
}

git -C $VcpkgRoot fetch --depth 1 origin $env:VCPKG_COMMIT
if ($LASTEXITCODE -ne 0) { throw "Failed to fetch pinned vcpkg commit" }
git -C $VcpkgRoot checkout --detach --force $env:VCPKG_COMMIT
if ($LASTEXITCODE -ne 0) { throw "Failed to check out pinned vcpkg commit" }

$Vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
$VcpkgMarker = Join-Path $VcpkgRoot ".forevertas-bootstrap-commit"
$BootstrapCurrent = (Test-Path $Vcpkg) -and (Test-Path $VcpkgMarker) -and
    ((Get-Content $VcpkgMarker -Raw).Trim() -eq $env:VCPKG_COMMIT)
if (-not $BootstrapCurrent) {
    Remove-Item $Vcpkg -Force -ErrorAction SilentlyContinue
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "Failed to bootstrap vcpkg" }
    Set-Content -NoNewline -Path $VcpkgMarker -Value $env:VCPKG_COMMIT
}

$env:VCPKG_BINARY_SOURCES = "clear;files,$BinaryCache,readwrite"
& $Vcpkg install --triplet x64-windows openssl zlib vulkan
if ($LASTEXITCODE -ne 0) { throw "Failed to install Windows dependencies" }

$env:VCPKG_INSTALLATION_ROOT = $VcpkgRoot
$env:SCCACHE_DIR = $SccacheDirectory
$env:SCCACHE_CACHE_SIZE = "50G"
$env:FOREVERTAS_WINDOWS_SEARCH_CACHE = $SearchCache
