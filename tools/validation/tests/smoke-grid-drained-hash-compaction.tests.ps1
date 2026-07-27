$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) { throw "$Message Expected=$Expected Actual=$Actual" }
}

$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_contracts.h'
$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridData.hlsli'
$compact = Read-Source 'source/common/rendering/nri/shaders/SmokeGridBuildDispatch.cs.hlsl'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.cpp'

Assert-Match $contracts 'NRI_SMOKE_GRID_FLAG_HASH_HEALTH\s*=\s*1u' 'CPU hash-health flag changed unexpectedly.'
Assert-Match $contracts 'NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH\s*=\s*2u' 'CPU drained-compaction flag is missing.'
Assert-Match $data 'NRI_SMOKE_GRID_FLAG_HASH_HEALTH\s+1u' 'HLSL hash-health flag changed unexpectedly.'
Assert-Match $data 'NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH\s+2u' 'HLSL drained-compaction flag is missing.'
Assert-Match $owner 'frame\.simulationSubsteps\s*>\s*0u[\s\S]*constants\.flags\s*\|=\s*NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH' 'Only a post-rebuild publication may authorize compaction.'
Assert-Match $compact 'ResidentCount\s*!=\s*0u[\s\S]*return' 'Compaction must reject live resident topology.'
Assert-Match $compact 'activeCount\s*==\s*0u[\s\S]*FreeCount\s*==\s*gSmokeGridConstants\.BrickCapacity' 'Compaction must verify drained active and free-list accounting.'
Assert-Match $compact 'state\s*==\s*NRI_SMOKE_GRID_EMPTY\s*\|\|\s*state\s*==\s*NRI_SMOKE_GRID_TOMBSTONE' 'Compaction must reject CLAIMED, NEW, RESIDENT, and invalid states.'
Assert-Match $compact 'HashRebuildAttempts\+\+[\s\S]*!safeToReset[\s\S]*HashRebuildFailures\+\+[\s\S]*HashRebuildSuccesses\+\+' 'Attempt, failure, and success counters must describe the exact outcome.'
Assert-Match $compact 'SmokeGridHashEntry entry = \(SmokeGridHashEntry\)0;[\s\S]*entry\.BrickIndex = 0xffffffffu;[\s\S]*gSmokeGridHash\[slot\] = entry' 'Successful compaction must restore canonical EMPTY entries.'
Assert-Match $compact 'SmokeGridCompactDrainedHash\(\);[\s\S]*NRI_SMOKE_GRID_FLAG_HASH_HEALTH' 'Compaction must finish before optional exact gauges are scanned.'

function Invoke-Compaction([uint32[]]$States, [uint32]$ResidentCount, [uint32]$ActiveCount, [uint32]$FreeCount, [uint32]$BrickCapacity) {
    $result = [ordered]@{ States = [uint32[]]$States.Clone(); Attempts = 0; Successes = 0; Failures = 0 }
    if ($ResidentCount -ne 0) { return $result }
    $hasNonEmpty = $false
    $safe = $ActiveCount -eq 0 -and $FreeCount -eq $BrickCapacity
    foreach ($state in $result.States) {
        $hasNonEmpty = $hasNonEmpty -or $state -ne 0
        $safe = $safe -and ($state -eq 0 -or $state -eq 4)
    }
    if (-not $hasNonEmpty) { return $result }
    $result.Attempts++
    if (-not $safe) { $result.Failures++; return $result }
    for ($i = 0; $i -lt $result.States.Count; $i++) { $result.States[$i] = 0 }
    $result.Successes++
    return $result
}

$drained = Invoke-Compaction -States @(0, 4, 0, 4) -ResidentCount 0 -ActiveCount 0 -FreeCount 2 -BrickCapacity 2
Assert-Equal $drained.Attempts 1 'A drained tombstone topology must attempt once.'
Assert-Equal $drained.Successes 1 'A drained tombstone topology must succeed.'
Assert-Equal (($drained.States | Measure-Object -Sum).Sum) 0 'A successful reset must leave every slot EMPTY.'

foreach ($unsafeState in @(1, 2, 3, 5)) {
    $unsafe = Invoke-Compaction -States @(4, $unsafeState) -ResidentCount 0 -ActiveCount 0 -FreeCount 2 -BrickCapacity 2
    Assert-Equal $unsafe.Attempts 1 "Unsafe state $unsafeState must publish an attempted reset."
    Assert-Equal $unsafe.Failures 1 "Unsafe state $unsafeState must fail closed."
    Assert-Equal $unsafe.States[0] 4 "Unsafe state $unsafeState must not mutate tombstones."
    Assert-Equal $unsafe.States[1] $unsafeState "Unsafe state $unsafeState must not mutate its mapping."
}

$live = Invoke-Compaction -States @(4, 2) -ResidentCount 1 -ActiveCount 1 -FreeCount 1 -BrickCapacity 2
Assert-Equal $live.Attempts 0 'Live resident topology must not attempt compaction.'
Assert-Equal $live.States[1] 2 'Live resident topology must remain untouched.'

$empty = Invoke-Compaction -States @(0, 0) -ResidentCount 0 -ActiveCount 0 -FreeCount 2 -BrickCapacity 2
Assert-Equal $empty.Attempts 0 'An already canonical EMPTY topology must not publish a redundant event.'

Write-Host 'Smoke drained hash-compaction contracts passed.'
