$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Repo([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

$cvars = Read-Repo 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$settings = Read-Repo 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$smoke = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$dense = Read-Repo 'source/common/rendering/nri/shaders/SmokeEvaluateGrid.cs.hlsl'
$compare = Read-Repo 'source/common/rendering/nri/shaders/SmokeViewWorkCompareDense.cs.hlsl'
$control = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke_view_work_contracts.h'
$owner = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke_view_work.cpp'
$timing = Read-Repo 'source/common/rendering/nri/system/nri_gpu_timing.cpp'

Assert-Match $cvars 'CVAR\(Bool,\s*nri_ptsmokeviewcompare,\s*false,\s*0\)' 'View-mask comparison must be opt-in, default-off, and session-only.'
Assert-Match $settings 'settings\.viewCompare\s*=\s*\(bool\)nri_ptsmokeviewcompare' 'The immutable frame settings must capture the diagnostic CVar.'
Assert-Match $smoke 'renderGrid\s*&&\s*\(mSettings\.viewCompare\s*\|\|\s*mSettings\.viewRoute\s*!=\s*0u\)' 'Mask preparation must run only for grid rendering under an explicit diagnostic/static route.'
Assert-Match $smoke 'SmokeMaterialize[\s\S]*EvaluateGrid[\s\S]*CompareDense' 'Dense evaluation must remain the authority and execute before comparison.'
Assert-Match $smoke 'SmokeViewPrepare[\s\S]*mViewWork\.Prepare' 'View preparation must have an explicit timing scope separate from dense evaluation.'
Assert-Match $timing 'view_prepare=%\.6f[\s\S]*evaluate_grid=%\.6f' 'Telemetry must expose view preparation separately from dense grid evaluation.'
Assert-NotMatch $dense 'CompactIndices|DispatchIndirect' 'The production evaluation shader must not depend on compaction or indirect work.'

Assert-Match $compare 'gViewDenseMedium\[dispatchThreadId\]' 'Comparator must read the dense reference medium.'
Assert-Match $compare 'gViewDenseSource\[dispatchThreadId\]' 'Comparator must read the dense reference radiance source.'
Assert-Match $compare 'FalseNegatives' 'Comparator must report false negatives.'
Assert-Match $compare 'FalsePositives' 'Comparator must report false positives.'
Assert-Match $compare 'TauErrorBits' 'Comparator must report maximum optical-depth error.'
Assert-Match $compare 'OpacityErrorBits' 'Comparator must report maximum opacity error.'
Assert-Match $compare 'RadianceErrorBits' 'Comparator must report maximum radiance error.'
Assert-Match $compare 'BoundaryFalseNegatives' 'Comparator must classify boundary false negatives.'
Assert-NotMatch $compare 'gSmokeFroxelMedium\s*\[[^]]+\]\s*=|gSmokeFroxelSource\s*\[[^]]+\]\s*=' 'Comparator must never mutate production froxel outputs.'

foreach ($field in @('false_negatives', 'false_positives', 'tau_error_max', 'opacity_error_max', 'radiance_error_max', 'boundary_false_negatives', 'overflow')) {
    Assert-Match $owner ([regex]::Escape($field + '=')) "Status output must report $field."
}
Assert-Match $owner 'authority=smoke-evaluate-grid\s+comparator_output_mutation=no' 'Status must state production output authority and diagnostic non-mutation.'
Assert-Match $control 'uint32_t\s+overflow' 'Comparator contract must retain an explicit overflow counter.'
Assert-NotMatch $smoke 'CmdDispatchIndirect|opaque.*depth|available.*headroom|gpu.*budget' 'Slice 3A must not add indirect execution, opaque-depth culling, or adaptive budgeting.'

Write-Host 'Smoke view conservative-mask comparator contract passed.'
