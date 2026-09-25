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
New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null

foreach ($Name in $Flavors) {
    $BuildDirectory = Join-Path $RepoRoot "build/distribution-$Name"
    $TemporaryDist = Join-Path $RepoRoot "build/distribution-$Name-dist"
    New-Item -ItemType Directory -Force -Path $TemporaryDist | Out-Null
    $Options = @{
        BuildDirectory = $BuildDirectory
        DistDirectory = $TemporaryDist
        RuntimeDirectory = $RuntimeDirectory
        Flavor = $Name
    }
    if ($Name -like "nvidia-sm*") {
        $Sm = $Name.Substring(9)
        $Options.CudaArchitectures = "$Sm-real;$Sm-virtual"
        $Options.HipArchitectures = $Sm
        $Options.HipPlatform = "nvidia"
    } elseif ($Name -eq "amd-rx7000-rx9000") {
        $Options.HipArchitectures = ($Release.distribution.amd_gfx -join ";")
        $Options.HipPlatform = "amd"
    }
    Write-Host "Building Windows $Name"
    & (Join-Path $RepoRoot "packaging/windows/build-portable.ps1") @Options
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed for $Name" }

    if ($Name -like "nvidia-sm*") {
        $Executable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
        $Cubins = (& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf $Executable 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "CUDA inspection failed for $Name" }
        $Architectures = @([regex]::Matches($Cubins, 'sm_([0-9]+)\.cubin') |
            ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        if ($Architectures.Count -ne 1 -or $Architectures[0] -ne $Sm) {
            throw "$Name has unexpected CUDA cubins: $($Architectures -join ', ')"
        }
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

Write-Host "Built $($Flavors.Count) Windows installer flavors in $DistDirectory"
