param(
    [string]$Manifest = "",
    [string]$DistDirectory = "",
    [string[]]$Flavor = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (-not $Manifest) { $Manifest = Join-Path $PSScriptRoot "manifest.json" }
if (-not $DistDirectory) { $DistDirectory = Join-Path $RepoRoot "dist" }
$Release = Get-Content (Resolve-Path $Manifest) -Raw | ConvertFrom-Json
$Version = [string]$Release.release.version
if ($Release.release.tag -ne "v$Version") {
    throw "Release tag and version differ"
}

if (Test-Path C:\Tools\Enter-BuildEnv.ps1) {
    . C:\Tools\Enter-BuildEnv.ps1
}
$env:FOREVERTAS_CACHE_ROOT = [string]$Release.cache.windows
$env:VCPKG_COMMIT = [string]$Release.toolchains.windows.vcpkg_commit
$env:CUDA_VERSION = [string]$Release.cuda.version
. (Join-Path $PSScriptRoot "ensure-windows-cuda.ps1")
. (Join-Path $PSScriptRoot "ensure-windows-dependencies.ps1")
if (-not $env:VCPKG_INSTALLATION_ROOT -or -not $env:VCToolsRedistDir) {
    throw "Run in an MSVC environment with VCPKG_INSTALLATION_ROOT"
}
$RuntimeDirectory = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/x64-windows/bin"
$Flavors = @("universal") + @($Release.distribution.nvidia_sm | ForEach-Object { "nvidia-sm$_" }) + @("amd-rx7000-rx9000")
if ($Flavor.Count -gt 0) {
    foreach ($Name in $Flavor) {
        if ($Flavors -cnotcontains $Name) { throw "Unknown distribution flavor: $Name" }
    }
    $Flavors = $Flavor
} else {
    if (Test-Path (Join-Path $RepoRoot ".git")) {
        $Dirty = (git -C $RepoRoot status --porcelain=v1) -join ""
        if ($Dirty) {
            throw "Full Windows package builds require clean committed source"
        }
    } else {
        $Marker = Join-Path $RepoRoot ".release-source-commit"
        if (-not (Test-Path $Marker)) {
            throw "Release source commit marker is missing"
        }
    }
    $Validator = Join-Path $RepoRoot ".dependencies/ForeverValidator"
    if (Test-Path (Join-Path $Validator ".git")) {
        $ValidatorHead = (git -C $Validator rev-parse HEAD).Trim()
    } else {
        $ValidatorMarker = Join-Path $Validator ".release-source-commit"
        if (-not (Test-Path $ValidatorMarker)) {
            throw "ForeverValidator source marker is missing"
        }
        $ValidatorHead = (Get-Content $ValidatorMarker -Raw).Trim()
    }
    if ($ValidatorHead -ne $Release.sources.forevervalidator.commit) {
        throw "ForeverValidator does not match the release pin"
    }
}
if (Test-Path (Join-Path $RepoRoot ".git")) {
    $SourceCommit = (git -C $RepoRoot rev-parse HEAD).Trim()
} else {
    $SourceCommit = (Get-Content (Join-Path $RepoRoot ".release-source-commit") -Raw).Trim()
}
if ($SourceCommit -notmatch '^[0-9a-f]{40}$') {
    throw "ForeverTAS source commit is invalid"
}
New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null

$Components = @($Flavors | ForEach-Object {
    if ($_ -like "nvidia-sm*") { "nvidia-matrix" } else { $_ }
} | Select-Object -Unique)
foreach ($Component in $Components) {
    $BuildDirectory = Join-Path $RepoRoot "build/distribution-$Component"
    $TemporaryDist = Join-Path $RepoRoot "build/distribution-$Component-dist"
    New-Item -ItemType Directory -Force -Path $TemporaryDist | Out-Null
    $Options = @{
        BuildDirectory = $BuildDirectory
        DistDirectory = $TemporaryDist
        RuntimeDirectory = $RuntimeDirectory
        Flavor = $Component
    }
    if ($Component -eq "nvidia-matrix") {
        $Options.CudaArchitectures = (($Release.distribution.nvidia_sm |
            ForEach-Object { "$_-real;$_-virtual" }) -join ";")
        $Options.HipArchitectures = ($Release.distribution.nvidia_sm -join ";")
        $Options.HipPlatform = "nvidia"
        $Options.SkipInstaller = $true
    } elseif ($Component -eq "amd-rx7000-rx9000") {
        $Options.HipArchitectures = ($Release.distribution.amd_gfx -join ";")
        $Options.HipPlatform = "amd"
    }
    Write-Host "Building Windows component $Component"
    & (Join-Path $RepoRoot "packaging/windows/build-portable.ps1") @Options
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed for $Component" }

    if ($Component -eq "nvidia-matrix") {
        $Executable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
        $Cubins = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $Executable 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "CUDA inspection failed for $Component" }
        $Architectures = @([regex]::Matches($Cubins, 'sm_([0-9]+)\.cubin') |
            ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        $Expected = @($Release.distribution.nvidia_sm | ForEach-Object { [string]$_ })
        if (@(Compare-Object $Architectures $Expected).Count -ne 0) {
            throw "$Component has unexpected CUDA cubins: $($Architectures -join ', ')"
        }
        $Ptx = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-ptx $Executable 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "CUDA PTX inspection failed for $Component" }
        $PtxArchitectures = @([regex]::Matches($Ptx, 'sm_([0-9]+)\.ptx') |
            ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        if (@(Compare-Object $PtxArchitectures $Expected).Count -ne 0) {
            throw "$Component has unexpected CUDA PTX: $($PtxArchitectures -join ', ')"
        }
        foreach ($ObjectPath in @(
            "_deps/forevervalidator-build/CMakeFiles/forevervalidator_core.dir/src/simulation/backends/cuda/cuda_search_executor.cu.obj",
            "_deps/forevervalidator-build/CMakeFiles/forevervalidator_core.dir/src/simulation/backends/hip/generated/hip_search_executor.cu.obj")) {
            $Object = Join-Path $BuildDirectory $ObjectPath
            $ObjectCubins = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $Object 2>&1) -join "`n"
            if ($LASTEXITCODE -ne 0) { throw "CUDA inspection failed for $ObjectPath" }
            $ObjectArchitectures = @([regex]::Matches($ObjectCubins, 'sm_([0-9]+)\.cubin') |
                ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
            if (@(Compare-Object $ObjectArchitectures $Expected).Count -ne 0) {
                throw "$ObjectPath has unexpected CUDA cubins: $($ObjectArchitectures -join ', ')"
            }
        }
        $Archive = @(Get-ChildItem $TemporaryDist -Filter "ForeverTAS-*-windows-*.zip")
        if ($Archive.Count -ne 1) { throw "Expected one NVIDIA component archive" }
    }

    $PackageFlavors = if ($Component -eq "nvidia-matrix") {
        @($Flavors | Where-Object { $_ -like "nvidia-sm*" })
    } else { @($Component) }
    foreach ($Name in $PackageFlavors) {
        if ($Component -eq "nvidia-matrix") {
            & (Join-Path $RepoRoot "packaging/windows/build-installer.ps1") `
                -Archive $Archive[0].FullName -AssetId "windows-$Name-x86_64" `
                -PackageId $Name -DistDirectory $TemporaryDist
            if ($LASTEXITCODE -ne 0) { throw "Installer build failed for $Name" }
        }
        $InstallerName = "ForeverTAS-$Version-windows-$Name-x86_64-Setup.exe"
        foreach ($Filename in @($InstallerName, "$InstallerName.sha256")) {
            $Source = Join-Path $TemporaryDist $Filename
            if (-not (Test-Path $Source -PathType Leaf)) {
                throw "Missing $Filename"
            }
            Copy-Item -LiteralPath $Source -Destination (Join-Path $DistDirectory $Filename) -Force
        }
    }
}

$ComponentEvidence = [ordered]@{}
$PackageEvidence = [ordered]@{}
foreach ($Component in $Components) {
    $Archive = @(Get-ChildItem (Join-Path $RepoRoot "build/distribution-$Component-dist") `
        -Filter "ForeverTAS-*-windows-*.zip")
    if ($Archive.Count -ne 1) { throw "Missing component archive for $Component" }
    $ComponentEvidence[$Component] = [ordered]@{
        sha256 = (Get-FileHash -Algorithm SHA256 $Archive[0].FullName).Hash.ToLowerInvariant()
        toolchain = ($Release.toolchains.windows | ConvertTo-Json -Compress)
    }
}
foreach ($Name in $Flavors) {
    $Installer = Join-Path $DistDirectory `
        "ForeverTAS-$Version-windows-$Name-x86_64-Setup.exe"
    $PackageEvidence[$Name] = (Get-FileHash -Algorithm SHA256 $Installer).Hash.ToLowerInvariant()
}
$SourceRecord = [ordered]@{
    schema = 1
    forevertas = $SourceCommit
    forevervalidator = $Release.sources.forevervalidator.commit
    components = $ComponentEvidence
    packages = $PackageEvidence
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $DistDirectory "windows-source.json"),
    "$SourceRecord`n", (New-Object Text.UTF8Encoding($false)))
if (Test-Path (Join-Path $RepoRoot ".git")) {
    $Dirty = (git -C $RepoRoot status --porcelain=v1) -join ""
    if ((git -C $RepoRoot rev-parse HEAD).Trim() -ne $SourceCommit -or $Dirty) {
        throw "ForeverTAS source changed during Windows packaging"
    }
}
Write-Host "Assembled $($Flavors.Count) Windows installer flavors from $($Components.Count) builds in $DistDirectory"
