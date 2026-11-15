# The Windows half of a release, on the builder VM; release.py runs it over
# ssh. Root holds sources.tar (from release.py), the synced sources, the
# build folders and caches (kept between releases) and out\windows.
#
#   build-windows.ps1 -Root C:/forevertas-release -Parts "app cuda hip" ...
#   build-windows.ps1 -Root C:/forevertas-release -Check [-ExpectRenderModule]
#
# Tools (found by default, or set): Visual Studio 2022 Build Tools, LLVM
# (clang-cl), Qt 6.11.1 msvc2022_64 (FOREVERTAS_QT), vcpkg
# (FOREVERTAS_VCPKG), the HIP SDK (HIP_PATH), Inno Setup 6, Python 3,
# sccache (FOREVERTAS_SCCACHE). CUDA 12.8 is fetched by
# ensure-windows-cuda.ps1 (pinned archives) into Root\cache\cuda.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Parts = "app cuda hip",
    [string]$CudaSm = "",
    [int]$CudaPtx = 0,
    [string]$HipGfx = "",
    [long]$SourceDateEpoch = 0,
    [string]$VcpkgCommit = "",
    [switch]$Check,
    # -Check: the app ships the In-game render module (release.py: a release does)
    [switch]$ExpectRenderModule
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$Root = (New-Item -ItemType Directory -Force $Root).FullName
$Out = Join-Path $Root "out\windows"
$Build = Join-Path $Root "build"
$Cache = Join-Path $Root "cache"
$Src = Join-Path $Root "src"
$Validator = Join-Path $Root "validator"

function Step([string]$Name) { Write-Host "`n=== $Name ($(Get-Date -Format HH:mm:ss))" }
function Invoke-Native([string]$What, [scriptblock]$Command) {
    # native tools write warnings to stderr: judge them by exit code only
    $ErrorActionPreference = "Continue"
    & $Command
    $Code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($Code -ne 0) { throw "$What failed ($Code)" }
}

# A program started with only Windows on PATH (what a user's computer has
# for sure): its exit code and output. A missing DLL ends it with an error
# code instead of a dialog nobody can click (the error mode is inherited),
# and one that hangs is killed after TimeoutSeconds.
Add-Type -Namespace ForeverTas -Name Native -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetErrorMode(uint mode);
"@
[void][ForeverTas.Native]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000)
function Invoke-Clean([string]$Exe, [string[]]$Arguments, [int]$TimeoutSeconds = 120) {
    $Info = New-Object System.Diagnostics.ProcessStartInfo
    $Info.FileName = $Exe
    $Info.Arguments = ($Arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '
    $Info.UseShellExecute = $false
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Info.EnvironmentVariables["PATH"] = "$env:SystemRoot\System32;$env:SystemRoot"
    $Info.EnvironmentVariables["QT_QPA_PLATFORM"] = "windows"
    $Info.EnvironmentVariables["QSG_RHI_BACKEND"] = "software"
    $Process = [System.Diagnostics.Process]::Start($Info)
    $Output = $Process.StandardOutput.ReadToEndAsync()
    $Errors = $Process.StandardError.ReadToEndAsync()
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Process.Kill()
        return @{ Code = -1; Text = "did not finish in $TimeoutSeconds s" }
    }
    return @{ Code = $Process.ExitCode; Text = "$($Output.Result)$($Errors.Result)".Trim() }
}

function Find-First([string[]]$Candidates, [string]$What) {
    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path $Candidate)) { return (Resolve-Path $Candidate).Path }
    }
    throw "$What not found (looked in: $($Candidates -join ', '))"
}

$QtRoot = Find-First @($env:FOREVERTAS_QT, "C:\Qt\6.11.1\msvc2022_64") "Qt 6.11.1 msvc2022_64"
# vcpkg: the builder's, or one of our own at the pinned commit
$VcpkgRoot = @($env:FOREVERTAS_VCPKG, "C:\release-cache\forevertas\vcpkg", (Join-Path $Cache "vcpkg")) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ "vcpkg.exe")) } | Select-Object -First 1
if (-not $VcpkgRoot -and -not $Check) {
    if (-not $VcpkgCommit) { throw "no vcpkg on this builder, and no -VcpkgCommit to set one up" }
    $VcpkgRoot = Join-Path $Cache "vcpkg"
    if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
        Invoke-Native "cloning vcpkg" { git clone -q https://github.com/microsoft/vcpkg.git $VcpkgRoot }
    }
    Invoke-Native "checking out vcpkg" { git -C $VcpkgRoot fetch -q --depth 1 origin $VcpkgCommit
                                         git -C $VcpkgRoot checkout -q --detach --force $VcpkgCommit }
    Invoke-Native "bootstrapping vcpkg" { & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics }
}
$Sccache = Find-First @($env:FOREVERTAS_SCCACHE, "C:\lab\sccache\sccache.exe",
                        (Get-Command sccache.exe -ErrorAction SilentlyContinue | ForEach-Object Source)) "sccache"
$Llvm = Find-First @("C:\Program Files\LLVM\bin") "LLVM (clang-cl)"
$App = Join-Path $Build "app-install"

# --- the app as the installer lays it out: what -Check runs ------------------
if ($Check) {
    $Exe = Join-Path $App "ForeverTAS.exe"
    if (-not (Test-Path $Exe)) { throw "no built app in $App" }
    $Failures = 0
    $Archives = @(Get-ChildItem (Join-Path $Out "compute") -Recurse -Filter "*.tar.zst")
    foreach ($Archive in $Archives) {
        # only Windows itself on PATH: a module missing a DLL it needs fails
        # here, not on a user's computer
        $Result = Invoke-Clean $Exe @("--check-compute-module", $Archive.FullName)
        if ($Result.Code -ne 0) {
            $Failures++
            Write-Host "$($Archive.Name): FAILED ($($Result.Code)) $($Result.Text)"
        }
    }
    Write-Host "Windows modules loaded by the app: $($Archives.Count - $Failures) of $($Archives.Count)"
    if ($ExpectRenderModule) {
        if (-not (Get-ChildItem (Join-Path $App "licenses\render-module") -Filter "*.txt" -ErrorAction SilentlyContinue)) {
            $Failures++
            Write-Host "render module: shipped without its libraries' licenses"
        }
        $Result = Invoke-Clean $Exe @("--check-render-module")
        if ($Result.Code -ne 0) {
            $Failures++
            Write-Host "render module: FAILED ($($Result.Code)) $($Result.Text)"
        } else {
            Write-Host "Windows $($Result.Text) (loaded by the app with only Windows on PATH)"
        }
    }
    if ($Failures -gt 0) { exit 1 }
    exit 0
}

# --- sources, by content -------------------------------------------------------
function Sync-Tree([string]$From, [string]$To) {
    # files that did not change keep their time: the build folders rebuild only
    # what did
    New-Item -ItemType Directory -Force $To | Out-Null
    $FromRoot = (Resolve-Path $From).Path.TrimEnd('\') + '\'
    $ToRoot = (Resolve-Path $To).Path.TrimEnd('\') + '\'
    $Wanted = @{}
    foreach ($File in Get-ChildItem $From -Recurse -File) {
        $Relative = $File.FullName.Substring($FromRoot.Length)
        $Wanted[$Relative.ToLowerInvariant()] = $true
        $Target = Join-Path $To $Relative
        $Same = (Test-Path $Target -PathType Leaf) -and
                ((Get-Item $Target).Length -eq $File.Length) -and
                ((Get-FileHash $Target -Algorithm SHA256).Hash -eq (Get-FileHash $File.FullName -Algorithm SHA256).Hash)
        if (-not $Same) {
            New-Item -ItemType Directory -Force (Split-Path $Target) | Out-Null
            Copy-Item -LiteralPath $File.FullName -Destination $Target -Force
        }
    }
    foreach ($File in Get-ChildItem $To -Recurse -File) {
        if (-not $Wanted.ContainsKey($File.FullName.Substring($ToRoot.Length).ToLowerInvariant())) {
            Remove-Item -LiteralPath $File.FullName -Force
        }
    }
}

Step "sources"
$Incoming = Join-Path $Root "incoming"
Remove-Item $Incoming -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Incoming | Out-Null
Invoke-Native "extracting sources" { tar.exe -xf (Join-Path $Root "sources.tar") -C $Incoming }
Sync-Tree (Join-Path $Incoming "src") $Src
Sync-Tree (Join-Path $Incoming "validator") $Validator
Remove-Item $Incoming -Recurse -Force

# --- toolchain -------------------------------------------------------------------
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" `
    -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH = "$Llvm;$QtRoot\bin;$(Split-Path $Sccache);$env:PATH"
$env:SCCACHE_DIR = Join-Path $Cache "sccache"
$env:SCCACHE_CACHE_SIZE = "40G"
# A fresh server with these settings (a running one keeps its own), never
# stopping while idle: it would stop in the middle of a long CUDA file
$env:SCCACHE_IDLE_TIMEOUT = "0"
cmd /c "`"$Sccache`" --stop-server >nul 2>&1"
Invoke-Native "starting sccache" { & $Sccache --start-server }
if ($SourceDateEpoch -gt 0) { $env:SOURCE_DATE_EPOCH = "$SourceDateEpoch" }
$Jobs = [Environment]::ProcessorCount
New-Item -ItemType Directory -Force $Out, (Join-Path $Out "compute") | Out-Null
$Common = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=$Validator",
            "-DFOREVERTAS_ENABLE_TWIN=ON",
            "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
            "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
            "-DCMAKE_C_COMPILER_LAUNCHER=$Sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$Sccache")
$VcpkgBin = Join-Path $VcpkgRoot "installed\x64-windows\bin"
$env:VCPKG_BINARY_SOURCES = "clear;files,$(New-Item -ItemType Directory -Force (Join-Path $Cache 'vcpkg-binary')),readwrite"
Invoke-Native "vcpkg" { & (Join-Path $VcpkgRoot "vcpkg.exe") install --triplet x64-windows openssl zlib zstd }
$Python = (Get-Command python -ErrorAction Stop).Source
$Version = ([regex]::Match((Get-Content (Join-Path $Src "CMakeLists.txt") -Raw),
                           "project\(ForeverTAS\s+VERSION\s+([0-9.]+)")).Groups[1].Value

if ($Parts -match "\bapp\b") {
    Step "app"
    $AppBuild = Join-Path $Build "app"
    # the In-game render module's files, beside the app, and their licenses (release.py)
    $Extra = (@(Get-ChildItem (Join-Path $Root "extra") -File -ErrorAction SilentlyContinue) |
              ForEach-Object { $_.FullName -replace '\\', '/' }) -join ';'
    $ExtraLicenses = (@(Get-ChildItem (Join-Path $Root "extra/licenses") -File -ErrorAction SilentlyContinue) |
                      ForEach-Object { $_.FullName -replace '\\', '/' }) -join ';'
    Invoke-Native "configuring the app" {
        cmake -S $Src -B $AppBuild @Common -DBUILD_TESTING=OFF "-DCMAKE_PREFIX_PATH=$QtRoot" `
            "-DFOREVERTAS_WINDOWS_RUNTIME_DIR=$VcpkgBin" `
            "-DFOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR=$env:VCToolsRedistDir/x64/Microsoft.VC143.CRT" `
            "-DFOREVERTAS_EXTRA_INSTALL_FILES=$Extra" "-DFOREVERTAS_EXTRA_LICENSE_FILES=$ExtraLicenses" | Out-Null
    }
    Invoke-Native "building the app" { cmake --build $AppBuild --parallel $Jobs }
    Remove-Item $App -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-Native "installing the app" { cmake --install $AppBuild --prefix $App }
    Copy-Item (Join-Path $AppBuild "generated\forevertas_compute_abi.h") (Join-Path $Out "compute-abi.h") -Force
    # starts without a GPU and without anything on PATH but Windows
    $Smoke = Invoke-Clean (Join-Path $App "ForeverTAS.exe") @("--qml-smoke-test")
    if ($Smoke.Code -ne 0) { throw "the app does not start ($($Smoke.Code)): $($Smoke.Text)" }
    $Iscc = Find-First @($env:ISCC_PATH, "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                         "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") "Inno Setup 6"
    $Name = "ForeverTAS-$Version-windows-universal-x86_64-Setup"
    Remove-Item (Join-Path $Out "$Name.exe") -ErrorAction SilentlyContinue
    Invoke-Native "the installer" {
        & $Iscc /Q "/DAppVersion=$Version" "/DCompressionMode=lzma2/max" "/DSourceDir=$App" `
            "/DOutputDir=$Out" "/DOutputBaseFilename=$Name" (Join-Path $Src "packaging\windows\ForeverTAS.iss")
    }
}

if ($Parts -match "\bcuda\b") {
    Step "CUDA module (sm $CudaSm, PTX $CudaPtx)"
    $env:FOREVERTAS_CACHE_ROOT = $Cache
    $env:CUDA_VERSION = "12.8.1"
    & (Join-Path $Src "packaging\release\ensure-windows-cuda.ps1")
    $Architectures = (($CudaSm -split ",") | ForEach-Object { "$_-real" }) -join ";"
    $Architectures += ";$CudaPtx-virtual"
    $CudaBuild = Join-Path $Build "cuda"
    Invoke-Native "configuring the CUDA module" {
        cmake -S $Src -B $CudaBuild @Common -DFOREVERTAS_COMPUTE_MODULE=cuda `
            "-DCMAKE_CUDA_COMPILER=$env:CUDA_PATH/bin/nvcc.exe" "-DCMAKE_CUDA_HOST_COMPILER=cl.exe" `
            "-DCMAKE_CUDA_ARCHITECTURES=$Architectures" "-DCMAKE_CUDA_COMPILER_LAUNCHER=$Sccache" `
            "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler" | Out-Null
    }
    Invoke-Native "building the CUDA module" { cmake --build $CudaBuild --parallel $Jobs }
    $Target = Join-Path $Out "compute\cuda"
    Remove-Item $Target -Recurse -Force -ErrorAction SilentlyContinue
    # the CUDA EULA (the redistributable archives' LICENSE), shipped with cudart
    $Eula = Join-Path $env:TEMP "CUDA-EULA.txt"
    Copy-Item (Join-Path $env:CUDA_PATH "LICENSE") $Eula -Force
    $Cudart = Get-ChildItem (Join-Path $env:CUDA_PATH "bin") -Filter "cudart64_*.dll" | Select-Object -First 1
    Invoke-Native "packing the CUDA modules" {
        & $Python (Join-Path $Src "packaging\release\compute_modules.py") pack --build $CudaBuild `
            --backend cuda --os windows --sm $CudaSm --ptx $CudaPtx --runtime $Cudart.FullName `
            --runtime $Eula `
            --nvprune (Join-Path $env:CUDA_PATH "bin\nvprune.exe") `
            --cuobjdump (Join-Path $env:CUDA_PATH "bin\cuobjdump.exe") `
            --archiver (Join-Path $Llvm "llvm-lib.exe") --out $Target --jobs $Jobs
    }
}

if ($Parts -match "\bhip\b") {
    Step "HIP module ($HipGfx)"
    $HipRoot = Find-First @($env:HIP_PATH, (Get-ChildItem "C:\Program Files\AMD\ROCm" -Directory |
                                           Sort-Object Name -Descending | Select-Object -First 1).FullName) "HIP SDK"
    $HipRoot = $HipRoot.TrimEnd('\') -replace '\\', '/'
    $HipBuild = Join-Path $Build "hip"
    # CMake 4 (pip install cmake): Visual Studio's CMake 3.31 compiles HIP
    # files as C++ (-TP instead of -x hip) with the HIP SDK's clang in cl mode
    $Cmake4 = (& $Python -c "import cmake, os; print(os.path.join(cmake.CMAKE_BIN_DIR, 'cmake.exe'))" 2>$null)
    if (-not $Cmake4 -or -not (Test-Path $Cmake4) -or
        -not ((& $Cmake4 --version | Select-Object -First 1) -match "cmake version [4-9]\.")) {
        throw "the HIP module needs CMake 4 for Python (pip install cmake)"
    }
    Invoke-Native "configuring the HIP module" {
        & $Cmake4 -S $Src -B $HipBuild @Common -DFOREVERTAS_COMPUTE_MODULE=hip -DFOREVERTAS_ENABLE_TWIN=ON `
            -DCMAKE_HIP_PLATFORM=amd "-DCMAKE_HIP_ARCHITECTURES=$HipGfx" `
            "-DCMAKE_HIP_COMPILER=$HipRoot/bin/clang++.exe" "-DCMAKE_HIP_COMPILER_ROCM_ROOT=$HipRoot" `
            "-DCMAKE_HIP_FLAGS=--driver-mode=cl /std:c++17 /clang:-ffp-contract=off /clang:-fno-fast-math" `
            "-DCMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL=/MD" `
            -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_HIP_COMPILER_FORCED=ON `
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
            "-DCMAKE_HIP_ARCHIVE_CREATE=<CMAKE_AR> /OUT:<TARGET> <OBJECTS>" `
            "-DCMAKE_HIP_ARCHIVE_APPEND=<CMAKE_AR> /OUT:<TARGET> <OBJECTS>" "-DCMAKE_HIP_ARCHIVE_FINISH=" `
            "-DCMAKE_HIP_COMPILER_LAUNCHER=$Sccache" | Out-Null
    }
    Invoke-Native "building the HIP module" { & $Cmake4 --build $HipBuild --parallel $Jobs }
    Invoke-Native "checking the HIP device code" {
        & $Python (Join-Path $Src "packaging\release\verify_hip_targets.py") `
            (Join-Path $HipBuild "forevertas-compute-hip.dll") --expect ($HipGfx -replace ';', ',')
    }
    $Target = Join-Path $Out "compute\hip"
    Remove-Item $Target -Recurse -Force -ErrorAction SilentlyContinue
    $HipRuntime = Get-ChildItem "$HipRoot/bin" -Filter "amdhip64_*.dll" | Select-Object -First 1
    Invoke-Native "packing the HIP module" {
        & $Python (Join-Path $Src "packaging\release\compute_modules.py") pack --build $HipBuild `
            --backend hip --os windows --runtime $HipRuntime.FullName --out $Target
    }
}
cmd /c "`"$Sccache`" --stop-server >nul 2>&1"
Step "done"
