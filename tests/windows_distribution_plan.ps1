$ErrorActionPreference = "Stop"
$Script = Join-Path $PSScriptRoot "../packaging/release/build-distribution-windows.ps1"
$Full = & $Script -PlanOnly | ConvertFrom-Json
if ($Full.flavors.Count -ne 19 -or $Full.build_order.Count -ne 19 -or
        $Full.build_order[0] -ne "universal" -or
        $Full.build_order[1] -ne "nvidia-sm75") {
    throw "Full Windows release must build QML once, then the NVIDIA template"
}
$Subset = & $Script -PlanOnly -Flavor nvidia-sm50 | ConvertFrom-Json
if ($Subset.flavors.Count -ne 1 -or
        @($Subset.build_order) -join "," -ne "universal,nvidia-sm75,nvidia-sm50") {
    throw "A selected NVIDIA flavor must use universal QML and the SM75 runtime template"
}
"Windows distribution plan OK"
