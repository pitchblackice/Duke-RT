$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$relativePath) {
    $path = Join-Path $root $relativePath
    if (-not (Test-Path $path)) { throw "Missing analytic smoke source: $relativePath" }
    return Get-Content -Raw $path
}
function Require-Match([string]$text, [string]$pattern, [string]$message) {
    if ($text -notmatch $pattern) { throw $message }
}

$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeAnalyticData.hlsli'
$resources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeResources.hlsli'
$build = Read-Source 'source/common/rendering/nri/shaders/SmokeAnalyticBuildTiles.cs.hlsl'
$materialize = Read-Source 'source/common/rendering/nri/shaders/SmokeAnalyticMaterialize.cs.hlsl'
$direct = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeDirectCache.hlsli'
$emissive = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeEmissiveReservoir.hlsli'

Require-Match $data 'struct\s+SmokeAnalyticCarrier' 'Analytic carrier record is missing.'
Require-Match $data 'NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE\s+NRI_SMOKE_ANALYTIC_MAX_CARRIERS' 'Tile list must retain the complete fixed carrier pool.'
Require-Match $resources 'gSmokeAnalyticCarriers\s*:\s*register\(t2,\s*space0\)' 'Analytic carriers must use their own t2 input.'
Require-Match $resources 'gSmokeAnalyticTileHeaders\s*:\s*register\(u49,\s*space1\)' 'Analytic tile headers must use u49.'
Require-Match $resources 'gSmokeAnalyticTileIndices\s*:\s*register\(u50,\s*space1\)' 'Analytic tile indices must use u50.'
Require-Match $resources 'gSmokeAnalyticFroxelMedium\s*:\s*register\(u51,\s*space1\)' 'Analytic-only medium must use u51.'
Require-Match $build 'SmokeProjectSphereToFroxelBounds' 'Carrier projection must conservatively bound view work.'
Require-Match $build 'InterlockedAdd\(gSmokeAnalyticTileHeaders\[tileIndex\]\.Count' 'Tile construction must claim bounded list slots atomically.'
Require-Match $materialize 'SmokeSphereSegmentKernelAverage' 'Sphere carriers must use exact slice-segment integration.'
Require-Match $materialize 'gSmokeAnalyticFroxelMedium\[index\]\s*=\s*analyticMedium' 'Analytic medium must remain separately attributable.'
Require-Match $materialize 'previousOwnership\s*\|\s*NRI_SMOKE_FROXEL_CARRIER_ANALYTIC' 'Materialization must add explicit analytic ownership.'
if ($materialize -match 'kPromptFallbackOpticalScale|gSmokePromptOutcomes|NRI_SMOKE_PROMPT') {
    throw 'First-class analytic materialization must not inherit prompt policy or scaling.'
}
Require-Match $direct 'SmokeFroxelHasAnalyticCarrier' 'Analytic smoke must use receiver-sampled direct lighting.'
Require-Match $emissive '!SmokeFroxelHasAnalyticCarrier' 'Analytic smoke must not claim grid emissive-world ownership.'

Write-Host 'Smoke analytic carrier structural tests passed.'
