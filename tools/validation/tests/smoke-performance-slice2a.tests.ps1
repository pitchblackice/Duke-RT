$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_contracts.h'
$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridData.hlsli'
$allocate = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAllocateCommands.cs.hlsl'
$rebuild = Read-Source 'source/common/rendering/nri/shaders/SmokeGridRebuild.cs.hlsl'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'

Assert-Match $contracts 'admissionFootprintCulled' 'CPU grid control must expose exact footprint culls.'
Assert-Match $contracts 'footprintCulled' 'CPU per-source telemetry must expose footprint culls.'
Assert-Match $data 'AdmissionFootprintCulled' 'HLSL grid control must mirror footprint culls.'
Assert-Match $data 'FootprintCulled' 'HLSL per-source telemetry must mirror footprint culls.'
Assert-Match $allocate 'SmokeGridCommandMayIntersectBrick[\s\S]*SmokeGridSupportAxisSeparates' 'Allocation must conservatively test thick source support against each candidate brick.'
Assert-Match $allocate 'SmokeGridCellCenter[\s\S]*SmokeInjectionClosestRectanglePoint[\s\S]*dot\(offset,\s*offset\)\s*<\s*radius\s*\*\s*radius' 'Allocation must mirror the exact deposition cell-center support test.'
Assert-Match $allocate 'cross\(halfAxisU,\s*halfAxisV\)[\s\S]*cross\(halfAxisU,\s*worldX\)[\s\S]*cross\(halfAxisV,\s*worldZ\)' 'Thick rectangles must use conservative separating axes, including skewed authored bases.'
Assert-Match $allocate '!SmokeGridCommandMayIntersectBrick[\s\S]*FootprintCulled\+\+[\s\S]*AdmissionFootprintCulled\+\+' 'Culled AABB corners must be attributed before admission.'
Assert-Match $runtime 'footprint_culled=%u' 'Compact and status telemetry must publish footprint culls.'

$occupancyStart = $rebuild.IndexOf('gSmokeGridOccupied[localIndex]')
$occupancyEnd = $rebuild.IndexOf('SmokeGridCellFieldHash', $occupancyStart)
if ($occupancyStart -lt 0 -or $occupancyEnd -le $occupancyStart) {
    throw 'Could not isolate the optical occupancy predicate.'
}
$occupancy = $rebuild.Substring($occupancyStart, $occupancyEnd - $occupancyStart)
Assert-Match $occupancy 'scalar\.z' 'Extinction must retain visible topology.'
Assert-Match $occupancy 'optical\.x[\s\S]*optical\.y[\s\S]*optical\.z' 'RGB scattering must retain visible topology.'
Assert-NotMatch $occupancy 'scalar\.x' 'Transport-only mass must not retain visually empty topology.'

Write-Host 'Smoke Slice 2A footprint and optical lifecycle contracts passed.'
