param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [string]$WorkingDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Archive = (Resolve-Path $Archive).Path
$RemoveWorkingDirectory = [string]::IsNullOrWhiteSpace($WorkingDirectory)
if ($RemoveWorkingDirectory) {
    $WorkingDirectory = Join-Path ([IO.Path]::GetTempPath()) `
        "ForeverTAS-portable-$([guid]::NewGuid().ToString('N'))"
}

$OriginalEnvironment = @{
    PATH = $env:PATH
    QT_PLUGIN_PATH = $env:QT_PLUGIN_PATH
    QML2_IMPORT_PATH = $env:QML2_IMPORT_PATH
    QML_IMPORT_PATH = $env:QML_IMPORT_PATH
    QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM
    QSG_RHI_BACKEND = $env:QSG_RHI_BACKEND
}

function Restore-Environment {
    foreach ($Entry in $OriginalEnvironment.GetEnumerator()) {
        if ($null -eq $Entry.Value) {
            Remove-Item "Env:$($Entry.Key)" -ErrorAction SilentlyContinue
        } else {
            Set-Item "Env:$($Entry.Key)" $Entry.Value
        }
    }
}

try {
    Remove-Item $WorkingDirectory -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null
    Expand-Archive -Path $Archive -DestinationPath $WorkingDirectory

    $Executables = @(
        Get-ChildItem $WorkingDirectory -Recurse -File -Filter "ForeverTAS.exe"
    )
    if ($Executables.Count -ne 1) {
        throw "Expected one ForeverTAS.exe in the archive, found $($Executables.Count)"
    }

    $Executable = $Executables[0]
    $ApplicationDirectory = $Executable.Directory.FullName
    foreach ($RequiredPath in @(
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Qml.dll",
        "Qt6Quick.dll",
        "qt.conf",
        "plugins\platforms\qwindows.dll",
        "qml\QtQuick\qtquick2plugin.dll"
    )) {
        $FullPath = Join-Path $ApplicationDirectory $RequiredPath
        if (-not (Test-Path $FullPath -PathType Leaf)) {
            throw "Missing deployed runtime file: $RequiredPath"
        }
    }
    $CudaRuntimeFamilies = [ordered]@{
        cudart = "cudart64_*.dll"
        nvrtc = "nvrtc64_120_0.dll"
        nvrtc_builtins = "nvrtc-builtins64_*.dll"
        nvJitLink = "nvJitLink_*.dll"
    }
    foreach ($Family in $CudaRuntimeFamilies.GetEnumerator()) {
        $RuntimeMatches = @(
            Get-ChildItem $ApplicationDirectory -File -Filter $Family.Value
        )
        if ($RuntimeMatches.Count -ne 1) {
            throw "Expected exactly one $($Family.Key) CUDA runtime DLL, found $($RuntimeMatches.Count)"
        }
    }

    $Dumpbin = (Get-Command "dumpbin.exe" -ErrorAction Stop).Source
    $SystemDirectory = Join-Path $env:SystemRoot "System32"
    $MissingDependencies = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $Binaries = @(
        Get-ChildItem $ApplicationDirectory -Recurse -File |
            Where-Object { $_.Extension -in @(".exe", ".dll") }
    )
    foreach ($Binary in $Binaries) {
        $DumpOutput = & $Dumpbin /nologo /dependents $Binary.FullName 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed for $($Binary.FullName):`n$DumpOutput"
        }

        foreach ($Line in $DumpOutput) {
            if ($Line -notmatch "^\s+([A-Za-z0-9_.+-]+\.dll)\s*$") {
                continue
            }

            $Dependency = $Matches[1]
            if ($Dependency -match "^(api-ms-win-|ext-ms-win-)") {
                continue
            }
            $BesideApplication = Join-Path $ApplicationDirectory $Dependency
            $BesideBinary = Join-Path $Binary.Directory.FullName $Dependency
            $InSystemDirectory = Join-Path $SystemDirectory $Dependency
            $IsPackaged = (Test-Path $BesideApplication -PathType Leaf) -or
                (Test-Path $BesideBinary -PathType Leaf)
            $MustBePackaged = $Dependency -match `
                "^(concrt|msvcp|vcruntime)[0-9_]*\.dll$"
            if (-not $IsPackaged -and
                    ($MustBePackaged -or
                        -not (Test-Path $InSystemDirectory -PathType Leaf))) {
                [void]$MissingDependencies.Add(
                    "$Dependency (required by $($Binary.Name))")
            }
        }
    }
    if ($MissingDependencies.Count -ne 0) {
        $Details = ($MissingDependencies | Sort-Object) -join "`n  "
        throw "The portable tree has unresolved DLL dependencies:`n  $Details"
    }

    $env:PATH = "$SystemDirectory;$env:SystemRoot"
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:QML_IMPORT_PATH -ErrorAction SilentlyContinue
    $env:QT_QPA_PLATFORM = "windows"
    $env:QSG_RHI_BACKEND = "software"

    $ProcessStartInfo = [Diagnostics.ProcessStartInfo]::new()
    $ProcessStartInfo.FileName = $Executable.FullName
    $ProcessStartInfo.Arguments = "--qml-smoke-test"
    $ProcessStartInfo.WorkingDirectory = $ApplicationDirectory
    $ProcessStartInfo.UseShellExecute = $false
    $ProcessStartInfo.RedirectStandardOutput = $true
    $ProcessStartInfo.RedirectStandardError = $true
    $Process = [Diagnostics.Process]::new()
    $Process.StartInfo = $ProcessStartInfo
    if (-not $Process.Start()) {
        throw "Failed to start the packaged application"
    }
    $StandardOutput = $Process.StandardOutput.ReadToEndAsync()
    $StandardError = $Process.StandardError.ReadToEndAsync()
    if (-not $Process.WaitForExit(60000)) {
        $Process.Kill()
        $Process.WaitForExit()
        $Output = $StandardOutput.GetAwaiter().GetResult()
        $ErrorOutput = $StandardError.GetAwaiter().GetResult()
        throw "The packaged application did not finish its smoke test within 60 seconds." +
            "`n$Output`n$ErrorOutput"
    }
    $Process.WaitForExit()
    $Output = $StandardOutput.GetAwaiter().GetResult()
    $ErrorOutput = $StandardError.GetAwaiter().GetResult()
    if ($Process.ExitCode -ne 0) {
        throw "Packaged application exited with $($Process.ExitCode).`n$Output`n$ErrorOutput"
    }

    Write-Host "Portable ZIP dependency closure and QML startup test passed."
} finally {
    Restore-Environment
    if ($RemoveWorkingDirectory) {
        Remove-Item $WorkingDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
