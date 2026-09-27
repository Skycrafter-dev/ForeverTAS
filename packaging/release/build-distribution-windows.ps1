param(
    [string]$Manifest = "",
    [string]$DistDirectory = "",
    [string[]]$Flavor = @(),
    [switch]$PlanOnly
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
$Flavors = @("universal") + @($Release.distribution.nvidia_sm | ForEach-Object { "nvidia-sm$_" }) + @("amd-rx7000-rx9000")
if ($Flavor.Count -gt 0) {
    foreach ($Name in $Flavor) {
        if ($Flavors -cnotcontains $Name) { throw "Unknown distribution flavor: $Name" }
    }
    $Flavors = $Flavor
}
$NvidiaTemplate = "nvidia-sm75"
$BuildOrder = @("universal")
if (@($Flavors | Where-Object { $_ -like "nvidia-sm*" }).Count -gt 0) {
    $BuildOrder += $NvidiaTemplate
}
$BuildOrder += @($Flavors | Where-Object {
    $_ -ne "universal" -and $_ -ne $NvidiaTemplate
})
if ($PlanOnly) {
    [ordered]@{ version = $Version; flavors = $Flavors; build_order = $BuildOrder } |
        ConvertTo-Json -Depth 3
    return
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
if ($Flavor.Count -eq 0) {
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

$SourceCommit = if (Test-Path (Join-Path $RepoRoot ".git")) {
    (git -C $RepoRoot rev-parse HEAD).Trim()
} else {
    (Get-Content (Join-Path $RepoRoot ".release-source-commit") -Raw).Trim()
}
$ValidatorSource = Join-Path $RepoRoot ".dependencies/ForeverValidator"
$ValidatorCommit = if (Test-Path (Join-Path $ValidatorSource ".git")) {
    (git -C $ValidatorSource rev-parse HEAD).Trim()
} else {
    (Get-Content (Join-Path $ValidatorSource ".release-source-commit") -Raw).Trim()
}
if ($SourceCommit -notmatch '^[0-9a-f]{40}$' -or
        $ValidatorCommit -ne $Release.sources.forevervalidator.commit) {
    throw "Source provenance does not match the release manifest"
}
if (Test-Path (Join-Path $RepoRoot ".git")) {
    $Dirty = (git -C $RepoRoot status --porcelain=v1) -join ""
    if ($Dirty) { throw "Commit ForeverTAS source before building installers" }
}
if (Test-Path (Join-Path $ValidatorSource ".git")) {
    $DirtyValidator = (git -C $ValidatorSource status --porcelain=v1) -join ""
    if ($DirtyValidator) { throw "Commit ForeverValidator source before building installers" }
}
$PackageEvidence = [ordered]@{}
$VariantJobs = @()
$VariantJobLimit = if ($env:FOREVERTAS_RELEASE_VARIANT_JOBS) {
    [int]$env:FOREVERTAS_RELEASE_VARIANT_JOBS
} else { 6 }
if ($VariantJobLimit -lt 1 -or $VariantJobLimit -gt 8) {
    throw "FOREVERTAS_RELEASE_VARIANT_JOBS must be between 1 and 8"
}
function Receive-VariantJob($Job) {
    $Job | Receive-Job -ErrorAction SilentlyContinue | Out-Null
    if ($Job.State -ne "Completed") {
        throw "Windows $($Job.Name) variant failed; see build/distribution-$($Job.Name)-dist/variant-build.log"
    }
    $EvidencePath = Join-Path $RepoRoot "build/distribution-$($Job.Name)-dist/variant-evidence.json"
    if (-not (Test-Path $EvidencePath -PathType Leaf)) {
        throw "Windows $($Job.Name) has no variant evidence"
    }
    $Entry = Get-Content $EvidencePath -Raw | ConvertFrom-Json
    $PackageEvidence[$Job.Name] = [ordered]@{
        sha256 = $Entry.sha256
        binary_sha256 = $Entry.binary_sha256
        toolchain = ($Release.toolchains.windows | ConvertTo-Json -Compress)
        runtime_template = $Entry.runtime_template
    }
    Remove-Job $Job
}
$WarmSeedPath = Join-Path $RepoRoot "build/windows-warm-seed.json"
$WarmSeed = if (Test-Path $WarmSeedPath) {
    Get-Content $WarmSeedPath -Raw | ConvertFrom-Json
} else { $null }
if ($env:FOREVERTAS_ALLOW_COLD_BUILD -ne "1") {
    foreach ($Name in $BuildOrder) {
        $WarmCache = Join-Path $RepoRoot "build/distribution-$Name/CMakeCache.txt"
        if (-not (Test-Path $WarmCache -PathType Leaf)) {
            throw "Refusing a cold Windows build for $Name; a warm build directory is required"
        }
    }
}
foreach ($Name in $BuildOrder) {
    $BuildDirectory = Join-Path $RepoRoot "build/distribution-$Name"
    $TemporaryDist = Join-Path $RepoRoot "build/distribution-$Name-dist"
    New-Item -ItemType Directory -Force -Path $TemporaryDist | Out-Null
    $Options = @{
        BuildDirectory = $BuildDirectory
        DistDirectory = $TemporaryDist
        RuntimeDirectory = $RuntimeDirectory
        Flavor = $Name
    }
    if ($Name -ne "universal") {
        $Options.ExternalQmlModule = $true
        $Options.CommonQmlDirectory = Join-Path $RepoRoot `
            "build/distribution-universal/bin/qml/ForeverTAS"
        if ($WarmSeed -and $WarmSeed.recipient.forevervalidator -eq $ValidatorCommit -and
                $WarmSeed.manifest_sha256 -eq
                (Get-FileHash $Manifest -Algorithm SHA256).Hash.ToLower() -and
                $WarmSeed.validator_archives -and
                $WarmSeed.validator_archives.PSObject.Properties.Name -contains $Name) {
            $ArchiveDirectory = Join-Path $BuildDirectory "_deps/forevervalidator-build"
            $Recorded = $WarmSeed.validator_archives.$Name
            foreach ($Component in @("core", "native")) {
                $Archive = Join-Path $ArchiveDirectory "forevervalidator_$Component.lib"
                if (-not (Test-Path $Archive -PathType Leaf) -or
                        (Get-FileHash $Archive -Algorithm SHA256).Hash.ToLower() -ne
                        $Recorded.$Component) {
                    throw "Warm Validator archive provenance failed for $Name/$Component"
                }
            }
            $Options.PrebuiltValidatorDirectory = $ArchiveDirectory
            Write-Host "Reusing verified Validator compute for $Name"
        }
    }
    if ($Name -like "nvidia-sm*") {
        $Sm = $Name.Substring(9)
        $Options.CudaArchitectures = "$Sm-real;$Sm-virtual"
        $Options.HipArchitectures = $Sm
        $Options.HipPlatform = "nvidia"
        if ($Name -ne $NvidiaTemplate) {
            $Options.BuildOnly = $true
        }
    } elseif ($Name -eq "amd-rx7000-rx9000") {
        $Options.HipArchitectures = ($Release.distribution.amd_gfx -join ";")
        $Options.HipPlatform = "amd"
    }
    if ($Name -eq "amd-rx7000-rx9000") {
        while ($VariantJobs.Count -ge $VariantJobLimit) {
            $Finished = Wait-Job -Job $VariantJobs -Any
            Receive-VariantJob $Finished
            $VariantJobs = @($VariantJobs | Where-Object { $_.Id -ne $Finished.Id })
        }
        $Log = Join-Path $TemporaryDist "variant-build.log"
        New-Item -ItemType Directory -Force -Path $TemporaryDist | Out-Null
        $Script = Join-Path $RepoRoot "packaging/release/build-windows-amd-variant.ps1"
        $Job = Start-Job -Name $Name -ScriptBlock {
            param($Script, $Root, $ReleaseVersion, $Output, $Prebuilt, $LogPath)
            & $Script -RepoRoot $Root -Version $ReleaseVersion `
                -DistDirectory $Output -PrebuiltValidatorDirectory $Prebuilt *> $LogPath
            if (-not $?) { throw "Variant build failed: amd-rx7000-rx9000" }
        } -ArgumentList $Script, $RepoRoot, $Version, $DistDirectory,
            ([string]$Options["PrebuiltValidatorDirectory"]), $Log
        $VariantJobs += $Job
        Write-Host "Started Windows $Name"
        continue
    }
    if ($Name -like "nvidia-sm*" -and $Name -ne $NvidiaTemplate) {
        $TemplateDist = Join-Path $RepoRoot "build/distribution-$NvidiaTemplate-dist"
        $TemplateArchive = @(Get-ChildItem $TemplateDist -Filter "ForeverTAS-*-windows-*.zip")
        if ($TemplateArchive.Count -ne 1) { throw "Missing NVIDIA runtime template" }
        while ($VariantJobs.Count -ge $VariantJobLimit) {
            $Finished = Wait-Job -Job $VariantJobs -Any
            Receive-VariantJob $Finished
            $VariantJobs = @($VariantJobs | Where-Object { $_.Id -ne $Finished.Id })
        }
        $Log = Join-Path $TemporaryDist "variant-build.log"
        New-Item -ItemType Directory -Force -Path $TemporaryDist | Out-Null
        $Script = Join-Path $RepoRoot "packaging/release/build-windows-nvidia-variant.ps1"
        $Job = Start-Job -Name $Name -ScriptBlock {
            param($Script, $Root, $FlavorName, $ReleaseVersion, $Template,
                $Output, $Prebuilt, $LogPath)
            & $Script -RepoRoot $Root -Name $FlavorName -Version $ReleaseVersion `
                -TemplateArchive $Template -DistDirectory $Output `
                -PrebuiltValidatorDirectory $Prebuilt *> $LogPath
            if (-not $?) { throw "Variant build failed: $FlavorName" }
        } -ArgumentList $Script, $RepoRoot, $Name, $Version,
            $TemplateArchive[0].FullName, $DistDirectory,
            ([string]$Options["PrebuiltValidatorDirectory"]), $Log
        $VariantJobs += $Job
        Write-Host "Started Windows $Name"
        continue
    }
    Write-Host "Building Windows $Name"
    & (Join-Path $RepoRoot "packaging/windows/build-portable.ps1") @Options
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed for $Name" }
    if ($Name -like "nvidia-sm*" -and $Name -ne $NvidiaTemplate) {
        $TemplateDist = Join-Path $RepoRoot "build/distribution-$NvidiaTemplate-dist"
        $TemplateArchive = @(Get-ChildItem $TemplateDist -Filter "ForeverTAS-*-windows-*.zip")
        if ($TemplateArchive.Count -ne 1) { throw "Missing NVIDIA runtime template" }
        & (Join-Path $RepoRoot "packaging/windows/package-from-template.ps1") `
            -BuildDirectory $BuildDirectory `
            -TemplateArchive $TemplateArchive[0].FullName `
            -Flavor $Name -DistDirectory $TemporaryDist
        if ($LASTEXITCODE -ne 0) { throw "Windows package failed for $Name" }
    }

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
    $PackageEvidence[$Name] = [ordered]@{
        sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $DistDirectory $InstallerName)).Hash.ToLowerInvariant()
        binary_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $BuildDirectory "bin/ForeverTAS.exe")).Hash.ToLowerInvariant()
        toolchain = ($Release.toolchains.windows | ConvertTo-Json -Compress)
        runtime_template = $(if ($Name -like "nvidia-sm*") { $NvidiaTemplate } else { $Name })
    }
}
foreach ($Job in $VariantJobs) {
    Wait-Job $Job | Out-Null
    Receive-VariantJob $Job
}

if (Test-Path (Join-Path $RepoRoot ".git")) {
    $CurrentHead = (git -C $RepoRoot rev-parse HEAD).Trim()
    $Dirty = (git -C $RepoRoot status --porcelain=v1) -join ""
    if ($CurrentHead -ne $SourceCommit -or $Dirty) {
        throw "ForeverTAS source changed during Windows packaging"
    }
}
$RecordPath = Join-Path $DistDirectory "windows-source.json"
if (Test-Path $RecordPath) {
    $Previous = Get-Content $RecordPath -Raw | ConvertFrom-Json
    if ($Previous.schema -eq 1 -and
            $Previous.forevertas -eq $SourceCommit -and
            $Previous.forevervalidator -eq $ValidatorCommit) {
        foreach ($Property in $Previous.packages.PSObject.Properties) {
            if (-not $PackageEvidence.Contains($Property.Name)) {
                $PackageEvidence[$Property.Name] = $Property.Value
            }
        }
    }
}
$Record = [ordered]@{
    schema = 1
    forevertas = $SourceCommit
    forevervalidator = $ValidatorCommit
    packages = $PackageEvidence
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($RecordPath, "$Record`n",
    (New-Object Text.UTF8Encoding($false)))

Write-Host "Built $($Flavors.Count) Windows installer flavors in $DistDirectory"
