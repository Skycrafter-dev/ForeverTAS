$ErrorActionPreference = "Stop"
$Temporary = Join-Path ([IO.Path]::GetTempPath()) `
    "forevertas-template-test-$([guid]::NewGuid().ToString('N'))"
try {
    $TemplateRoot = Join-Path $Temporary "template/ForeverTAS-0.2.4-windows-x86_64"
    $BuildDirectory = Join-Path $Temporary "build"
    $DistDirectory = Join-Path $Temporary "dist"
    New-Item -ItemType Directory -Force -Path $TemplateRoot,
        (Join-Path $BuildDirectory "bin"),
        (Join-Path $TemplateRoot "qml/ForeverTAS") | Out-Null
    Set-Content (Join-Path $TemplateRoot "ForeverTAS.exe") "template application"
    Set-Content (Join-Path $TemplateRoot "forevertas-simulation-debug-worker.exe") "template worker"
    Set-Content (Join-Path $TemplateRoot "Qt6Core.dll") "shared runtime"
    Set-Content (Join-Path $TemplateRoot "qml/ForeverTAS/qmldir") "shared QML manifest"
    Set-Content (Join-Path $TemplateRoot "qml/ForeverTAS/forevertas_qml.dll") "shared QML plugin"
    Set-Content (Join-Path $BuildDirectory "bin/ForeverTAS.exe") "sm50 application"
    Set-Content (Join-Path $BuildDirectory "bin/forevertas-simulation-debug-worker.exe") "sm50 worker"
    Set-Content (Join-Path $BuildDirectory "CMakeCache.txt") `
        "FOREVERTAS_UPDATE_ASSET_ID:INTERNAL=windows-nvidia-sm50-x86_64"
    $TemplateArchive = Join-Path $Temporary "ForeverTAS-0.2.4-windows-x86_64.zip"
    Compress-Archive -LiteralPath $TemplateRoot -DestinationPath $TemplateArchive
    & (Join-Path $PSScriptRoot "../packaging/windows/package-from-template.ps1") `
        -BuildDirectory $BuildDirectory -TemplateArchive $TemplateArchive `
        -Flavor "nvidia-sm50" -DistDirectory $DistDirectory -AssemblyOnly
    $Extracted = Join-Path $Temporary "extracted"
    Expand-Archive -LiteralPath (Join-Path $DistDirectory `
        "ForeverTAS-0.2.4-windows-x86_64.zip") -DestinationPath $Extracted
    $Root = Join-Path $Extracted "ForeverTAS-0.2.4-windows-x86_64"
    if ((Get-Content (Join-Path $Root "ForeverTAS.exe") -Raw).Trim() -ne
            "sm50 application" -or
            (Get-Content (Join-Path $Root "forevertas-simulation-debug-worker.exe") -Raw).Trim() -ne
            "sm50 worker" -or
            (Get-Content (Join-Path $Root "Qt6Core.dll") -Raw).Trim() -ne
            "shared runtime" -or
            (Get-Content (Join-Path $Root "qml/ForeverTAS/qmldir") -Raw).Trim() -ne
            "shared QML manifest" -or
            (Get-Content (Join-Path $Root "qml/ForeverTAS/forevertas_qml.dll") -Raw).Trim() -ne
            "shared QML plugin") {
        throw "The Windows portable template did not preserve the common runtime and QML module while replacing compute executables"
    }
    "Windows template assembly OK"
} finally {
    Remove-Item $Temporary -Recurse -Force -ErrorAction SilentlyContinue
}
