param(
    [string]$BuildDirectory = "",
    [string]$DistDirectory = "",
    [string]$RuntimeDirectory = "",
    [string]$Flavor = "",
    [string]$CudaArchitectures = "",
    [string]$HipArchitectures = "",
    [string]$HipPlatform = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $RepoRoot "build/package-windows"
}
if ([string]::IsNullOrWhiteSpace($DistDirectory)) {
    $DistDirectory = Join-Path $RepoRoot "dist"
}
if ([string]::IsNullOrWhiteSpace($RuntimeDirectory) -and
        -not [string]::IsNullOrWhiteSpace($env:VCPKG_INSTALLATION_ROOT)) {
    $RuntimeDirectory = Join-Path $env:VCPKG_INSTALLATION_ROOT `
        "installed/x64-windows/bin"
}
if ([string]::IsNullOrWhiteSpace($RuntimeDirectory)) {
    throw "RuntimeDirectory or VCPKG_INSTALLATION_ROOT is required"
}
if ([string]::IsNullOrWhiteSpace($env:VCToolsRedistDir)) {
    throw "Run this script from an MSVC developer environment"
}
if ([string]::IsNullOrWhiteSpace($env:VCPKG_INSTALLATION_ROOT)) {
    throw "VCPKG_INSTALLATION_ROOT is required"
}
if ([string]::IsNullOrWhiteSpace($env:QT_ROOT)) {
    throw "QT_ROOT is required"
}

New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
Get-ChildItem $DistDirectory -Filter "ForeverTAS-*-windows-*.zip*" |
    Remove-Item -Force

$FlavorOptions = @()
if ($Flavor) {
    $FlavorOptions += "-DFOREVERTAS_DISTRIBUTION_FLAVOR=$Flavor"
    $FlavorOptions += "-DFOREVERTAS_ENABLE_VULKAN=ON"
    $FlavorOptions += "-DFOREVERTAS_ENABLE_CUDA=$($(if ($Flavor -like 'nvidia-*') { 'ON' } else { 'OFF' }))"
    $FlavorOptions += "-DFOREVERTAS_ENABLE_HIP=$($(if ($Flavor -eq 'universal') { 'OFF' } else { 'ON' }))"
}
if ($CudaArchitectures) {
    $FlavorOptions += "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures"
}
if ($HipArchitectures) {
    $FlavorOptions += "-DCMAKE_HIP_ARCHITECTURES=$HipArchitectures"
}
if ($HipPlatform) {
    $FlavorOptions += "-DCMAKE_HIP_PLATFORM=$HipPlatform"
    if (-not $env:HIP_PATH) { throw "HIP_PATH is required for HIP packages" }
    $HipRoot = $env:HIP_PATH.TrimEnd('\') -replace '\\', '/'
    $FlavorOptions += "-DCMAKE_HIP_COMPILER_ROCM_ROOT=$HipRoot"
    # CMake's HIP rules use Unix-style link/archive flags with Ninja on
    # Windows. All HIP code in ForeverTAS is archived into static libraries.
    $FlavorOptions += "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    $FlavorOptions += "-DCMAKE_HIP_COMPILER_FORCED=ON"
    $FlavorOptions += "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
    $FlavorOptions += '-DCMAKE_HIP_ARCHIVE_CREATE=<CMAKE_AR> /OUT:<TARGET> <OBJECTS>'
    $FlavorOptions += '-DCMAKE_HIP_ARCHIVE_APPEND=<CMAKE_AR> /OUT:<TARGET> <OBJECTS>'
    $FlavorOptions += '-DCMAKE_HIP_ARCHIVE_FINISH='
}
$Toolchain = Join-Path $env:VCPKG_INSTALLATION_ROOT "scripts/buildsystems/vcpkg.cmake"
$FlavorOptions += "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
$FlavorOptions += "-DCMAKE_PREFIX_PATH=$env:QT_ROOT"
$FlavorOptions += "-DCMAKE_CXX_COMPILER=clang-cl"
$Validator = Join-Path $RepoRoot ".dependencies/ForeverValidator"
if (Test-Path (Join-Path $Validator "CMakeLists.txt")) {
    $FlavorOptions += "-DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=$Validator"
}
$ManifestTool = Get-Command mt.exe -ErrorAction SilentlyContinue
if ($ManifestTool) { $FlavorOptions += "-DCMAKE_MT=$($ManifestTool.Source)" }
$CompilerCache = Get-Command sccache.exe -ErrorAction SilentlyContinue
if ($CompilerCache) {
    $FlavorOptions += "-DCMAKE_CXX_COMPILER_LAUNCHER=$($CompilerCache.Source)"
}
if ($Flavor -like 'nvidia-*') {
    if (-not $env:CUDA_PATH) { throw "CUDA_PATH is required for NVIDIA packages" }
    $FlavorOptions += "-DCMAKE_CUDA_COMPILER=$env:CUDA_PATH/bin/nvcc.exe"
    $FlavorOptions += "-DCMAKE_CUDA_HOST_COMPILER=cl.exe"
    $FlavorOptions += "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler"
    $FlavorOptions += "-DFOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR=$env:CUDA_PATH/bin"
    $FlavorOptions += "-DCMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL=-Xcompiler=/MD"
}
if ($Flavor -eq 'amd-rx7000-rx9000') {
    if (-not $env:HIP_PATH) { throw "HIP_PATH is required for AMD packages" }
    $HipRoot = $env:HIP_PATH.TrimEnd('\') -replace '\\', '/'
    $FlavorOptions += "-DCMAKE_HIP_COMPILER=$HipRoot/bin/clang++.exe"
    $FlavorOptions += "-DCMAKE_HIP_FLAGS=--driver-mode=cl /std:c++17 /clang:-ffp-contract=off /clang:-fno-fast-math"
    $FlavorOptions += "-DCMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL=/MD"
    $FlavorOptions += "-DFOREVERTAS_WINDOWS_HIP_RUNTIME_DIR=$env:HIP_PATH/bin"
}

$ErrorActionPreference = "Continue"
cmake -S $RepoRoot -B $BuildDirectory -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    "-DFOREVERTAS_WINDOWS_RUNTIME_DIR=$RuntimeDirectory" `
    "-DFOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR=$env:VCToolsRedistDir/x64/Microsoft.VC143.CRT" `
    -DBUILD_TESTING=OFF `
    @FlavorOptions
$ConfigureExitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($ConfigureExitCode -ne 0) { throw "CMake configure failed" }

$ErrorActionPreference = "Continue"
cmake --build $BuildDirectory --parallel
$BuildExitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($BuildExitCode -ne 0) { throw "Build failed" }

$ErrorActionPreference = "Continue"
cpack --config (Join-Path $BuildDirectory "CPackConfig.cmake") `
    -G ZIP -B $DistDirectory
$PackageExitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($PackageExitCode -ne 0) { throw "CPack failed" }

$Artifacts = @(Get-ChildItem $DistDirectory -Filter "ForeverTAS-*-windows-*.zip")
if ($Artifacts.Count -ne 1) {
    throw "Expected one Windows ZIP, found $($Artifacts.Count)"
}

$Artifact = $Artifacts[0]
$Hash = Get-FileHash -Algorithm SHA256 $Artifact.FullName
"$($Hash.Hash.ToLower())  $($Artifact.Name)" |
    Set-Content -NoNewline "$($Artifact.FullName).sha256"
& (Join-Path $PSScriptRoot "test-portable.ps1") -Archive $Artifact.FullName
$Cache = Get-Content (Join-Path $BuildDirectory "CMakeCache.txt") -Raw
if ($Cache -notmatch '(?m)^FOREVERTAS_UPDATE_ASSET_ID:INTERNAL=(windows(?:-[a-z0-9-]+)?-(?:x86_64|arm64))\r?$') {
    throw "Build has no Windows update asset identity"
}
& (Join-Path $PSScriptRoot "build-installer.ps1") `
    -Archive $Artifact.FullName -AssetId $Matches[1] -DistDirectory $DistDirectory
Write-Host "Created $($Artifact.FullName)"
