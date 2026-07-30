$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
function Read-RepoFile([string]$relativePath) {
    Get-Content -Raw (Join-Path $root $relativePath)
}
function Require-Match([string]$text, [string]$pattern, [string]$message) {
    if ($text -notmatch $pattern) { throw $message }
}

$resources = Read-RepoFile 'source/common/rendering/nri/shaders/Include/SmokeResources.hlsli'
$medium = Read-RepoFile 'source/common/rendering/nri/shaders/SmokeEvaluateMedium.cs.hlsl'
$grid = Read-RepoFile 'source/common/rendering/nri/shaders/SmokeEvaluateGrid.cs.hlsl'
$resolve = Read-RepoFile 'source/common/rendering/nri/shaders/SmokeResolveVolume.cs.hlsl'
$temporal = Read-RepoFile 'source/common/rendering/nri/shaders/SmokeTemporalVolume.cs.hlsl'
$settings = Read-RepoFile 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'

Require-Match $resources 'NRI_SMOKE_CARRIER_VALID' 'Carrier validity is not independently encoded.'
Require-Match $resources 'NRI_SMOKE_RADIANCE_VALID' 'Radiance validity is not independently encoded.'
Require-Match $resources 'CARRIER_GENERATION_SHIFT' 'Carrier generation is not published.'
Require-Match $resources 'RADIANCE_GENERATION_SHIFT' 'Radiance generation is not published.'
Require-Match $resources 'CARRIER_AGE_SHIFT' 'Carrier age is not published.'
Require-Match $resources 'RADIANCE_AGE_SHIFT' 'Radiance age is not published.'
Require-Match $resources 'NRI_SMOKE_FALLBACK_SHIFT' 'Fallback identity is not published.'
Require-Match $resources 'NRI_SMOKE_RADIANCE_UNRESOLVED' 'Unresolved radiance work is not explicit.'
Require-Match $medium 'SmokeFroxelCarrierMetadata\(gSmokeConstants\.SimulationEpoch\)' 'Particle materialization does not publish current carrier authority.'
Require-Match $grid 'SmokeFroxelCarrierMetadata\(gSmokeConstants\.SimulationEpoch\)' 'Grid materialization does not publish current carrier authority.'
Require-Match $resolve 'debugMode >= 8u && debugMode <= 11u' 'Carrier/radiance debug views are missing.'
Require-Match $settings 'nri_ptsmokedebug, 0, 11' 'Carrier/radiance debug views are not reachable from the smoke debug control.'

# Current optical depth must remain authoritative regardless of history lighting.
Require-Match $temporal 'history = float4\(clampedNormalized \* \(1\.0 - exp\(-current\.a\)\), current\.a\)' 'History radiance is not re-premultiplied by current opacity.'
Require-Match $temporal 'resolved\.a = current\.a' 'Temporal reconstruction can manufacture stale opacity.'

Write-Output 'Smoke carrier/radiance contract tests passed.'
