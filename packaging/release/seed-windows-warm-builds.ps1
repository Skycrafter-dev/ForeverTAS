param(
    [Parameter(Mandatory = $true)][string]$DonorRoot,
    [string]$TargetRoot = "",
    [string[]]$Flavor = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $TargetRoot) {
    $TargetRoot = Join-Path $PSScriptRoot "../.."
}
$DonorRoot = (Resolve-Path $DonorRoot).Path
$TargetRoot = (Resolve-Path $TargetRoot).Path
if ($DonorRoot -eq $TargetRoot) {
    throw "Warm builds require a separate donor checkout"
}
$DonorValidator = Join-Path $DonorRoot ".dependencies/ForeverValidator"
$TargetValidator = Join-Path $TargetRoot ".dependencies/ForeverValidator"
$DonorManifest = Get-Content (Join-Path $DonorRoot "packaging/release/manifest.json") -Raw |
    ConvertFrom-Json
$TargetManifest = Get-Content (Join-Path $TargetRoot "packaging/release/manifest.json") -Raw |
    ConvertFrom-Json
if (($DonorManifest.toolchains.windows | ConvertTo-Json -Compress) -ne
        ($TargetManifest.toolchains.windows | ConvertTo-Json -Compress) -or
        $DonorManifest.cuda.version -ne $TargetManifest.cuda.version) {
    throw "Donor and target require different Windows toolchains"
}
$ValidatorPin = [string]$TargetManifest.sources.forevervalidator.commit
$DonorCommit = (Get-Content (Join-Path $DonorRoot ".release-source-commit") -Raw).Trim()
$DonorValidatorCommit = (Get-Content (Join-Path $DonorValidator ".release-source-commit") -Raw).Trim()
$TargetCommit = (git -C $TargetRoot rev-parse HEAD).Trim()
$TargetValidatorCommit = (git -C $TargetValidator rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $DonorCommit -notmatch '^[0-9a-f]{40}$' -or
        $DonorValidatorCommit -ne $ValidatorPin -or
        $TargetValidatorCommit -ne $ValidatorPin -or
        (git -C $TargetRoot status --porcelain) -or
        (git -C $TargetValidator status --porcelain)) {
    throw "Warm seed requires clean, pinned source checkouts"
}
foreach ($Relative in @("packaging/release/manifest.json",
        "packaging/release/ensure-windows-cuda.ps1")) {
    $DonorHash = (Get-FileHash (Join-Path $DonorRoot $Relative) -Algorithm SHA256).Hash
    $TargetHash = (Get-FileHash (Join-Path $TargetRoot $Relative) -Algorithm SHA256).Hash
    if ($DonorHash -ne $TargetHash) {
        throw "Warm seed toolchain input differs: $Relative"
    }
}
if ($Flavor.Count -eq 0) {
    $Flavor = @("universal") +
        @($TargetManifest.distribution.nvidia_sm | ForEach-Object { "nvidia-sm$_" }) +
        @("amd-rx7000-rx9000")
}
$Supported = @("universal") +
    @($TargetManifest.distribution.nvidia_sm | ForEach-Object { "nvidia-sm$_" }) +
    @("amd-rx7000-rx9000")
foreach ($Name in $Flavor) {
    if ($Supported -cnotcontains $Name) { throw "Unknown flavor: $Name" }
    $Source = Join-Path $DonorRoot "build/distribution-$Name"
    $Destination = Join-Path $TargetRoot "build/distribution-$Name"
    $Cache = Join-Path $Source "CMakeCache.txt"
    if (-not (Test-Path $Cache -PathType Leaf)) {
        throw "Missing donor warm build for $Name"
    }
    $CacheText = [IO.File]::ReadAllText($Cache)
    if ($CacheText -notmatch "(?m)^FOREVERTAS_DISTRIBUTION_FLAVOR:STRING=$Name\r?$" -or
            $CacheText -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=') {
        throw "Incompatible donor CMake cache for $Name"
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $TargetRoot "build") | Out-Null
foreach ($Name in $Flavor) {
    $Source = Join-Path $DonorRoot "build/distribution-$Name"
    $Destination = Join-Path $TargetRoot "build/distribution-$Name"
    if (-not (Test-Path $Destination)) {
        if ($env:OS -eq "Windows_NT") {
            & robocopy $Source $Destination /E /COPY:DAT /DCOPY:DAT /R:0 /W:0 /MT:8 /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -gt 7) { throw "Could not copy warm $Name build" }
        } else {
            Copy-Item -LiteralPath $Source -Destination $Destination -Recurse
        }
    }
    $CachePath = Join-Path $Destination "CMakeCache.txt"
    $CacheText = [IO.File]::ReadAllText($CachePath)
    $CacheText = $CacheText -replace
        [regex]::Escape($DonorRoot.Replace('\', '/')),
        $TargetRoot.Replace('\', '/')
    $CacheText = $CacheText -replace [regex]::Escape($DonorRoot), $TargetRoot
    if ($CacheText -match [regex]::Escape($DonorRoot.Replace('\', '/')) -or
            $CacheText -match [regex]::Escape($DonorRoot)) {
        throw "Could not relocate the warm CMake cache for $Name"
    }
    [IO.File]::WriteAllText($CachePath, $CacheText,
        [Text.UTF8Encoding]::new($false))
}

function Restore-MatchingSourceTimes([string]$Donor, [string]$Target) {
    $Raw = (& git -C $Target ls-files -z) -join ""
    if ($LASTEXITCODE -ne 0) { throw "Could not list tracked source in $Target" }
    $Matching = 0
    $Changed = 0
    foreach ($Relative in $Raw.Split([char[]]@([char]0),
                                     [StringSplitOptions]::RemoveEmptyEntries)) {
        $Source = Join-Path $Donor $Relative
        $Destination = Join-Path $Target $Relative
        if (-not (Test-Path $Destination -PathType Leaf)) { continue }
        $Timestamp = [DateTime]::UtcNow
        if ((Test-Path $Source -PathType Leaf) -and
                (Get-FileHash $Source -Algorithm SHA256).Hash -eq
                (Get-FileHash $Destination -Algorithm SHA256).Hash) {
            $Timestamp = (Get-Item $Source).LastWriteTimeUtc
            $Matching++
        } else {
            $Changed++
        }
        (Get-Item $Destination).LastWriteTimeUtc = $Timestamp
    }
    return [ordered]@{ matching = $Matching; changed = $Changed }
}

$TasSource = Restore-MatchingSourceTimes $DonorRoot $TargetRoot
$ValidatorSource = Restore-MatchingSourceTimes $DonorValidator $TargetValidator
$EvidencePath = Join-Path $TargetRoot "build/windows-warm-seed.json"
$Prior = if (Test-Path $EvidencePath) {
    Get-Content $EvidencePath -Raw | ConvertFrom-Json
} else { $null }
$Archives = @{}
if ($Prior -and $Prior.donor.forevertas -eq $DonorCommit -and
        $Prior.recipient.forevervalidator -eq $TargetValidatorCommit -and
        $Prior.PSObject.Properties.Name -contains "manifest_sha256" -and
        $Prior.manifest_sha256 -eq
        (Get-FileHash (Join-Path $TargetRoot "packaging/release/manifest.json") -Algorithm SHA256).Hash.ToLower() -and
        $Prior.PSObject.Properties.Name -contains "validator_archives") {
    foreach ($Entry in $Prior.validator_archives.PSObject.Properties) {
        $Archives[$Entry.Name] = $Entry.Value
    }
}
foreach ($Name in $Flavor) {
    $Source = Join-Path $DonorRoot "build/distribution-$Name/_deps/forevervalidator-build"
    $Destination = Join-Path $TargetRoot "build/distribution-$Name/_deps/forevervalidator-build"
    $Hashes = [ordered]@{}
    foreach ($Component in @("core", "native")) {
        $File = "forevervalidator_$Component.lib"
        $DonorArchive = Join-Path $Source $File
        $TargetArchive = Join-Path $Destination $File
        if (-not (Test-Path $DonorArchive -PathType Leaf) -or
                -not (Test-Path $TargetArchive -PathType Leaf)) {
            throw "Missing warm Validator archive for $Name/$Component"
        }
        $DonorHash = (Get-FileHash $DonorArchive -Algorithm SHA256).Hash.ToLower()
        $TargetHash = (Get-FileHash $TargetArchive -Algorithm SHA256).Hash.ToLower()
        if ($DonorHash -ne $TargetHash) {
            throw "Changed warm Validator archive for $Name/$Component"
        }
        $Hashes[$Component] = $TargetHash
    }
    $Archives[$Name] = $Hashes
}
$Evidence = [ordered]@{
    schema = 1
    donor = [ordered]@{ forevertas = $DonorCommit; forevervalidator = $DonorValidatorCommit }
    recipient = [ordered]@{ forevertas = $TargetCommit; forevervalidator = $TargetValidatorCommit }
    manifest_sha256 = (Get-FileHash (Join-Path $TargetRoot "packaging/release/manifest.json") -Algorithm SHA256).Hash.ToLower()
    flavors = @($Archives.Keys | Sort-Object)
    validator_archives = $Archives
    source_files = [ordered]@{ forevertas = $TasSource; forevervalidator = $ValidatorSource }
}
[IO.File]::WriteAllText($EvidencePath,
    ($Evidence | ConvertTo-Json -Depth 6) + "`n", [Text.UTF8Encoding]::new($false))
Write-Host "Seeded $($Flavor.Count) Windows warm builds; $($TasSource.matching + $ValidatorSource.matching) source files reused"
