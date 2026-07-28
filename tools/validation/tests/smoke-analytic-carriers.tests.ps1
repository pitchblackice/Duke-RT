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
$carrierEmissiveBuild = Read-Source 'source/common/rendering/nri/shaders/SmokeAnalyticEmissiveBuild.cs.hlsl'
$carrierEmissiveResolve = Read-Source 'source/common/rendering/nri/shaders/SmokeAnalyticEmissiveResolve.cs.hlsl'
$smoke = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_contracts.h'
$clear = Read-Source 'source/common/rendering/nri/shaders/SmokeClear.cs.hlsl'

Require-Match $data 'struct\s+SmokeAnalyticCarrier' 'Analytic carrier record is missing.'
Require-Match $data 'NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE\s+NRI_SMOKE_ANALYTIC_MAX_CARRIERS' 'Tile list must retain the complete fixed carrier pool.'
Require-Match $resources 'gSmokeAnalyticCarriers\s*:\s*register\(t2,\s*space0\)' 'Analytic carriers must use their own t2 input.'
Require-Match $resources 'gSmokeAnalyticTileHeaders\s*:\s*register\(u49,\s*space1\)' 'Analytic tile headers must use u49.'
Require-Match $resources 'gSmokeAnalyticTileIndices\s*:\s*register\(u50,\s*space1\)' 'Analytic tile indices must use u50.'
Require-Match $resources 'gSmokeAnalyticFroxelMedium\s*:\s*register\(u51,\s*space1\)' 'Analytic-only medium must use u51.'
Require-Match $resources 'gSmokeAnalyticEmissiveCurrent\s*:\s*register\(u52,\s*space1\)' 'Analytic emissive anchor bank A must use u52.'
Require-Match $resources 'gSmokeAnalyticEmissiveHistory\s*:\s*register\(u53,\s*space1\)' 'Analytic emissive anchor bank B must use u53.'
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
Require-Match $data 'SmokeAnalyticCarrierSlot' 'Carrier records must expose stable physical slot identity.'
Require-Match $data 'LightGroupGeneration' 'Carrier records must expose exact event-light generation identity.'
Require-Match $data 'SmokeAnalyticLightIdentityMatches' 'Anchor fields must reject stale group generations and epochs.'
Require-Match $carrierEmissiveBuild 'NRI_SMOKE_ANALYTIC_LIGHT_BUILD_PENDING' 'Anchor fields must build only on admission or an atomic profile rebuild.'
Require-Match $carrierEmissiveBuild 'for\s*\(uint anchorIndex.*anchorCount' 'Admission must construct every declared event anchor together.'
Require-Match $carrierEmissiveResolve 'SmokeEvaluateAnalyticLightField' 'Resolve must smoothly evaluate the event-owned anchor field.'
Require-Match $carrierEmissiveResolve 'analyticSource\s*\+=' 'Resolve must accumulate every overlapping analytic carrier.'
if ($carrierEmissiveResolve -match 'SmokeEmissiveVisible(?:Filtered)?\(') {
    throw 'Analytic field application must issue no per-froxel visibility ray.'
}
if ($carrierEmissiveBuild -match 'EmissiveTemporal|FrameIndex\s*-\s*1') {
    throw 'Admission-time analytic lighting must not retain per-carrier temporal convergence.'
}
Require-Match $smoke 'runCarrierEmissive\s*=\s*analyticCount\s*>\s*0u\s*&&\s*worldEmissiveReady' 'Carrier emissive reuse must be restricted to the world-field route.'
Require-Match $smoke '!renderParticles\s*&&\s*!mSettings\.emissiveReference' 'Particle/compare and reference modes must retain their established lighting routes.'
Require-Match $contracts 'analyticLightBuildEvents[\s\S]*analyticLightApplyVisibilityRays' 'Analytic build/apply counters must have a dedicated GPU readback contract.'
Require-Match $clear 'AnalyticLightBuildEvents\s*=\s*0u[\s\S]*AnalyticLightApplyVisibilityRays\s*=\s*0u' 'Every analytic counter must reset at the frame boundary.'
Require-Match $smoke 'PERF pt smoke analytic light NRI:[^\n]*source_frame=%llu[^\n]*apply_visibility_rays=%u[^\n]*apply_rays_zero=%s' 'Frame-keyed compact telemetry must expose the zero apply-ray invariant.'
Require-Match $smoke 'analyticSnapshot\s*=\s*mAnalyticCarriers\.GetSnapshot' 'GPU readback metadata must retain the matching CPU admission snapshot.'

Write-Host 'Smoke analytic carrier structural tests passed.'
