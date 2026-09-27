param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$TemplateArchive,
    [Parameter(Mandatory = $true)][string]$Flavor,
    [Parameter(Mandatory = $true)][string]$DistDirectory,
    [string]$TemplateDirectory = "",
    [switch]$AssemblyOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($Flavor -notmatch '^nvidia-sm[0-9]+$') {
    throw "Only matching NVIDIA compute builds can use the NVIDIA template"
}
$TemplateArchive = (Resolve-Path $TemplateArchive).Path
$Executable = Join-Path $BuildDirectory "bin/ForeverTAS.exe"
$Worker = Join-Path $BuildDirectory "bin/forevertas-simulation-debug-worker.exe"
if (-not (Test-Path $Executable -PathType Leaf) -or
        -not (Test-Path $Worker -PathType Leaf)) {
    throw "The compute build is missing an executable"
}
$Cache = Get-Content (Join-Path $BuildDirectory "CMakeCache.txt") -Raw
if ($Cache -notmatch "(?m)^FOREVERTAS_UPDATE_ASSET_ID:INTERNAL=(windows-$Flavor-x86_64)\r?$") {
    throw "Compute build has the wrong update asset identity"
}
$AssetId = $Matches[1]
$Archive = Join-Path $DistDirectory ([IO.Path]::GetFileName($TemplateArchive))
New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
Copy-Item -LiteralPath $TemplateArchive -Destination $Archive -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$Zip = [IO.Compression.ZipFile]::Open($Archive, [IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($Replacement in @(@("ForeverTAS.exe", $Executable),
                               @("forevertas-simulation-debug-worker.exe", $Worker))) {
        $Entries = @($Zip.Entries | Where-Object {
            $_.FullName -match "(^|[/\\])$([regex]::Escape($Replacement[0]))$"
        })
        if ($Entries.Count -ne 1) { throw "The template lacks $($Replacement[0])" }
        $EntryName = $Entries[0].FullName
        $Timestamp = $Entries[0].LastWriteTime
        $Entries[0].Delete()
        $NewEntry = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $Zip, $Replacement[1], $EntryName,
            [IO.Compression.CompressionLevel]::Optimal)
        $NewEntry.LastWriteTime = $Timestamp
    }
} finally {
    $Zip.Dispose()
}
$Zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    foreach ($Replacement in @(@("ForeverTAS.exe", $Executable),
                               @("forevertas-simulation-debug-worker.exe", $Worker))) {
        $Entry = @($Zip.Entries | Where-Object {
            $_.FullName -match "(^|[/\\])$([regex]::Escape($Replacement[0]))$"
        })
        if ($Entry.Count -ne 1) { throw "The overlay has no $($Replacement[0])" }
        $Stream = $Entry[0].Open()
        try {
            $Hasher = [Security.Cryptography.SHA256]::Create()
            $Hash = [BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-', '')
        } finally {
            $Stream.Dispose()
            $Hasher.Dispose()
        }
        if ($Hash -ne (Get-FileHash $Replacement[1] -Algorithm SHA256).Hash) {
            throw "The ZIP overlay differs from $($Replacement[0])"
        }
    }
} finally {
    $Zip.Dispose()
}
if ($AssemblyOnly) { return }
$Staging = Join-Path $BuildDirectory "runtime-template-stage"
try {
    Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
    if ($TemplateDirectory -and (Test-Path $TemplateDirectory -PathType Container)) {
        Copy-Item -LiteralPath $TemplateDirectory -Destination $Staging -Recurse
        $Roots = @(Get-ChildItem $Staging -Directory)
        if ($Roots.Count -ne 1) { throw "The runtime template has no single root" }
        foreach ($Replacement in @(@("ForeverTAS.exe", $Executable),
                                   @("forevertas-simulation-debug-worker.exe", $Worker))) {
            $Destination = Join-Path $Roots[0].FullName $Replacement[0]
            Remove-Item -LiteralPath $Destination -Force
            Copy-Item -LiteralPath $Replacement[1] -Destination $Destination
        }
    } else {
        Expand-Archive -LiteralPath $Archive -DestinationPath $Staging
    }
    & (Join-Path $PSScriptRoot "test-portable.ps1") `
        -Archive $Archive -ExtractedDirectory $Staging
    & (Join-Path $PSScriptRoot "build-installer.ps1") `
        -Archive $Archive -AssetId $AssetId -DistDirectory $DistDirectory `
        -ExtractedDirectory $Staging
} finally {
    Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
}
