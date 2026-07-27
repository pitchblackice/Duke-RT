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
$resources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridResources.hlsli'
$hashHealth = Read-Source 'source/common/rendering/nri/shaders/SmokeGridBuildDispatch.cs.hlsl'
$halo = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAllocateHalo.cs.hlsl'
$evaluate = Read-Source 'source/common/rendering/nri/shaders/SmokeEvaluateGrid.cs.hlsl'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.cpp'

$cpuFields = @(
    'hashEmpty', 'hashClaimed', 'hashResident', 'hashNew', 'hashTombstone',
    'hashInvalidState', 'hashInvalidMapping', 'controlProbeTotal', 'controlProbeBin1',
    'controlProbeBin2To4', 'controlProbeBin5To8', 'controlProbeBin9To16',
    'controlProbeBin17To24', 'lookupProbeTotal', 'insertionProbeTotal',
    'lookupProbeLimitFailures', 'insertionProbeLimitFailures', 'insertionCapacityFailures',
    'insertionActiveFailures', 'reclaimInvalidMappingFailures', 'hashRebuildAttempts',
    'hashRebuildSuccesses', 'hashRebuildFailures'
)
$gpuFields = @(
    'HashEmpty', 'HashClaimed', 'HashResident', 'HashNew', 'HashTombstone',
    'HashInvalidState', 'HashInvalidMapping', 'ControlProbeTotal', 'ControlProbeBin1',
    'ControlProbeBin2To4', 'ControlProbeBin5To8', 'ControlProbeBin9To16',
    'ControlProbeBin17To24', 'LookupProbeTotal', 'InsertionProbeTotal',
    'LookupProbeLimitFailures', 'InsertionProbeLimitFailures', 'InsertionCapacityFailures',
    'InsertionActiveFailures', 'ReclaimInvalidMappingFailures', 'HashRebuildAttempts',
    'HashRebuildSuccesses', 'HashRebuildFailures'
)
for ($index = 0; $index -lt $cpuFields.Count; $index++) {
    Assert-Match $contracts ("uint32_t\s+{0}\s*=" -f $cpuFields[$index]) ("Missing CPU hash-health field {0}." -f $cpuFields[$index])
    Assert-Match $data ("uint\s+{0}\s*;" -f $gpuFields[$index]) ("Missing HLSL hash-health field {0}." -f $gpuFields[$index])
}
Assert-Match $contracts 'sizeof\(NRISmokeGridControlGpu\)\s*==\s*304' 'The mirrored grid control ABI size must be pinned.'
Assert-Match $hashHealth 'for\s*\(uint slot = 0u; slot < gSmokeGridConstants.HashCapacity; \+\+slot\)' 'Hash gauges must scan every physical hash slot.'
Assert-Match $hashHealth 'gSmokeGridConstants\.Flags\s*&\s*NRI_SMOKE_GRID_FLAG_HASH_HEALTH' 'Exact hash scanning must run only for the final diagnostic dispatch.'
Assert-Match $hashHealth 'HashEmpty\+\+[\s\S]*HashClaimed\+\+[\s\S]*HashResident\+\+[\s\S]*HashNew\+\+[\s\S]*HashTombstone\+\+' 'All valid hash states must have exact gauges.'
Assert-Match $hashHealth 'brick\.HashSlot\s*==\s*slot[\s\S]*brick\.Generation\s*==\s*entry\.Generation[\s\S]*brick\.State\s*==\s*entry\.State' 'Hash health must validate the reverse brick mapping and generation.'
Assert-Match $resources 'SmokeGridRecordControlProbe[\s\S]*ControlProbeBin1[\s\S]*ControlProbeBin2To4[\s\S]*ControlProbeBin5To8[\s\S]*ControlProbeBin9To16[\s\S]*ControlProbeBin17To24' 'Control-plane probes must be assigned to the planned bounded bins.'
Assert-Match $resources 'LookupProbeLimitFailures\+\+' 'Lookup probe-limit failures must be separate.'
Assert-Match $resources 'InsertionProbeLimitFailures\+\+' 'Insertion probe-limit failures must be separate.'
Assert-Match $halo 'SmokeGridLookupBrickControlSerial' 'Halo allocation lookups must contribute to control-plane probe telemetry.'
Assert-NotMatch $evaluate 'ControlProbe|ProbeBin|ProbeTotal' 'Render sampling must not add per-sample telemetry atomics.'
Assert-Match $owner 'delta\.controlProbeTotal[\s\S]*delta\.hashRebuildFailures' 'Cumulative hash-health counters must publish frame deltas.'
Assert-Match $owner 'frame\.simulationSubsteps\s*>\s*0u\s*\|\|\s*frame\.hashHealthDiagnostic' 'The final dispatch must run only for simulation publication or explicit hash diagnostics.'
Assert-Match $owner 'constants\.flags\s*=\s*frame\.hashHealthDiagnostic\s*\?\s*NRI_SMOKE_GRID_FLAG_HASH_HEALTH\s*:\s*0u' 'The owner must request exact scanning only for explicit diagnostics.'
Assert-Match $runtime 'PERF pt smoke hash health NRI:' 'Compact capture must include the hash-health row.'
Assert-Match $runtime 'hash_empty=%u[\s\S]*hash_tombstone=%u[\s\S]*control_probe_delta=%u[\s\S]*hash_rebuild_failures_delta=%u' 'Compact hash-health output must include gauges, probe bins, reasons, and rebuild events.'

Write-Host 'Smoke Slice 2B hash-health telemetry contracts passed.'
