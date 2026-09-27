param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$AssetId,
    [string]$DistDirectory = "",
    [string]$ExtractedDirectory = "",
    [ValidateSet("lzma2/fast", "zip")][string]$CompressionMode = "lzma2/fast"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($AssetId -notmatch '^windows(?:-[a-z0-9-]+)?-(x86_64|arm64)$') {
    throw "Invalid update asset identity: $AssetId"
}
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Archive = (Resolve-Path $Archive).Path
if ([IO.Path]::GetFileName($Archive) -notmatch
        '^ForeverTAS-([0-9]+\.[0-9]+\.[0-9]+)-windows-[a-z0-9_-]+\.zip$') {
    throw "Portable archive has no valid ForeverTAS version"
}
$Version = $Matches[1]
if ([string]::IsNullOrWhiteSpace($DistDirectory)) {
    $DistDirectory = Join-Path $RepoRoot "dist"
}
New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
$Compiler = $env:ISCC_PATH
if (-not $Compiler) {
    $Candidates = @(
        (Join-Path $env:ProgramFiles "Inno Setup 7/ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6/ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs/Inno Setup 6/ISCC.exe")
    )
    $Compiler = $Candidates | Where-Object { Test-Path $_ } |
        Select-Object -First 1
}
if (-not $Compiler) {
    $Command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($Command) { $Compiler = $Command.Source }
}
if (-not $Compiler -or -not (Test-Path $Compiler)) {
    throw "Install Inno Setup 6 or 7, or set ISCC_PATH to ISCC.exe"
}
$Temporary = if ($ExtractedDirectory) {
    (Resolve-Path $ExtractedDirectory).Path
} else {
    Join-Path ([IO.Path]::GetTempPath()) `
        "forevertas-installer-$([guid]::NewGuid().ToString('N'))"
}
try {
    if (-not $ExtractedDirectory) {
        Expand-Archive -LiteralPath $Archive -DestinationPath $Temporary
    }
    $Roots = @(Get-ChildItem -LiteralPath $Temporary -Directory)
    if ($Roots.Count -ne 1 -or
            -not (Test-Path (Join-Path $Roots[0].FullName "ForeverTAS.exe"))) {
        throw "Portable archive does not contain one ForeverTAS install tree"
    }
    $Executable = Get-Item (Join-Path $Roots[0].FullName "ForeverTAS.exe")
    if ($Executable.VersionInfo.ProductVersion -notin @($Version, "$Version.0")) {
        throw "ForeverTAS.exe product version differs from the archive"
    }
    $OutputBaseFilename = "ForeverTAS-$Version-$AssetId-Setup"
    & $Compiler "/DAppVersion=$Version" `
        "/DCompressionMode=$CompressionMode" `
        "/DSourceDir=$($Roots[0].FullName)" `
        "/DOutputDir=$DistDirectory" `
        "/DOutputBaseFilename=$OutputBaseFilename" `
        (Join-Path $PSScriptRoot "ForeverTAS.iss")
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed" }
    $Installer = Join-Path $DistDirectory "$OutputBaseFilename.exe"
    if (-not (Test-Path $Installer)) { throw "Installer was not created" }
    $Hash = (Get-FileHash -Algorithm SHA256 $Installer).Hash.ToLowerInvariant()
    "$Hash  $OutputBaseFilename.exe" |
        Set-Content -NoNewline -Path "$Installer.sha256"
    Write-Host "Created $Installer"
} finally {
    if (-not $ExtractedDirectory) {
        Remove-Item -LiteralPath $Temporary -Recurse -Force -ErrorAction SilentlyContinue
    }
}
