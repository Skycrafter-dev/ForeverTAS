$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$ValidatorRoot = Join-Path $RepoRoot ".dependencies/ForeverValidator"
$BuildDirectory = Join-Path $RepoRoot "build/release"
$DistDirectory = Join-Path $RepoRoot "dist"
$SplitCompileJobs = $env:FOREVERVALIDATOR_CUDA_SPLIT_COMPILE_JOBS
$CacheSchema = "cuda-search-object-v2"
$ExpectedCudaArchitectures = "61-real;62-real;70-real;72-real;75-real;80-real;86-real;87-real;89-real;90-real;100-real;101-real;120-real;120-virtual"
$ExpectedArchitectureKey = "sm61-sm62-sm70-sm72-sm75-sm80-sm86-sm87-sm89-sm90-sm100-sm101-sm120-ptx120"
$ExpectedCubinArchitectures = @(61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120)
$ExpectedPtxArchitectures = @(120)

foreach ($Name in @(
    "CUDA_PATH",
    "CUDA_VERSION",
    "CUDA_ARCHITECTURES",
    "CUDA_ARCHITECTURE_KEY",
    "FOREVERVALIDATOR_COMMIT",
    "FOREVERVALIDATOR_CUDA_SEARCH_SOURCE_COMMIT",
    "FOREVERTAS_VERSION",
    "FOREVERTAS_WINDOWS_SEARCH_CACHE",
    "SCCACHE_PATH",
    "VCPKG_INSTALLATION_ROOT",
    "VCToolsRedistDir"
)) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($Name))) {
        throw "$Name is required"
    }
}
if ($env:CUDA_VERSION -ne "12.8.1" -or
        $env:CUDA_ARCHITECTURES -cne $ExpectedCudaArchitectures -or
        $env:CUDA_ARCHITECTURE_KEY -cne $ExpectedArchitectureKey -or
        $SplitCompileJobs -ne "4") {
    throw "Windows release inputs do not match the validated CUDA contract"
}
if ($env:FOREVERVALIDATOR_COMMIT -cnotmatch '^[0-9a-f]{40}$' -or
        $env:FOREVERVALIDATOR_COMMIT -cne
            $env:FOREVERVALIDATOR_CUDA_SEARCH_SOURCE_COMMIT) {
    throw "ForeverValidator release and CUDA object commits must be one exact lowercase SHA"
}

$ValidatorMarker = Join-Path $ValidatorRoot ".release-source-commit"
if (Test-Path (Join-Path $ValidatorRoot ".git")) {
    $ActualValidatorCommit = (git -C $ValidatorRoot rev-parse HEAD).Trim()
} elseif (Test-Path $ValidatorMarker) {
    $ActualValidatorCommit = (Get-Content $ValidatorMarker -Raw).Trim()
} else {
    throw "ForeverValidator source commit marker is missing"
}
if ($ActualValidatorCommit -ne $env:FOREVERVALIDATOR_COMMIT) {
    throw "ForeverValidator checkout does not match the pinned commit"
}
$TasMarker = Join-Path $RepoRoot ".release-source-commit"
if (-not (Test-Path $TasMarker)) { throw "ForeverTAS source commit marker is missing" }
$ActualTasCommit = (Get-Content $TasMarker -Raw).Trim()
if ($ActualTasCommit -notmatch '^[0-9a-f]{40}$') { throw "ForeverTAS source commit marker is invalid" }

function Get-TextHash([string]$Text) {
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($Sha256.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    } finally {
        $Sha256.Dispose()
    }
}

function Test-CudaArchitectures([string]$Object) {
    if (-not (Test-Path $Object -PathType Leaf)) { return $false }

    $ElfOutput = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $Object 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { return $false }
    $ActualCubins = @(
        [regex]::Matches($ElfOutput, 'sm_([0-9]+)\.cubin') |
            ForEach-Object { [int]$_.Groups[1].Value } |
            Sort-Object -Unique
    )
    if ($ActualCubins.Count -ne $ExpectedCubinArchitectures.Count -or
            @(Compare-Object $ActualCubins $ExpectedCubinArchitectures).Count -ne 0) {
        return $false
    }

    $PtxOutput = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-ptx $Object 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { return $false }
    $ActualPtx = @(
        [regex]::Matches($PtxOutput, 'sm_([0-9]+)\.ptx') |
            ForEach-Object { [int]$_.Groups[1].Value } |
            Sort-Object -Unique
    )
    return $ActualPtx.Count -eq $ExpectedPtxArchitectures.Count -and
        @(Compare-Object $ActualPtx $ExpectedPtxArchitectures).Count -eq 0
}

function Test-CudaCache([string]$Directory) {
    $Object = Join-Path $Directory "cuda_search_executor.cu.obj"
    $MetadataPath = Join-Path $Directory "metadata.txt"
    if (-not (Test-CudaArchitectures $Object) -or
            -not (Test-Path $MetadataPath -PathType Leaf)) {
        return $false
    }
    $Metadata = @(Get-Content $MetadataPath)
    if ($Metadata.Count -ne $CacheIdentity.Count) { return $false }
    for ($Index = 0; $Index -lt $CacheIdentity.Count; ++$Index) {
        if ($Metadata[$Index] -cne $CacheIdentity[$Index]) { return $false }
    }
    $HashPath = Join-Path $Directory "object.sha256"
    $ActualHash = (Get-FileHash -Algorithm SHA256 $Object).Hash.ToLowerInvariant()
    if (-not (Test-Path $HashPath -PathType Leaf)) { return $false }
    return (Get-Content $HashPath -Raw).Trim().ToLowerInvariant() -eq $ActualHash
}

function Test-CudaSessionLtoArtifacts([string]$ValidatorBuildDirectory) {
    $LtoIr = Join-Path $ValidatorBuildDirectory `
        "generated/forevervalidator_cuda_search.ltoir"
    $LtoHeader = Join-Path $ValidatorBuildDirectory `
        "generated/forevervalidator_cuda_search_lto_ir.h"
    return (Test-Path $LtoIr -PathType Leaf) -and
        (Get-Item $LtoIr).Length -ne 0 -and
        (Test-Path $LtoHeader -PathType Leaf) -and
        (Get-Item $LtoHeader).Length -ne 0 -and
        (Get-Content $LtoHeader -Raw) -match
            'ForeverValidatorCudaSearchLtoIr'
}

$ToolchainIdentity = Get-TextHash (@(
    "cuda_host_compatibility=allow-unsupported-compiler"
    (& clang-cl --version | Out-String).Trim()
    $env:VCToolsVersion
    (& nvcc --version | Out-String).Trim()
) -join "`n")
$CacheIdentity = @(
    "cache_schema=$CacheSchema"
    "toolchain=$ToolchainIdentity"
    "cuda=$env:CUDA_VERSION"
    "architectures=$env:CUDA_ARCHITECTURES"
    "architecture_key=$env:CUDA_ARCHITECTURE_KEY"
    "split_compile_jobs=$SplitCompileJobs"
    "validator=$env:FOREVERVALIDATOR_CUDA_SEARCH_SOURCE_COMMIT"
)
$SearchKey = Get-TextHash ($CacheIdentity -join "`n")
$SearchCacheDirectory = Join-Path $env:FOREVERTAS_WINDOWS_SEARCH_CACHE $SearchKey
$CachedSearchObject = Join-Path $SearchCacheDirectory "cuda_search_executor.cu.obj"

$CacheHit = Test-CudaCache $SearchCacheDirectory
if ($CacheHit) {
    Write-Host "Reusing cached CUDA search object $SearchKey"
} elseif (Test-Path $SearchCacheDirectory) {
    Write-Warning "Discarding invalid CUDA search object cache $SearchKey"
    Remove-Item $SearchCacheDirectory -Recurse -Force
}

Remove-Item $BuildDirectory, $DistDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $BuildDirectory, $DistDirectory | Out-Null

& $env:SCCACHE_PATH --start-server
& $env:SCCACHE_PATH --zero-stats
try {
    $PrebuiltOption = "-DFOREVERVALIDATOR_CUDA_SEARCH_PREBUILT_OBJECT="
    if ($CacheHit) {
        $PrebuiltOption = "-DFOREVERVALIDATOR_CUDA_SEARCH_PREBUILT_OBJECT=$CachedSearchObject"
    }

    $Python = (Get-Command python -ErrorAction Stop).Source
    $Launcher = Join-Path $RepoRoot "packaging/release/cuda_compiler_launcher.py"
    $CudaCompiler = Join-Path $env:CUDA_PATH "bin/nvcc.exe"
    $ManifestTool = (Get-Command mt.exe -ErrorAction Stop).Source
    $VcpkgToolchain = Join-Path $env:VCPKG_INSTALLATION_ROOT "scripts/buildsystems/vcpkg.cmake"
    $RuntimeDirectory = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/x64-windows/bin"
    $MsvcRuntimeDirectory = Join-Path $env:VCToolsRedistDir "x64/Microsoft.VC143.CRT"

    cmake -S $RepoRoot -B $BuildDirectory -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_CUDA_ARCHITECTURES=$env:CUDA_ARCHITECTURES" `
        "-DCMAKE_CUDA_COMPILER=$CudaCompiler" `
        "-DCMAKE_CUDA_HOST_COMPILER=cl.exe" `
        "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler" `
        -DCMAKE_CXX_COMPILER=clang-cl `
        "-DCMAKE_MT=$ManifestTool" `
        "-DCMAKE_CUDA_COMPILER_LAUNCHER=$Python;$Launcher;$env:SCCACHE_PATH" `
        "-DCMAKE_CXX_COMPILER_LAUNCHER=$env:SCCACHE_PATH" `
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain" `
        -DVCPKG_TARGET_TRIPLET=x64-windows `
        "-DFOREVERTAS_WINDOWS_RUNTIME_DIR=$RuntimeDirectory" `
        "-DFOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR=$MsvcRuntimeDirectory" `
        "-DFOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR=$env:CUDA_PATH/bin" `
        -DBUILD_TESTING=OFF `
        -DFOREVERTAS_ENABLE_CUDA=ON `
        "-DFOREVERVALIDATOR_CUDA_SPLIT_COMPILE_JOBS=$SplitCompileJobs" `
        $PrebuiltOption `
        "-DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=$ValidatorRoot"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    $CMakeCache = Get-Content (Join-Path $BuildDirectory "CMakeCache.txt") -Raw
    $CompileCommands = Get-Content (Join-Path $BuildDirectory "compile_commands.json") -Raw
    if ($CMakeCache -notmatch "FOREVERTAS_ENABLE_CUDA:BOOL=ON" -or
            $CompileCommands -notmatch "FOREVERVALIDATOR_HAS_CUDA=1" -or
            $CMakeCache -notmatch "FOREVERVALIDATOR_CUDA_SPLIT_COMPILE_JOBS:STRING=$SplitCompileJobs") {
        throw "CUDA configuration or unchanged split-compile setting was not preserved"
    }
    if ($CacheHit) {
        if ($CompileCommands -match "cuda_search_executor\.cu") {
            throw "Cached CUDA search object was not used"
        }
    } elseif ($CompileCommands -notmatch "cuda_search_executor\.cu") {
        throw "CUDA search translation unit is missing after a validated cache miss"
    }

    cmake --build $BuildDirectory --config Release `
        --target forevertas-simulation-debug-worker --parallel
    if ($LASTEXITCODE -ne 0) { throw "Windows debugger worker build failed" }
    cmake --build $BuildDirectory --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }

    $BuiltSearchObject = Join-Path $BuildDirectory `
        "_deps/forevervalidator-build/CMakeFiles/forevervalidator_core.dir/src/simulation/backends/cuda/cuda_search_executor.cu.obj"
    if (-not $CacheHit) {
        if (-not (Test-CudaArchitectures $BuiltSearchObject)) {
            throw "Built CUDA search object does not contain every required architecture"
        }
        $TemporaryDirectory = "$SearchCacheDirectory.tmp.$([guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Force -Path $TemporaryDirectory | Out-Null
        Copy-Item $BuiltSearchObject (Join-Path $TemporaryDirectory "cuda_search_executor.cu.obj")
        $CacheIdentity |
            Set-Content -Path (Join-Path $TemporaryDirectory "metadata.txt")
        (Get-FileHash -Algorithm SHA256 `
            (Join-Path $TemporaryDirectory "cuda_search_executor.cu.obj")).Hash.ToLowerInvariant() |
            Set-Content -NoNewline -Path (Join-Path $TemporaryDirectory "object.sha256")
        Move-Item $TemporaryDirectory $SearchCacheDirectory
        Write-Host "Cached CUDA search object $SearchKey"
    }

    if (-not (Test-CudaCache $SearchCacheDirectory)) {
        throw "Cached CUDA search object failed final integrity validation"
    }
    $ValidatorBuildDirectory = Join-Path $BuildDirectory "_deps/forevervalidator-build"
    if (-not (Test-CudaSessionLtoArtifacts $ValidatorBuildDirectory)) {
        throw "CUDA session-specialization LTO artifacts are missing or empty"
    }
    & "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $CachedSearchObject |
        Tee-Object (Join-Path $BuildDirectory "cuda-elf-images.txt")
    & "$env:CUDA_PATH\bin\cuobjdump.exe" --list-ptx $CachedSearchObject |
        Tee-Object (Join-Path $BuildDirectory "cuda-ptx-images.txt")

    $CudaObjects = @(Get-ChildItem `
        (Join-Path $BuildDirectory "_deps/forevervalidator-build") `
        -Recurse -File | Where-Object { $_.Name -like "*.cu.obj" })
    if ($CudaObjects.Count -eq 0) {
        throw "No Windows CUDA objects were produced"
    }
    foreach ($CudaObject in $CudaObjects) {
        if (-not (Test-CudaArchitectures $CudaObject.FullName)) {
            throw "CUDA architecture validation failed: $($CudaObject.FullName)"
        }
    }
    $FinalExecutable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
    if (-not (Test-CudaArchitectures $FinalExecutable)) {
        throw "Final ForeverTAS.exe CUDA architecture validation failed"
    }

    cpack --config (Join-Path $BuildDirectory "CPackConfig.cmake") `
        -C Release -G ZIP -B $DistDirectory
    if ($LASTEXITCODE -ne 0) { throw "CPack failed" }

    $Artifacts = @(Get-ChildItem $DistDirectory -Filter "ForeverTAS-*-windows-*.zip")
    if ($Artifacts.Count -ne 1) {
        throw "Expected one Windows ZIP, found $($Artifacts.Count)"
    }
    $Artifact = $Artifacts[0]
    $Hash = Get-FileHash -Algorithm SHA256 $Artifact.FullName
    "$($Hash.Hash.ToLowerInvariant())  $($Artifact.Name)" |
        Set-Content -NoNewline -Path "$($Artifact.FullName).sha256"
    & (Join-Path $RepoRoot "packaging/windows/test-portable.ps1") `
        -Archive $Artifact.FullName

    [ordered]@{
        cuda = "12.8.1"
        cubin_architectures = @(
            61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120)
        ptx_architecture = 120
        split_compile_jobs = 4
        scope = "all CUDA objects and final ForeverTAS.exe"
        resolved_sources = [ordered]@{
            forevertas = $ActualTasCommit
            forevervalidator = $ActualValidatorCommit
            version = $env:FOREVERTAS_VERSION
        }
    } | ConvertTo-Json | Set-Content -Path `
        (Join-Path $DistDirectory "cuda-fatbinary-windows.json")
} finally {
    & $env:SCCACHE_PATH --show-stats
    & $env:SCCACHE_PATH --stop-server
}
