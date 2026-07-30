$ErrorActionPreference = 'Stop'

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message expected=$Expected actual=$Actual"
    }
}

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$header = Get-Content -Raw (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_emitters.h')
$source = Get-Content -Raw (Join-Path $root 'source/common/rendering/nri/renderer/nri_smoke_emitters.cpp')
$overlaySource = Get-Content -Raw (Join-Path $root 'source/core/lightoverlay.cpp')

Assert-Match $header 'struct MapEmitterState[\s\S]*previousTimeSeconds[\s\S]*logicalElapsedSeconds[\s\S]*intervalRemainder[\s\S]*nextCadenceOrdinal[\s\S]*coalescedDebt[\s\S]*previousTier[\s\S]*initialized' 'Map emitters need persistent logical cadence and dormant-debt state.'
Assert-Match $header 'FString mActiveMapName[\s\S]*mMapEmitterStates' 'Map emitter state must be scoped to the active resolved map.'
Assert-Match $source 'mGeneration != resolved\.resolvedGeneration \|\| mActiveMapName\.CompareNoCase\(resolved\.activeMapName\) != 0[\s\S]*mMapEmitterStates\.clear\(\)' 'Resolved generation or map changes must reset placed-emitter cadence.'
Assert-Match $source 'resolved\.mapSmokeEmitterRules[\s\S]*resolved\.currentMapAvailable[\s\S]*rule\.styleResolved[\s\S]*rule\.mapName\.CompareNoCase\(resolved\.activeMapName\)' 'Only resolved emitters belonging to the active map may schedule commands.'
Assert-Match $source 'NRIMakeSmokeSourceId\("map"[\s\S]*interest\.Resolve\(sourceId\)[\s\S]*mMapEmitterStates\[sourceId\]' 'Map emitter interest and cadence state must share a stable source identity rather than rule ordering.'
Assert-Match $source '!state\.initialized[\s\S]*std::floor\(nonNegativeTime / intervalSeconds\)[\s\S]*state\.nextCadenceOrdinal = firstCadenceOrdinal \+ 1u[\s\S]*candidateCount = 1u' 'A newly observed map source must derive its phase from gameplay time and contribute one current cadence candidate.'
Assert-Match $source 'tier == NRISmokeInterestTier::Dormant[\s\S]*MaximumCoalescedDebt[\s\S]*stats\.dormant = candidateCount[\s\S]*return stats' 'Dormant emitters must advance logical cadence without submitting injection commands, with bounded debt telemetry.'
Assert-Match $source 'promoting[\s\S]*state\.coalescedDebt = 0u[\s\S]*emitCount == 0u[\s\S]*emitCount = 1u' 'Promotion must collapse missed history into at most one current prewarm pulse when no cadence crossing is due.'
Assert-Match $source 'emitCount = std::min\(candidateCount, rule\.maxSegmentsPerFrame\)[\s\S]*candidateCount - emitCount \+ emissionIndex' 'Active cadence must retain only the newest bounded crossings.'
Assert-Match $source 'NRIMakeSmokeSourceId\("map-pulse"[\s\S]*cadenceOrdinal' 'Map pulse randomness must derive from stable cadence identity rather than frame submission order.'

Assert-Match $overlaySource 'BuildLightOverlayMapSmokeEmitterRectangle[\s\S]*reference \^ normal[\s\S]*normal \^ baseU[\s\S]*halfAxisU[\s\S]*halfAxisV[\s\S]*center \+= normal \* rule\.offset' 'Shared rectangle geometry must form a rotated orthonormal basis, encode half extents, and apply normal offset.'
Assert-Match $source 'BuildLightOverlayMapSmokeEmitterRectangle\(rule, rectangle\)[\s\S]*normal \* rule\.velocityScale' 'Runtime injection must consume the shared rectangle geometry and launch along its authored normal.'
Assert-Match $source 'WorldToPathTracingDirection\(axisU, command\.halfAxisU\)[\s\S]*WorldToPathTracingDirection\(axisV, command\.halfAxisV\)[\s\S]*command\.shape = static_cast<uint32_t>\(NRISmokeInjectionShape::Rectangle\)' 'Rectangle half axes must be converted to render space and tagged as rectangle shape.'
Assert-Match $source 'SetPointSourceShape\(command\)[\s\S]*for \(const PathTracingWeaponLightEvent& event[\s\S]*SetPointSourceShape\(command\)' 'Actor and event commands must explicitly retain point-source shape and zero axes.'
Assert-Match $source 'ConservativeMapEmitterBrickFootprint[\s\S]*SmokeGridFloorDiv8[\s\S]*footprint_bricks=%u[\s\S]*grid_capacity=%u[\s\S]*impossible=%u' 'Trace mode must expose a signed-coordinate-safe conservative footprint and capacity impossibility signal.'
Assert-Match $source 'event=%s map=%s rule=%s[\s\S]*cadence_ordinal=%llu[\s\S]*shape=rectangle[\s\S]*event=map-frame-summary[\s\S]*active=%u tier=%u emitted=%u particles=%u skipped=%u dormant=%u coalesced=%u debt=%u' 'Trace mode must expose stable pulse cadence and dormant/coalesced summaries.'
Assert-Match $source 'GetMapSmokeEmitterEditorRuntimePreview[\s\S]*suppressPersistedRule[\s\S]*event=map-preview-frame-summary' 'Staged editor rules must preview through the runtime emitter while suppressing the edited persisted source.'

# CPU mirror: a dormant source advances its logical cadence but emits no work.
# Debt is diagnostic and bounded, then promotion clears it and emits at most one
# current representative when no crossing is otherwise due.
$interval = 0.25
$maximum = 2
$remainder = 0.0
$previous = 0.0
$debt = 1
$dormantCommands = 0
$now = 1100.0
$total = $remainder + [Math]::Max(0.0, $now - $previous)
$candidates = [Math]::Floor($total / $interval)
$remainder = $total % $interval
$debt = [Math]::Min(4096, $debt + $candidates)
if ($dormantCommands -ne 0 -or $debt -ne 4096) { throw 'Dormant logical-clock debt was not bounded without command submission.' }
$previous = $now
$promotedCommands = 1
$debt = 0
if ($promotedCommands -gt $maximum -or $debt -ne 0) { throw 'Promotion replayed dormant history instead of coalescing it.' }

# The next ordinary hitch still keeps only the newest bounded crossings and
# retains only the fractional cadence remainder.
$now = 1101.10
$total = $remainder + [Math]::Max(0.0, $now - $previous)
$candidates = [Math]::Floor($total / $interval)
$remainder = $total % $interval
$emitted = [Math]::Min($candidates, $maximum)
$skipped = $candidates - $emitted
if ($candidates -ne 4 -or $emitted -ne 2 -or $skipped -ne 2) { throw 'Newest-crossing cap mirror failed.' }
$previous = $now
$now = 1101.25
$total = $remainder + [Math]::Max(0.0, $now - $previous)
$candidates = [Math]::Floor(($total + 1e-9) / $interval)
if ($candidates -ne 1) { throw 'Discarded cadence crossings leaked into a later frame or fractional remainder was lost.' }

# Cadence identity comes from absolute gameplay time, so frame chunking cannot
# change the ordinal selected for a given scheduled pulse.
$directOrdinal = [Math]::Floor(42.75 / $interval)
$chunkedOrdinal = [Math]::Floor((40.0 + 1.0 + 1.0 + 0.75) / $interval)
if ($directOrdinal -ne $chunkedOrdinal) { throw 'Cadence identity depends on frame chunking.' }

# CPU mirror for normal (0,0,-1), rotation 90 degrees, size 8x4,
# position (10,20,30), offset 2, and speed 5. Expected Build-space half
# axes are (0,4,0) and (2,0,0), then converted with (x,-z,-y).
$center = @(10.0, 20.0, 28.0)
$velocity = @(0.0, 0.0, -5.0)
$axisU = @(0.0, 4.0, 0.0)
$axisV = @(2.0, 0.0, 0.0)
$renderCenter = @($center[0], -$center[2], -$center[1])
$renderVelocity = @($velocity[0], -$velocity[2], -$velocity[1])
$renderU = @($axisU[0], -$axisU[2], -$axisU[1])
$renderV = @($axisV[0], -$axisV[2], -$axisV[1])
Assert-Near $renderCenter[0] 10.0 1e-6 'Rectangle center X conversion failed.'
Assert-Near $renderCenter[1] -28.0 1e-6 'Rectangle center Y conversion failed.'
Assert-Near $renderCenter[2] -20.0 1e-6 'Rectangle center Z conversion failed.'
Assert-Near $renderVelocity[1] 5.0 1e-6 'Normal velocity conversion failed.'
Assert-Near $renderU[2] -4.0 1e-6 'Rotated U half extent conversion failed.'
Assert-Near $renderV[0] 2.0 1e-6 'Rotated V half extent conversion failed.'

Write-Host 'smoke-map-emitter-runtime.tests.ps1: PASS'
