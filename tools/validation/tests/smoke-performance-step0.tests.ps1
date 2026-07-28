$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

$timingHeader = Read-Source 'source\common\rendering\nri\system\nri_gpu_timing.h'
$timingOwner = Read-Source 'source\common\rendering\nri\system\nri_gpu_timing.cpp'
$perfHeader = Read-Source 'source\common\engine\perf_capture.h'
$perfOwner = Read-Source 'source\common\engine\perf_capture.cpp'
$gridHeader = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_grid.h'
$gridOwner = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_grid.cpp'
$worldOwner = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_grid_lighting.cpp'
$smokeOwner = Read-Source 'source\common\rendering\nri\renderer\nri_smoke.cpp'

Assert-Match $timingHeader 'QueryCapacity\s*=\s*194[\s\S]*ScopeCapacity\s*=\s*96' 'Smoke timing capacity must cover independent analytic build/apply scopes plus base and voxel timing.'
Assert-Match $gridHeader 'NRIRenderDevice\* gpuTimingDevice' 'Focused grid owners must use the existing render-device timing service.'
Assert-Match $gridHeader 'uint64_t rendererFrame' 'Smoke readbacks must retain the render-device frame used by GPU timing.'
Assert-Match $timingOwner 'CmdCopyQueries' 'Generic timing must retain asynchronous GPU query readback.'
if ($timingOwner -match '(?m)\b(Wait|WaitForFenceValue|WaitForCommands|DeviceWaitIdle|QueueWaitIdle)\s*\(') {
    throw 'Generic timing must not add a CPU wait.'
}

$scopes = @(
    @{ Enum = 'SmokeGridAllocate'; Field = 'smokeGridAllocateMs'; Key = 'smoke_grid_allocate' },
    @{ Enum = 'SmokeGridInitialize'; Field = 'smokeGridInitializeMs'; Key = 'smoke_grid_initialize' },
    @{ Enum = 'SmokeGridDeposit'; Field = 'smokeGridDepositMs'; Key = 'smoke_grid_deposit' },
    @{ Enum = 'SmokeGridHalo'; Field = 'smokeGridHaloMs'; Key = 'smoke_grid_halo' },
    @{ Enum = 'SmokeGridSimulate'; Field = 'smokeGridSimulateMs'; Key = 'smoke_grid_simulate' },
    @{ Enum = 'SmokeGridRebuild'; Field = 'smokeGridRebuildMs'; Key = 'smoke_grid_rebuild' },
    @{ Enum = 'SmokeDormantArchive'; Field = 'smokeDormantArchiveMs'; Key = 'smoke_dormant_archive' },
    @{ Enum = 'SmokeDormantPromote'; Field = 'smokeDormantPromoteMs'; Key = 'smoke_dormant_promote' },
    @{ Enum = 'SmokeDormantEvolve'; Field = 'smokeDormantEvolveMs'; Key = 'smoke_dormant_evolve' },
    @{ Enum = 'SmokeWorldActive'; Field = 'smokeWorldActiveMs'; Key = 'smoke_world_active' },
    @{ Enum = 'SmokeWorldLink'; Field = 'smokeWorldLinkMs'; Key = 'smoke_world_link' },
    @{ Enum = 'SmokeWorldProposal'; Field = 'smokeWorldProposalMs'; Key = 'smoke_world_proposal' },
    @{ Enum = 'SmokeWorldSeed'; Field = 'smokeWorldSeedMs'; Key = 'smoke_world_seed' },
    @{ Enum = 'SmokeWorldTemporal'; Field = 'smokeWorldTemporalMs'; Key = 'smoke_world_temporal' },
    @{ Enum = 'SmokeWorldFilter'; Field = 'smokeWorldFilterMs'; Key = 'smoke_world_filter' },
    @{ Enum = 'SmokeWorldScatter'; Field = 'smokeWorldScatterMs'; Key = 'smoke_world_scatter' },
    @{ Enum = 'SmokeCarrier'; Field = 'smokeCarrierMs'; Key = 'smoke_carrier' },
    @{ Enum = 'SmokeViewPrepare'; Field = 'smokeViewPrepareMs'; Key = 'smoke_view_prepare' },
    @{ Enum = 'SmokeMaterialize'; Field = 'smokeMaterializeMs'; Key = 'smoke_materialize' },
    @{ Enum = 'SmokeViewPoint'; Field = 'smokeViewPointMs'; Key = 'smoke_view_point' },
    @{ Enum = 'SmokeViewDirectional'; Field = 'smokeViewDirectionalMs'; Key = 'smoke_view_directional' },
    @{ Enum = 'SmokeViewDirectReuse'; Field = 'smokeViewDirectReuseMs'; Key = 'smoke_view_direct_reuse' },
    @{ Enum = 'SmokeViewEmissive'; Field = 'smokeViewEmissiveMs'; Key = 'smoke_view_emissive' },
    @{ Enum = 'SmokeAnalyticEmissiveBuild'; Field = 'smokeAnalyticEmissiveBuildMs'; Key = 'smoke_analytic_emissive_build' },
    @{ Enum = 'SmokeAnalyticEmissiveApply'; Field = 'smokeAnalyticEmissiveApplyMs'; Key = 'smoke_analytic_emissive_apply' },
    @{ Enum = 'SmokeViewIndirect'; Field = 'smokeViewIndirectMs'; Key = 'smoke_view_indirect' },
    @{ Enum = 'SmokeIntegrate'; Field = 'smokeIntegrateMs'; Key = 'smoke_integrate' },
    @{ Enum = 'SmokeReconstruction'; Field = 'smokeReconstructionMs'; Key = 'smoke_reconstruction' }
)

foreach ($scope in $scopes) {
    Assert-Match $timingHeader ([regex]::Escape($scope.Enum)) "Missing generic timing enum $($scope.Enum)."
    Assert-Match $timingOwner ("$([regex]::Escape($scope.Enum)):\s+timing\.$([regex]::Escape($scope.Field))\s*\+=\s*value") "Missing timestamp resolution for $($scope.Enum)."
    Assert-Match $perfHeader ([regex]::Escape($scope.Field)) "Missing compact timing field $($scope.Field)."
    Assert-Match $perfOwner ("$([regex]::Escape($scope.Key))=%\.3f") "Missing compact timing log key $($scope.Key)."
    Assert-Match $perfOwner ("gpu\.$([regex]::Escape($scope.Field))\s*\+=\s*timing\.$([regex]::Escape($scope.Field))") "Missing compact aggregation for $($scope.Field)."
}

Assert-Match $gridOwner 'SmokeGridAllocate[\s\S]*AllocateCommands[\s\S]*SmokeGridInitialize[\s\S]*BuildDispatch[\s\S]*PrepareBricks' 'Grid allocation/preparation timing is misplaced.'
Assert-Match $gridOwner 'SmokeGridDeposit[\s\S]*NRISmokeGridPass::Deposit[\s\S]*ResolveDeposit' 'Grid deposition timing is misplaced.'
Assert-Match $gridOwner 'SmokeGridHalo[\s\S]*AllocateHalo[\s\S]*SmokeGridSimulate[\s\S]*AdvectVelocity[\s\S]*AdvectFields[\s\S]*SmokeGridRebuild[\s\S]*NRISmokeGridPass::Rebuild' 'Halo, simulation, and rebuild timing must remain exclusive and ordered.'
Assert-Match $worldOwner 'SmokeWorldActive[\s\S]*Prepare[\s\S]*BuildActive[\s\S]*SmokeWorldLink[\s\S]*BuildLinks[\s\S]*SmokeWorldProposal[\s\S]*BuildProposals[\s\S]*SmokeWorldSeed[\s\S]*Seed[\s\S]*SmokeWorldTemporal[\s\S]*Temporal' 'World-light timing scopes are incomplete or out of order.'
Assert-Match $smokeOwner 'SmokeViewPrepare[\s\S]*SmokeMaterialize[\s\S]*EvaluateGrid[\s\S]*SmokeViewPoint[\s\S]*LightPoint[\s\S]*SmokeViewDirectional[\s\S]*LightDirectional[\s\S]*SmokeViewDirectReuse[\s\S]*SmokeIntegrate[\s\S]*Integrate[\s\S]*SmokeReconstruction[\s\S]*ResolveVolume[\s\S]*TemporalVolume[\s\S]*Composite' 'View smoke timing scopes are incomplete or out of order.'
Assert-Match $smokeOwner 'PERF pt smoke work NRI:[\s\S]*observe_renderer_frame=%llu[\s\S]*joined=%u[\s\S]*grid_renderer_frame=%llu[\s\S]*grid_deposition_cells_total=[\s\S]*grid_deposition_cells_delta=[\s\S]*world_renderer_frame=%llu[\s\S]*world_link_rays=[\s\S]*view_renderer_frame=%llu[\s\S]*view_direct_receiver_samples=' 'Step 0 compact work telemetry is incomplete.'
Assert-Match $smokeOwner 'PerfCompactCaptureTimingActive\(\) \|\| PerfCompactCaptureReadbackDrainActive\(\)' 'Smoke workload rows must drain through the final queued GPU timing identities.'

Write-Host 'Smoke performance Step 0 contract validation passed.'
