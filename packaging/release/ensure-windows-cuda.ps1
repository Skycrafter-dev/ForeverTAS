$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($env:FOREVERTAS_CACHE_ROOT)) {
    throw "FOREVERTAS_CACHE_ROOT is required"
}
if ($env:CUDA_VERSION -ne "12.8.1") {
    throw "The portable CUDA manifest is pinned to CUDA 12.8.1"
}

$CacheRoot = Join-Path $env:FOREVERTAS_CACHE_ROOT "cuda"
$CudaRoot = Join-Path $CacheRoot $env:CUDA_VERSION
$DownloadRoot = Join-Path $CacheRoot "downloads/$env:CUDA_VERSION"
$CompleteMarker = Join-Path $CudaRoot ".complete"
$BaseUrl = "https://developer.download.nvidia.com/compute/cuda/redist"
$Components = @(
    @{
        Path = "cuda_cccl/windows-x86_64/cuda_cccl-windows-x86_64-12.8.90-archive.zip"
        Sha256 = "bd8548fa1ae82f92910bebc3079e14bd58c5a92aa64596d46bd610a478cb39d7"
    },
    @{
        Path = "cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.8.90-archive.zip"
        Sha256 = "4a39058fd8519444a81cfc7ae055d136f48d1a31ffa41ae255b35b2edd61e13b"
    },
    @{
        Path = "cuda_cuobjdump/windows-x86_64/cuda_cuobjdump-windows-x86_64-12.8.90-archive.zip"
        Sha256 = "af6c4b7678cd9f3f3b8eeff7cc44f9d732bfacf70b1bdd03abccc25fec6c1ac1"
    },
    @{
        Path = "cuda_nvcc/windows-x86_64/cuda_nvcc-windows-x86_64-12.8.93-archive.zip"
        Sha256 = "9fdc70b4271ed9aad4d64cd7076a7d96ec36512d074b9995fe638de669197391"
    },
    @{
        Path = "cuda_nvrtc/windows-x86_64/cuda_nvrtc-windows-x86_64-12.8.93-archive.zip"
        Sha256 = "a63302a077f0248a743a1a7caa7dbd80d0fac56c6cfa9c41fa05fac9b7e5eda5"
    },
    @{
        Path = "cuda_profiler_api/windows-x86_64/cuda_profiler_api-windows-x86_64-12.8.90-archive.zip"
        Sha256 = "0a9d628d2582bd58c78ba92827cd9613d62790924cde1178719f677fc03b6ee2"
    },
    @{
        Path = "libnvjitlink/windows-x86_64/libnvjitlink-windows-x86_64-12.8.93-archive.zip"
        Sha256 = "5680b0a42ddf20f11705ca9c365d002f032ab876d3fe44382eeba633b558ccc0"
    }
)

function Test-CudaToolkit([string]$Root) {
    $Required = @(
        (Join-Path $Root "bin/nvcc.exe"),
        (Join-Path $Root "bin/cuobjdump.exe"),
        (Join-Path $Root "include/nvrtc.h"),
        (Join-Path $Root "include/cuda_profiler_api.h"),
        (Join-Path $Root "lib/x64/cudart.lib"),
        (Join-Path $Root "lib/x64/nvrtc.lib"),
        (Join-Path $Root "lib/x64/nvJitLink.lib")
    )
    $Missing = @($Required | Where-Object {
        -not (Test-Path $_ -PathType Leaf)
    })
    return $Missing.Count -eq 0
}

function Get-CudaTreeHash([string]$Root) {
    $Prefix = $Root.TrimEnd('\') + '\'
    $Lines = @(Get-ChildItem $Root -Recurse -File |
        Where-Object { $_.FullName -ne $CompleteMarker } |
        Sort-Object FullName |
        ForEach-Object {
            $Relative = $_.FullName.Substring($Prefix.Length).Replace('\', '/')
            "$Relative $((Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant())"
        })
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes(($Lines -join "`n"))
        return ([BitConverter]::ToString($Sha256.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $Sha256.Dispose()
    }
}

$CachedToolkitValid = (Test-Path $CompleteMarker -PathType Leaf) -and
    (Test-CudaToolkit $CudaRoot)
if ($CachedToolkitValid) {
    $ExpectedMarker = "$env:CUDA_VERSION`n$(Get-CudaTreeHash $CudaRoot)"
    $CachedToolkitValid = (Get-Content $CompleteMarker -Raw).Trim() -eq $ExpectedMarker
}

if ($CachedToolkitValid) {
    Write-Host "Reusing portable CUDA toolkit $CudaRoot"
} else {
    if (Test-Path $CudaRoot) {
        Remove-Item $CudaRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
    $StagingRoot = "$CudaRoot.tmp.$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force -Path $StagingRoot | Out-Null
    try {
        foreach ($Component in $Components) {
            $ArchiveName = Split-Path $Component.Path -Leaf
            $Archive = Join-Path $DownloadRoot $ArchiveName
            $ValidArchive = (Test-Path $Archive -PathType Leaf) -and
                ((Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant() -eq $Component.Sha256)
            if (-not $ValidArchive) {
                Remove-Item $Archive -Force -ErrorAction SilentlyContinue
                $Url = "$BaseUrl/$($Component.Path)"
                Write-Host "Downloading $Url"
                & curl.exe --fail --location --retry 3 --output $Archive $Url
                if ($LASTEXITCODE -ne 0) { throw "Failed to download $Url" }
            }
            $ActualHash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
            if ($ActualHash -ne $Component.Sha256) {
                throw "Checksum mismatch for $ArchiveName"
            }

            $ExtractRoot = Join-Path $StagingRoot ".extract-$([guid]::NewGuid().ToString('N'))"
            Expand-Archive -LiteralPath $Archive -DestinationPath $ExtractRoot
            $ArchiveRoots = @(Get-ChildItem $ExtractRoot -Directory)
            if ($ArchiveRoots.Count -ne 1) {
                throw "Expected one root directory in $ArchiveName"
            }
            Copy-Item (Join-Path $ArchiveRoots[0].FullName "*") `
                -Destination $StagingRoot -Recurse -Force
            Remove-Item $ExtractRoot -Recurse -Force
        }

        Get-ChildItem $StagingRoot -Recurse -File | Unblock-File
        if (-not (Test-CudaToolkit $StagingRoot)) {
            throw "The assembled portable CUDA toolkit is incomplete"
        }
        $env:CUDA_PATH = $StagingRoot
        & (Join-Path $StagingRoot "bin/nvcc.exe") --version
        if ($LASTEXITCODE -ne 0) { throw "Portable nvcc failed to execute" }
        $TreeHash = Get-CudaTreeHash $StagingRoot
        Set-Content -NoNewline -Path (Join-Path $StagingRoot ".complete") `
            -Value "$env:CUDA_VERSION`n$TreeHash"
        Move-Item $StagingRoot $CudaRoot
        Write-Host "Installed portable CUDA toolkit $CudaRoot"
    } finally {
        Remove-Item $StagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$env:CUDA_PATH = $CudaRoot
$env:CUDA_PATH_V12_8 = $CudaRoot
$env:Path = "$CudaRoot\bin;$env:Path"
& (Join-Path $CudaRoot "bin/nvcc.exe") --version
if ($LASTEXITCODE -ne 0) { throw "Portable nvcc validation failed" }
