param(
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$cacheFlag = [uint32]0x2000
$cacheAcceptFlag = [uint32]0x4000
$cacheFlagMask = [uint32]($cacheFlag -bor $cacheAcceptFlag)
$counterFields = [ordered]@{
	lookups = 'lookups'
	accepted = 'accepted'
	forcedMiss = 'forced_miss'
	collisions = 'collision'
	stale = 'stale'
	unsupported = 'unsupported'
	exactFallback = 'exact_fallback'
	occupancy = 'occupancy'
	updates = 'updates'
	clears = 'clears'
}

function Read-Pairs([string]$Line) {
	$pairs = @{}
	foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
		$pairs[$match.Groups[1].Value] = $match.Groups[2].Value.Trim('"')
	}
	return $pairs
}

function Require-Field([hashtable]$Row, [string]$Name, [string]$Context) {
	if (-not $Row.ContainsKey($Name)) { throw "$Context is missing '$Name'." }
}

function Get-Stats([double[]]$Values) {
	if ($Values.Count -eq 0) { throw 'Cannot summarize an empty timing set.' }
	$sorted = @($Values | Sort-Object)
	function Pick([double]$Percentile) {
		$index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
		return [double]$sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
	}
	return [pscustomobject][ordered]@{
		samples = $sorted.Count
		p50 = [Math]::Round((Pick 0.50), 3)
		p95 = [Math]::Round((Pick 0.95), 3)
		p99 = [Math]::Round((Pick 0.99), 3)
		max = [Math]::Round([double]$sorted[-1], 3)
	}
}

function Get-CounterSnapshot([hashtable]$Row, [string]$Context) {
	$result = [ordered]@{}
	foreach ($mapping in $counterFields.GetEnumerator()) {
		Require-Field $Row $mapping.Value $Context
		$result[$mapping.Key] = [uint64]$Row[$mapping.Value]
	}
	return [pscustomobject]$result
}

function Get-Delta([uint64]$Start, [uint64]$End, [bool]$AllowUint32Wrap) {
	if ($End -ge $Start) { return [uint64]($End - $Start) }
	if (-not $AllowUint32Wrap -or $Start -gt [uint32]::MaxValue -or $End -gt [uint32]::MaxValue) {
		throw "Cumulative counter regressed from $Start to $End."
	}
	return [uint64](([uint64][uint32]::MaxValue + 1) - $Start + $End)
}

function Get-CacheWindow([object]$Entry, [hashtable[]]$Rows, [uint64]$FirstNriFrame, [uint64]$LastNriFrame) {
	$mode = [string]$Entry.mode
	$expectedRequested = if ([bool]$Entry.cacheRequested) { 1 } else { 0 }
	foreach ($row in $Rows) {
		foreach ($field in @('requested', 'mode', 'valid', 'telemetry_frame', 'table_bytes', 'total_bytes', 'invalidation', 'pending_readbacks')) {
			Require-Field $row $field "Sequence $($Entry.sequence) cache telemetry"
		}
		[void](Get-CounterSnapshot $row "Sequence $($Entry.sequence) cache telemetry")
		if ([int]$row.requested -ne $expectedRequested) {
			throw "Sequence $($Entry.sequence) cache request state does not match mode '$mode'."
		}
	}

	if (-not [bool]$Entry.cacheRequested) {
		if ($Rows.Count -lt 1) { throw "Sequence $($Entry.sequence) has no exact-mode cache telemetry." }
		foreach ($row in $Rows) {
			$counters = Get-CounterSnapshot $row 'exact-mode cache telemetry'
			foreach ($name in $counterFields.Keys) {
				if ([uint64]$counters.$name -ne 0) { throw "Sequence $($Entry.sequence) exact mode has nonzero cache counter '$name'." }
			}
			if ([uint64]$row.table_bytes -ne 0 -or [uint64]$row.total_bytes -ne 0) {
				throw "Sequence $($Entry.sequence) exact mode allocated cache memory."
			}
		}
		$zero = [ordered]@{}
		foreach ($name in $counterFields.Keys) { $zero[$name] = [uint64]0 }
		return [pscustomobject][ordered]@{
			rows = $Rows.Count
			baselineTelemetryFrame = 0
			endTelemetryFrame = 0
			baseline = [pscustomobject]$zero
			end = [pscustomobject]$zero
			delta = [pscustomobject]$zero
			memory = [pscustomobject]@{ tableBytes = [uint64]0; totalBytes = [uint64]0 }
			pendingReadbacksMax = [uint32](($Rows | ForEach-Object { [uint32]$_.pending_readbacks } | Measure-Object -Maximum).Maximum)
		}
	}

	$expectedMode = if ($mode -eq 'forced-miss') { 'exact-miss' } elseif ($mode -eq 'age-one') { 'age-one' } else { throw "Unknown active cache mode '$mode'." }
	$validRows = @($Rows | Where-Object { [int]$_.valid -eq 1 })
	if ($validRows.Count -lt 2) { throw "Sequence $($Entry.sequence) has fewer than two valid cache snapshots." }
	if (@($Rows | Where-Object { $_.mode -ne $expectedMode }).Count -ne 0) {
		throw "Sequence $($Entry.sequence) did not actuate cache mode '$expectedMode'."
	}

	$byTelemetryFrame = @{}
	foreach ($row in $validRows) {
		$key = [string][uint64]$row.telemetry_frame
		if ($byTelemetryFrame.ContainsKey($key)) {
			$prior = $byTelemetryFrame[$key]
			foreach ($field in @($counterFields.Values) + @('table_bytes', 'total_bytes', 'invalidation')) {
				if ([string]$prior[$field] -ne [string]$row[$field]) {
					throw "Sequence $($Entry.sequence) has conflicting cache snapshots for telemetry frame $key."
				}
			}
		}
		$byTelemetryFrame[$key] = $row
	}

	$baselineFrame = $FirstNriFrame - 1
	$baselineKey = [string]$baselineFrame
	$endKey = [string]$LastNriFrame
	if (-not $byTelemetryFrame.ContainsKey($baselineKey) -or -not $byTelemetryFrame.ContainsKey($endKey)) {
		throw "Sequence $($Entry.sequence) lacks exact cache telemetry boundaries $baselineFrame..$LastNriFrame."
	}
	$baselineRow = $byTelemetryFrame[$baselineKey]
	$endRow = $byTelemetryFrame[$endKey]
	$baseline = Get-CounterSnapshot $baselineRow 'cache baseline'
	$end = Get-CounterSnapshot $endRow 'cache endpoint'
	$delta = [ordered]@{}
	foreach ($name in $counterFields.Keys) {
		$delta[$name] = Get-Delta -Start ([uint64]$baseline.$name) -End ([uint64]$end.$name) -AllowUint32Wrap ($name -ne 'clears')
	}
	if ([uint64]$delta.clears -ne 0) { throw "Sequence $($Entry.sequence) cleared the cache inside the measured window." }

	$windowRows = @($validRows | Where-Object { [uint64]$_.telemetry_frame -ge $baselineFrame -and [uint64]$_.telemetry_frame -le $LastNriFrame })
	$tableBytes = @($windowRows | ForEach-Object { [uint64]$_.table_bytes } | Sort-Object -Unique)
	$totalBytes = @($windowRows | ForEach-Object { [uint64]$_.total_bytes } | Sort-Object -Unique)
	if ($tableBytes.Count -ne 1 -or $totalBytes.Count -ne 1 -or $tableBytes[0] -eq 0 -or $totalBytes[0] -lt $tableBytes[0]) {
		throw "Sequence $($Entry.sequence) cache memory was zero or unstable inside the measured window."
	}
	if ($mode -eq 'forced-miss' -and ([uint64]$delta.accepted -ne 0 -or [uint64]$delta.exactFallback -ne [uint64]$delta.lookups)) {
		throw "Sequence $($Entry.sequence) forced-miss invariant failed: accepted=$($delta.accepted) fallbacks=$($delta.exactFallback) lookups=$($delta.lookups)."
	}

	return [pscustomobject][ordered]@{
		rows = $Rows.Count
		baselineTelemetryFrame = $baselineFrame
		endTelemetryFrame = $LastNriFrame
		baseline = $baseline
		end = $end
		delta = [pscustomobject]$delta
		memory = [pscustomobject]@{ tableBytes = [uint64]$tableBytes[0]; totalBytes = [uint64]$totalBytes[0] }
		pendingReadbacksMax = [uint32](($Rows | ForEach-Object { [uint32]$_.pending_readbacks } | Measure-Object -Maximum).Maximum)
	}
}

function Get-NormalizedWorkloadIdentity([hashtable]$Row, [string]$Context) {
	$fields = @(
		'schema', 'render_w', 'render_h', 'output_w', 'output_h', 'dispatch_x', 'dispatch_y', 'dispatch_z',
		'light_bounces', 'mirror_bounces', 'portal_depth', 'emissive_samples', 'emissive_requested', 'emissive_budget',
		'indirect_requested', 'indirect_effective', 'indirect_active', 'hit_recon', 'runtime_lights', 'light_tiles_x',
		'light_tiles_y', 'light_tile_size', 'light_tile_indices', 'light_tile_max', 'emissive_prims', 'emissive_power',
		'voxel_occurrences', 'voxel_instance_prims', 'voxel_occurrence_control', 'debug', 'bootstrap', 'upscaler',
		'upscaler_mode', 'denoiser', 'direct_scene', 'directional', 'directional_shadow', 'split_shadow',
		'fast_emissive_shadow', 'visible_chunk_gate'
	)
	$parts = [Collections.Generic.List[string]]::new()
	foreach ($field in $fields) {
		Require-Field $Row $field $Context
		$parts.Add("$field=$($Row[$field])")
	}
	Require-Field $Row 'flags' $Context
	$normalizedFlags = [uint32]$Row.flags -band (-bnot $cacheFlagMask)
	$parts.Add("flags=$normalizedFlags")
	return $parts -join ';'
}

function Read-Entry([object]$Entry) {
	$fixed = Get-Content -LiteralPath $Entry.fixedSummaryPath -Raw | ConvertFrom-Json
	if (-not [bool]$fixed.ok) { throw "Fixed summary failed for sequence $($Entry.sequence)." }
	$gpu = [Collections.Generic.List[hashtable]]::new()
	$workloads = [Collections.Generic.List[hashtable]]::new()
	$cacheRows = [Collections.Generic.List[hashtable]]::new()
	$policyRows = [Collections.Generic.List[hashtable]]::new()
	$failures = [Collections.Generic.List[string]]::new()
	foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Entry.logPath).Path)) {
		if ($line.StartsWith('PERF pt gpu timing NRI:') -and $line.Contains(' compact=1 ')) { $gpu.Add((Read-Pairs $line)) }
		elseif ($line.StartsWith('PERF pt trace workload NRI:') -and $line.Contains(' compact=1 ')) { $workloads.Add((Read-Pairs $line)) }
		elseif ($line.StartsWith('PERF pt indirect radiance cache NRI:')) { $cacheRows.Add((Read-Pairs $line)) }
		elseif ($line.StartsWith('PERF pt voxel blas policy NRI:')) { $policyRows.Add((Read-Pairs $line)) }
		if ($line -match 'Device removed|device lost|DRED:|NRI render failed|validation error|failed to create|assertion failed|fatal error') { $failures.Add($line) }
	}
	if ($failures.Count -ne 0) { throw "Sequence $($Entry.sequence) contains runtime failure rows." }
	$samples = [int]$fixed.samples
	if ($gpu.Count -ne $samples -or $workloads.Count -ne $samples) {
		throw "Sequence $($Entry.sequence) expected $samples GPU/workload rows, found $($gpu.Count)/$($workloads.Count)."
	}
	if ($policyRows.Count -lt 1) { throw "Sequence $($Entry.sequence) has no voxel BLAS policy row." }
	$policy = $policyRows[-1]
	foreach ($field in @('policy', 'compact', 'strict')) { Require-Field $policy $field 'voxel BLAS policy' }
	if ([int]$policy.policy -ne [int]$Entry.blasPolicy -or [int]$policy.compact -ne 0 -or [int]$policy.strict -ne 1) {
		throw "Sequence $($Entry.sequence) did not retain its explicit exact BLAS policy."
	}

	$orderedGpu = @($gpu | Sort-Object { [int]$_.sample })
	$orderedWorkloads = @($workloads | Sort-Object { [int]$_.sample })
	$workloadIdentities = [Collections.Generic.HashSet[string]]::new()
	$settingsKeys = [Collections.Generic.HashSet[string]]::new()
	for ($index = 0; $index -lt $samples; ++$index) {
		$gpuRow = $orderedGpu[$index]
		$workload = $orderedWorkloads[$index]
		foreach ($field in @('sample', 'nri_frame', 'segment', 'trace_dispatch', 'segments', 'invalid', 'dropped', 'resolved', 'expected')) { Require-Field $gpuRow $field 'GPU timing' }
		Require-Field $workload 'renderer_frame' 'trace workload'
		if ([int]$gpuRow.sample -ne $index -or [int]$gpuRow.invalid -ne 0 -or [int]$gpuRow.dropped -ne 0 -or
			[int]$gpuRow.expected -lt 1 -or [int]$gpuRow.resolved -ne [int]$gpuRow.expected -or
			[int]$gpuRow.segments -lt 1 -or [double]$gpuRow.segment -le 0.0 -or [double]$gpuRow.trace_dispatch -le 0.0) {
			throw "Sequence $($Entry.sequence) has an invalid GPU sample at index $index."
		}
		Require-Field $workload 'sample' 'trace workload'
		Require-Field $workload 'settings_key' 'trace workload'
		if ([int]$workload.sample -ne $index) { throw "Sequence $($Entry.sequence) has a noncontiguous workload sample." }
		[void]$settingsKeys.Add([string]$workload.settings_key)
		[void]$workloadIdentities.Add((Get-NormalizedWorkloadIdentity $workload 'trace workload'))
		$modeBits = [uint32]$workload.flags -band $cacheFlagMask
		$expectedBits = switch ([string]$Entry.mode) {
			'exact' { [uint32]0 }
			'forced-miss' { $cacheFlag }
			'age-one' { $cacheFlagMask }
			default { throw "Unknown cache mode '$($Entry.mode)'." }
		}
		if ($modeBits -ne $expectedBits) { throw "Sequence $($Entry.sequence) workload cache flags do not match mode '$($Entry.mode)'." }
	}
	if ($settingsKeys.Count -ne 1 -or $workloadIdentities.Count -ne 1) {
		throw "Sequence $($Entry.sequence) changed settings or normalized workload identity inside the fixed window."
	}

	$firstRendererFrame = [uint64]$orderedWorkloads[0].renderer_frame
	$lastRendererFrame = [uint64]$orderedWorkloads[-1].renderer_frame
	if ($lastRendererFrame -ne $firstRendererFrame + [uint64]$samples - 1) {
		throw "Sequence $($Entry.sequence) renderer frames are not contiguous."
	}
	$cache = Get-CacheWindow -Entry $Entry -Rows $cacheRows.ToArray() -FirstNriFrame $firstRendererFrame -LastNriFrame $lastRendererFrame
	return [pscustomobject][ordered]@{
		sequence = [int]$Entry.sequence
		cycle = [int]$Entry.cycle
		ordinal = [int]$Entry.ordinal
		mode = [string]$Entry.mode
		blasPolicy = [int]$Entry.blasPolicy
		samples = $samples
		settingsKey = @($settingsKeys)[0]
		workloadIdentity = @($workloadIdentities)[0]
		manifestHash = [string]$fixed.strictFirstFrameRelease.manifestHash
		selectedBindings = [uint64]$fixed.strictFirstFrameRelease.selectedBindings
		activeInstances = [uint64]$fixed.strictFirstFrameRelease.activeInstances
		batchReadyActors = [uint64]$fixed.strictFirstFrameRelease.batchReadyActors
		completeValues = [double[]]@($orderedGpu | ForEach-Object { [double]$_.segment })
		traceValues = [double[]]@($orderedGpu | ForEach-Object { [double]$_.trace_dispatch })
		cache = $cache
	}
}

$manifest = Get-Content -LiteralPath (Resolve-Path -LiteralPath $ManifestPath) -Raw | ConvertFrom-Json
$manifestProperties = @($manifest.PSObject.Properties.Name)
foreach ($field in @('cycles', 'samples', 'warmupSamples', 'blasPolicy', 'entries')) {
	if (-not $manifestProperties.Contains($field)) { throw "Cache matrix manifest is missing '$field'." }
}
$manifestModes = if ($manifestProperties.Contains('modes')) {
	@($manifest.modes | ForEach-Object { ([string]$_).ToLowerInvariant() })
}
else {
	# Backward compatibility for the original schema-1 3x3 manifest.
	@($manifest.entries | Where-Object { [int]$_.cycle -eq 1 } | Sort-Object { [int]$_.ordinal } | ForEach-Object { ([string]$_.mode).ToLowerInvariant() })
}
$allowedModes = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($mode in @('exact', 'forced-miss', 'age-one')) { [void]$allowedModes.Add($mode) }
$uniqueManifestModes = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($mode in $manifestModes) {
	if (-not $allowedModes.Contains($mode)) { throw "Cache matrix manifest contains unknown mode '$mode'." }
	if (-not $uniqueManifestModes.Add($mode)) { throw "Cache matrix manifest contains duplicate mode '$mode'." }
}
foreach ($requiredMode in @('exact', 'forced-miss')) {
	if (-not $uniqueManifestModes.Contains($requiredMode)) { throw "Cache matrix manifest must include '$requiredMode'." }
}
$modeCount = $manifestModes.Count
$cycles = [int]$manifest.cycles
if ($modeCount -lt 2 -or $modeCount -gt 3) { throw 'Cache matrix manifest must select two or three modes.' }
if ($cycles -ne $modeCount) { throw "Cache matrix cycles ($cycles) must equal its mode count ($modeCount)." }
if ($manifestProperties.Contains('modeCount') -and [int]$manifest.modeCount -ne $modeCount) {
	throw 'Cache matrix manifest modeCount does not match its modes list.'
}
$expectedRunCount = $cycles * $modeCount
if (@($manifest.entries).Count -ne $expectedRunCount) {
	throw "Cache matrix must contain exactly $expectedRunCount runs for its balanced ${modeCount}x${cycles} rotation."
}
$runs = @($manifest.entries | ForEach-Object { Read-Entry $_ })
$expectedSequence = 0
foreach ($cycle in 1..$cycles) {
	$cycleRuns = @($runs | Where-Object cycle -eq $cycle | Sort-Object ordinal)
	if ($cycleRuns.Count -ne $modeCount -or @($cycleRuns.mode | Sort-Object -Unique).Count -ne $modeCount) {
		throw "Cycle $cycle is not a complete $modeCount-mode rotation."
	}
	$start = (1 - $cycle + $modeCount) % $modeCount
	for ($offset = 0; $offset -lt $modeCount; ++$offset) {
		++$expectedSequence
		$run = $cycleRuns[$offset]
		$expectedMode = $manifestModes[($start + $offset) % $modeCount]
		if ($run.ordinal -ne $offset + 1 -or $run.sequence -ne $expectedSequence -or $run.mode -ne $expectedMode) {
			throw "Cycle $cycle ordinal $($offset + 1) does not match the declared balanced rotation."
		}
	}
}
foreach ($mode in $manifestModes) {
	$ordinals = @($runs | Where-Object mode -eq $mode | ForEach-Object ordinal | Sort-Object -Unique)
	$expectedOrdinals = 1..$modeCount
	if ($ordinals.Count -ne $modeCount -or ($ordinals -join ',') -ne ($expectedOrdinals -join ',')) {
		throw "Mode '$mode' did not occupy every rotation ordinal exactly once."
	}
}

$identity = @($runs | ForEach-Object {
	"$($_.manifestHash)/$($_.selectedBindings)/$($_.activeInstances)/$($_.batchReadyActors)/$($_.workloadIdentity)"
} | Sort-Object -Unique)
if ($identity.Count -ne 1) { throw "Cache matrix legs did not retain one strict content identity: $($identity -join ' | ')" }
if (@($runs.blasPolicy | Sort-Object -Unique).Count -ne 1 -or [int]$runs[0].blasPolicy -ne [int]$manifest.blasPolicy) {
	throw 'Cache matrix did not retain one explicit voxel BLAS policy.'
}

$legs = @($manifestModes | ForEach-Object {
	$mode = $_
	$selected = @($runs | Where-Object mode -eq $mode)
	$settings = @($selected.settingsKey | Sort-Object -Unique)
	if ($settings.Count -ne 1) { throw "Mode '$mode' changed settings identity between cycles." }
	$totals = [ordered]@{}
	$perRun = [ordered]@{}
	foreach ($counter in $counterFields.Keys) {
		$values = [double[]]@($selected | ForEach-Object { [double]$_.cache.delta.$counter })
		$totals[$counter] = [uint64](($values | Measure-Object -Sum).Sum)
		$perRun[$counter] = Get-Stats $values
	}
	$lookupTotal = [double]$totals.lookups
	[pscustomobject][ordered]@{
		mode = $mode
		runs = $selected.Count
		samples = (@($selected | Measure-Object -Property samples -Sum).Sum)
		settingsKey = $settings[0]
		completeGpu = Get-Stats ([double[]]@($selected | ForEach-Object completeValues))
		traceDispatch = Get-Stats ([double[]]@($selected | ForEach-Object traceValues))
		cache = [pscustomobject][ordered]@{
			totals = [pscustomobject]$totals
			perRun = [pscustomobject]$perRun
			acceptRate = if ($lookupTotal -gt 0.0) { [Math]::Round(100.0 * [double]$totals.accepted / $lookupTotal, 3) } else { 0.0 }
			exactFallbackRate = if ($lookupTotal -gt 0.0) { [Math]::Round(100.0 * [double]$totals.exactFallback / $lookupTotal, 3) } else { 0.0 }
			collisionRate = if ($lookupTotal -gt 0.0) { [Math]::Round(100.0 * [double]$totals.collisions / $lookupTotal, 3) } else { 0.0 }
			staleRate = if ($lookupTotal -gt 0.0) { [Math]::Round(100.0 * [double]$totals.stale / $lookupTotal, 3) } else { 0.0 }
			unsupportedRate = if ($lookupTotal -gt 0.0) { [Math]::Round(100.0 * [double]$totals.unsupported / $lookupTotal, 3) } else { 0.0 }
			occupancyEnd = Get-Stats ([double[]]@($selected | ForEach-Object { [double]$_.cache.end.occupancy }))
			tableBytes = @($selected | ForEach-Object { [uint64]$_.cache.memory.tableBytes } | Sort-Object -Unique)
			totalBytes = @($selected | ForEach-Object { [uint64]$_.cache.memory.totalBytes } | Sort-Object -Unique)
			pendingReadbacksMax = [uint32](($selected.cache.pendingReadbacksMax | Measure-Object -Maximum).Maximum)
		}
	}
})

$summary = [pscustomobject][ordered]@{
	ok = $true
	schema = 1
	manifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
	generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
	modes = $manifestModes
	modeCount = $modeCount
	cycles = $cycles
	samplesPerRun = [int]$manifest.samples
	warmupSamples = [int]$manifest.warmupSamples
	blasPolicy = [int]$manifest.blasPolicy
	identity = $identity[0]
	legs = $legs
	runs = @($runs | Select-Object sequence, cycle, ordinal, mode, blasPolicy, samples, settingsKey, manifestHash,
		selectedBindings, activeInstances, batchReadyActors, cache)
}
$parent = Split-Path -Parent $SummaryOutput
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
$summary | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
Write-Host "NRI indirect-radiance cache matrix passed: modes=$($manifestModes -join ',') cycles=$cycles runs=$($runs.Count) identity=$($identity[0]) summary=$SummaryOutput"
