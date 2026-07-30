$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -notmatch $Pattern) { throw $Message }
}

$cvars = Read-Source 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$settings = Read-Source 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_lighting.cpp'
$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid_lighting_contracts.h'
$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridLightingData.hlsli'
$prepare = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightPrepare.cs.hlsl'
$seed = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightSeed.cs.hlsl'
$temporal = Read-Source 'source/common/rendering/nri/shaders/SmokeGridLightTemporal.cs.hlsl'

Assert-Match $cvars 'nri_ptsmokeworldpartitions,\s*1,\s*0' 'Full refresh must remain the default and the partition control must remain session-only.'
Assert-Match $settings 'worldRadiancePartitions\s*=\s*requestedWorldPartitions\s*>=\s*4u\s*\?\s*4u\s*:\s*\(requestedWorldPartitions\s*>=\s*2u\s*\?\s*2u\s*:\s*1u\)' 'Only stable full, two-way, or four-way modes may reach the owner.'
Assert-Match $owner 'constants\.particleCapacity\s*=\s*settings\.worldRadiancePartitions[\s\S]*constants\.styleCount\s*=\s*radianceWorkLimited\s*\?\s*workTable\.radianceNewInvalidCells\s*:\s*settings\.worldRadianceNewCells[\s\S]*constants\.froxelWidth\s*=\s*radianceWorkLimited\s*\?\s*workTable\.radianceMaintenanceCells\s*:\s*settings\.worldRadianceMaintenanceCells[\s\S]*constants\.froxelHeight\s*=\s*settings\.worldRadianceMaximumAge' 'The owner must publish the selected immutable radiance work table per record.'
Assert-Match $contracts 'sizeof\(NRISmokeGridLightControlGpu\)\s*==\s*392' 'CPU control layout must include the complete partition telemetry.'
foreach ($field in @('RadiancePartitionCount','RadianceNewInvalidQuantity','RadianceMaintenanceQuantity','RadianceMaximumAge','RadianceNewInvalidRequested','RadianceNewInvalidScheduled','RadianceNewInvalidDeferred','RadianceMaintenanceRequested','RadianceMaintenanceScheduled','RadianceMaintenanceDeferred','RadianceHistoryRetained','RadianceHistoryMissing','RadianceAgeOverflows','RadianceNewInvalidTickets','RadianceMaintenanceTickets')) {
	Assert-Match $data ("uint\s+" + $field + "\s*;") "HLSL control is missing $field."
}
Assert-Match $prepare 'RadiancePartitionCount\s*=\s*max\(gSmokeConstants\.ParticleCapacity,\s*1u\)[\s\S]*RadianceNewInvalidQuantity[\s\S]*RadianceMaintenanceQuantity[\s\S]*RadianceMaximumAge' 'Prepare must freeze the resolved table before scheduling.'
Assert-Match $seed 'SmokeGridLightStableWorldKey[\s\S]*FrameIndex\s*%\s*partitionCount' 'Maintenance selection must rotate by stable world key.'
Assert-Match $seed 'NRI_SMOKE_GRID_LIGHT_WORK_LIMITED[\s\S]*partitionCount\s*<=\s*1u\s*&&\s*!workLimited[\s\S]*return true' 'Reference must remain unrestricted while static profiles activate radiance tickets.'
Assert-Match $seed 'RadianceNewInvalidRequested[\s\S]*SmokeGridLightClaimNewInvalid[\s\S]*RadianceNewInvalidDeferred' 'New and invalid radiance must own a bounded reserved lane.'
Assert-Match $seed 'RadianceMaintenanceRequested[\s\S]*SmokeGridLightClaimMaintenance[\s\S]*RadianceMaintenanceDeferred' 'Maintenance radiance must own an independently bounded lane.'
Assert-Match $seed 'if\s*\(!historyValid\)[\s\S]*RadianceNewInvalidRequested[\s\S]*scheduled\s*=\s*SmokeGridLightClaimNewInvalid\(\)' 'New and invalid work must claim its reserved lane independently of the maintenance partition.'
if ($seed -match 'scheduled\s*=\s*partitionDue\s*&&\s*SmokeGridLightClaimNewInvalid\(\)') {
	throw 'Stable maintenance partitioning must not filter new and invalid work.'
}
Assert-Match $seed 'InterlockedAdd\(gSmokeGridLightControl\[0\]\.RadianceNewInvalidTickets,\s*1u,\s*ticket\)[\s\S]*ticket\s*>=\s*gSmokeGridLightControl\[0\]\.RadianceNewInvalidQuantity[\s\S]*InterlockedAdd\(gSmokeGridLightControl\[0\]\.RadianceNewInvalidScheduled,\s*1u\)' 'New work must use one ticket atomic and count only accepted execution.'
Assert-Match $seed 'InterlockedAdd\(gSmokeGridLightControl\[0\]\.RadianceMaintenanceTickets,\s*1u,\s*ticket\)[\s\S]*ticket\s*>=\s*gSmokeGridLightControl\[0\]\.RadianceMaintenanceQuantity[\s\S]*InterlockedAdd\(gSmokeGridLightControl\[0\]\.RadianceMaintenanceScheduled,\s*1u\)' 'Maintenance must use one ticket atomic and count only accepted execution.'
if ($seed -match 'InterlockedCompareExchange\(gSmokeGridLightControl\[0\]\.Radiance(?:NewInvalid|Maintenance)Scheduled') {
	throw 'Contended compare-exchange scheduling loops must not return.'
}
Assert-Match $seed 'SmokeGridLightSetMetadata\(retained[\s\S]*SmokeGridLightLastUpdate\(prior\)[\s\S]*retainedAge[\s\S]*RadianceHistoryRetained' 'Unscheduled compatible work must retain explicit aged history.'
Assert-Match $temporal 'SmokeGridLightAge\(current\)\s*>\s*0u\)[\s\S]*return;' 'Temporal accumulation must not reinterpret retained history as a new sample.'

# CPU mirror: fixed claims cannot exceed either immutable quantity, new/invalid
# claims are independent of partitioning, and every maintenance key is due
# exactly once per partition cycle before age escalation.
$newQuantity = 7
$maintenanceQuantity = 11
$keys = 0..63

foreach ($partitionCount in @(2, 4)) {
	$seen = [bool[]]::new($keys.Count)
	for ($frame = 0; $frame -lt $partitionCount; $frame++) {
		$newRequested = [Math]::Min($newQuantity, 3 + $frame)
		$newScheduled = 0
		$newDeferred = 0
		$maintenanceScheduled = 0
		for ($request = 0; $request -lt $newRequested; $request++) {
			if ($newScheduled -lt $newQuantity) { $newScheduled++ } else { $newDeferred++ }
		}
		foreach ($key in $keys) {
			$due = ($key % $partitionCount) -eq ($frame % $partitionCount)
			if ($due) { $seen[$key] = $true }
			if ($due -and $maintenanceScheduled -lt $maintenanceQuantity) { $maintenanceScheduled++ }
		}
		if ($newScheduled -ne $newRequested -or $newDeferred -ne 0) {
			throw "New/invalid reserved work was partition-filtered: partitions=$partitionCount requested=$newRequested scheduled=$newScheduled deferred=$newDeferred"
		}
		if ($newScheduled -gt $newQuantity -or $maintenanceScheduled -gt $maintenanceQuantity) {
			throw 'A fixed radiance lane exceeded its immutable quantity.'
		}
	}
	if (($seen | Where-Object { -not $_ }).Count -ne 0) { throw "Stable $partitionCount-way rotation failed to visit every world key." }
}

$age = 0
for ($frame = 0; $frame -lt 5; $frame++) { $age = [Math]::Min($age + 1, 65535) }
if ($age -ne 5) { throw 'Deferred compatible history did not retain exact age.' }

# Ticket mirror: every eligible lane receives one ordinal, accepted execution
# is exactly min(requested, quantity), and the published executed count is hard capped.
$quantity = 17
foreach ($requested in @(0, 1, 16, 17, 18, 1024)) {
	$tickets = 0
	$executed = 0
	for ($work = 0; $work -lt $requested; $work++) {
		$ticket = $tickets
		$tickets++
		if ($ticket -lt $quantity) { $executed++ }
	}
	if ($tickets -ne $requested -or $executed -ne [Math]::Min($requested, $quantity) -or $executed -gt $quantity) {
		throw "Ticket accounting failed: requested=$requested tickets=$tickets executed=$executed"
	}
}

Write-Host 'Smoke grid fixed radiance partition contracts passed.'
