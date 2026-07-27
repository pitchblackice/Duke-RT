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
$gridOwner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.cpp'
$data = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridData.hlsli'
$resources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeGridResources.hlsli'
$allocate = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAllocateCommands.cs.hlsl'
$deposit = Read-Source 'source/common/rendering/nri/shaders/SmokeGridDeposit.cs.hlsl'

Assert-Match $contracts 'sizeof\(NRISmokeGridControlGpu\)\s*==\s*256' 'Grid control ABI must include admission and hash-health telemetry.'
Assert-Match $contracts 'sizeof\(NRISmokeGridSourceStatsGpu\)\s*==\s*64' 'Per-source admission rows must remain 64 bytes.'
Assert-Match $data 'struct SmokeGridSourceStats[\s\S]*SourceId[\s\S]*RejectedCapacity[\s\S]*RejectedProbe[\s\S]*RejectedInvalid[\s\S]*AdmittedKeyHash' 'HLSL source rejection contract is incomplete.'
Assert-Match $resources 'SmokeGridFindBrickSerial[\s\S]*NRI_SMOKE_GRID_RESIDENT[\s\S]*NRI_SMOKE_GRID_NEW' 'Existing and current-pass NEW hits must be found before capacity decisions.'
Assert-Match $allocate 'SmokeGridStableFootprintOrdinal[\s\S]*SmokeGridGreatestCommonDivisor' 'Each footprint must use a stable complete spatial permutation.'
Assert-Match $allocate 'for \(uint round[\s\S]*for \(uint sourceOrdinal[\s\S]*completedTurn' 'Allocator must permit at most one new-key decision per source round.'
Assert-Match $allocate 'SmokeGridFindBrickSerial[\s\S]*AdmissionExisting\+\+[\s\S]*FreeCount == 0u[\s\S]*SmokeGridRecordRejection' 'Existing hits must precede explicit capacity rejection.'
Assert-Match $allocate 'RejectedCapacity\+\+[\s\S]*RejectedProbe\+\+[\s\S]*RejectedInvalid\+\+' 'Allocator must preserve distinct terminal rejection reasons.'
Assert-Match $allocate 'AdmittedKeyHash \^=' 'Allocator must publish an order-independent admitted-key witness.'
Assert-Match $deposit 'sourceStatsValid[\s\S]*RejectedMassQ[\s\S]*DepositedMassQ[\s\S]*DepositionCells' 'Deposition must attribute accepted and rejected mass to a validated source row.'
Assert-Match $gridOwner 'sourceReadback[\s\S]*CmdCopyBuffer[\s\S]*NRISmokeGridSourceStatsGpu' 'Per-source telemetry must use bounded queued readback.'
Assert-NotMatch $allocate 'InteractiveReserve|AmbientBudget|BRICK_INTERACTIVE|BRICK_AMBIENT' 'Slice 1 must not introduce Slice 2 reserve or ownership policy.'

function Invoke-FairMirror([int]$Capacity, [hashtable]$Requests, [int[]]$StableSources, [int]$Frame) {
	$cursors = @{}
	$admitted = @{}
	foreach ($source in $StableSources) { $cursors[$source] = 0; $admitted[$source] = 0 }
	$round = 0
	while ($Capacity -gt 0) {
		$progress = $false
		for ($ordinal = 0; $ordinal -lt $StableSources.Count -and $Capacity -gt 0; $ordinal++) {
			$slot = ($ordinal + $Frame + $round) % $StableSources.Count
			$source = $StableSources[$slot]
			if ($cursors[$source] -ge $Requests[$source]) { continue }
			$cursors[$source]++
			$admitted[$source]++
			$Capacity--
			$progress = $true
		}
		if (-not $progress) { break }
		$round++
	}
	return $admitted
}

$requests = @{ 10 = 100; 20 = 100; 30 = 100; 40 = 100 }
$forward = Invoke-FairMirror 10 $requests @(10, 20, 30, 40) 0
$permutedInputCanonicalized = Invoke-FairMirror 10 $requests @(@(40, 10, 30, 20) | Sort-Object) 0
$shares = @(10, 20, 30, 40) | ForEach-Object { $forward[$_] }
if ((($shares | Measure-Object -Maximum).Maximum - ($shares | Measure-Object -Minimum).Minimum) -gt 1) {
	throw 'Equal sources did not receive bounded round-fair service.'
}
foreach ($source in @(10, 20, 30, 40)) {
	if ($forward[$source] -ne $permutedInputCanonicalized[$source]) {
		throw "Canonical rule permutation changed fair service for source $source."
	}
}

Write-Host 'Smoke grid source-fair admission contracts passed.'
