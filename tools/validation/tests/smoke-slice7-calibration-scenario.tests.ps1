Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$path = Join-Path $repo "tools/validation/perf-scenarios/gpu-time-slice7-large-voxel-smoke-256-d3d12.json"
$scenario = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
$args = @($scenario.launch.extraArgs) -join " "

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

Require ([int]$scenario.capture.loopTraceFrames -ge 272) "joined calibration must cover warmup plus 256 compact frames"
Require ([string]$scenario.capture.stopWhenPrefix -eq "PERF compact capture complete:") "joined calibration must stop on compact completion"
Require ($args -match "nri_ptlightbounces 2") "joined calibration must pin light bounces=2"
Require ($args -match "nri_ptmirrorbounces 2") "joined calibration must pin mirror bounces=2"
Require ($args -match "nri_ptportaldepth 3") "joined calibration must pin portal depth=3"
Require ($args -match "nri_ptemissivesamples 1") "joined calibration must pin emissive samples=1"
Require ($args -match "nri_ptsmoke true") "joined calibration must explicitly enable smoke"
Require ($args -match "nri_ptsmoke_test; wait 120; nri_ptsmokestatus") "joined calibration must mature a deterministic visible plume"
Require ($args -match "perf_fixedsimulationframes 272") "joined calibration must freeze the complete warmup/capture window"
Require ($args -match "perf_compactwarmupframes 16; perf_compactframes 256") "joined calibration must capture 256 settled compact frames"
foreach ($prefix in @(
    "PERF pt voxel preload release action NRI:", "PERF pt gpu timing NRI:",
    "PERF pt smoke gpu timing NRI:", "PERF pt smoke work NRI:",
    "PERF pt voxel gpu timing NRI:", "PERF compact capture complete:"
)) {
    Require (@($scenario.requiredPrefixes) -contains $prefix) "joined calibration is missing '$prefix'"
}

Write-Host "Smoke Slice 7 calibration scenario tests passed."
