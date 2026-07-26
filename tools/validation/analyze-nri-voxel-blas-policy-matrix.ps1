param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Pairs([string]$Line) {
    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value.Trim('"')
    }
    return $pairs
}

function Get-Stats([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'Cannot summarize an empty timing set.' }
    $sorted = @($Values | Sort-Object)
    function Pick([double]$P) {
        $index = [Math]::Ceiling($P * $sorted.Count) - 1
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

function Require-Field([hashtable]$Row, [string]$Name, [string]$Context) {
    if (-not $Row.ContainsKey($Name)) { throw "$Context is missing '$Name'." }
}

function Read-Entry([object]$Entry) {
    $fixed = Get-Content -LiteralPath $Entry.fixedSummaryPath -Raw | ConvertFrom-Json
    if (-not [bool]$fixed.ok) { throw "Fixed summary failed for sequence $($Entry.sequence)." }

    $gpu = [Collections.Generic.List[hashtable]]::new()
    $workloads = [Collections.Generic.List[hashtable]]::new()
    $preloadVoxelGpu = [Collections.Generic.List[hashtable]]::new()
    $directBlas = [Collections.Generic.List[hashtable]]::new()
    $policyRows = [Collections.Generic.List[hashtable]]::new()
    $memoryRows = [Collections.Generic.List[hashtable]]::new()
    $lifecycleRows = [Collections.Generic.List[hashtable]]::new()
    $gateReleaseRows = [Collections.Generic.List[hashtable]]::new()
    $closureRows = [Collections.Generic.List[hashtable]]::new()
    $reconcileRows = [Collections.Generic.List[hashtable]]::new()
    $compactionRows = [Collections.Generic.List[hashtable]]::new()
    $failureRows = [Collections.Generic.List[string]]::new()

    foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Entry.logPath).Path)) {
        if ($line.StartsWith('PERF pt gpu timing NRI:') -and $line.Contains(' compact=1 ')) { $gpu.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt trace workload NRI:') -and $line.Contains(' compact=1 ')) { $workloads.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel gpu timing NRI:') -and -not $line.Contains(' compact=1 ')) { $preloadVoxelGpu.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel compute direct blas NRI:') -and $line.Contains('action=ready')) { $directBlas.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel blas policy NRI:')) { $policyRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel preload memory NRI:')) { $memoryRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel preload lifecycle NRI:') -and $line.Contains('stage=final-check-release')) { $lifecycleRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel preload timeline NRI:') -and $line.Contains('stage=gate-release')) { $gateReleaseRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel preload closure NRI:') -and $line.Contains(' final=1 ') -and $line.Contains(' outcome=complete ')) { $closureRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('NRI PT voxel residency reconcile:')) { $reconcileRows.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith('PERF pt voxel blas compaction NRI:') -and $line.Contains('stage=complete')) { $compactionRows.Add((Read-Pairs $line)) }
        if ($line -match 'Device removed|device lost|DRED:|NRI render failed|validation error|failed to create|assertion failed|fatal error') { $failureRows.Add($line) }
    }

    $samples = [int]$fixed.samples
    if ($gpu.Count -ne $samples -or $workloads.Count -ne $samples) {
        throw "Sequence $($Entry.sequence) expected $samples GPU/workload rows, found $($gpu.Count)/$($workloads.Count)."
    }
    if ($policyRows.Count -lt 1 -or $memoryRows.Count -lt 1 -or
        ($lifecycleRows.Count -ne 1 -and $gateReleaseRows.Count -ne 1) -or
        $closureRows.Count -lt 1 -or $reconcileRows.Count -lt 1) {
        throw "Sequence $($Entry.sequence) lacks policy, memory, or load lifecycle evidence."
    }
    if ($failureRows.Count -ne 0) { throw "Sequence $($Entry.sequence) contains runtime failure rows." }

    $policy = $policyRows[-1]
    foreach ($field in @('policy', 'compact', 'strict', 'flags')) { Require-Field $policy $field "Sequence $($Entry.sequence) policy row" }
    if ([int]$policy.policy -ne [int]$Entry.policy -or [int]$policy.compact -ne [int][bool]$Entry.compact -or [int]$policy.strict -ne 1) {
        throw "Sequence $($Entry.sequence) policy identity does not match its manifest."
    }
    if ([bool]$Entry.compact -and $compactionRows.Count -ne 1) { throw "Sequence $($Entry.sequence) has no completed compaction." }
    if (-not [bool]$Entry.compact -and $compactionRows.Count -ne 0) { throw "Sequence $($Entry.sequence) unexpectedly compacted BLASes." }
    $closure = $closureRows[-1]
    Require-Field $closure 'unique_meshes' "Sequence $($Entry.sequence) closure"
    $reconcile = $reconcileRows[-1]
    Require-Field $reconcile 'mesh_resources' "Sequence $($Entry.sequence) final reconciliation"
    if ([bool]$Entry.compact -and [uint32]$compactionRows[0].resources -ne [uint32]$reconcile.mesh_resources) {
        throw "Sequence $($Entry.sequence) compaction covered $($compactionRows[0].resources) resources, not the final $($reconcile.mesh_resources) meshes."
    }

    $settingsKeys = @($workloads | ForEach-Object { Require-Field $_ 'settings_key' 'workload'; [string]$_['settings_key'] } | Sort-Object -Unique)
    $populationKeys = @($workloads | ForEach-Object {
        foreach ($field in @('voxel_occurrences', 'voxel_instance_prims', 'runtime_lights', 'light_tile_indices', 'emissive_prims')) { Require-Field $_ $field 'workload' }
        "occ=$($_['voxel_occurrences']);prims=$($_['voxel_instance_prims']);lights=$($_['runtime_lights']);indices=$($_['light_tile_indices']);emissive=$($_['emissive_prims'])"
    } | Sort-Object -Unique)
    if ($settingsKeys.Count -ne 1 -or $populationKeys.Count -ne 1) { throw "Sequence $($Entry.sequence) changed settings or population inside its fixed window." }

    foreach ($row in $gpu) {
        foreach ($field in @('segment', 'scene', 'trace_dispatch', 'invalid', 'dropped', 'resolved', 'expected')) { Require-Field $row $field 'GPU timing' }
        if ([int]$row.invalid -ne 0 -or [int]$row.dropped -ne 0 -or [int]$row.resolved -ne [int]$row.expected) {
            throw "Sequence $($Entry.sequence) has an invalid GPU sample."
        }
    }

    $memory = $memoryRows[-1]
    foreach ($field in @('pv_scene_bytes', 'pv_as_bytes', 'direct_blas_bytes', 'renderer_tracked_bytes', 'local_usage_bytes',
        'peak_tracked_bytes', 'arena_vertex_committed', 'arena_vertex_used', 'arena_index_committed', 'arena_index_used',
        'arena_primitive_committed', 'arena_primitive_used')) { Require-Field $memory $field 'preload memory' }

    $blasGpu = @($preloadVoxelGpu | Where-Object { $_.ContainsKey('voxel_blas') } | ForEach-Object { [double]$_['voxel_blas'] })
    $requestToBlas = @($directBlas | Where-Object { $_.ContainsKey('request_to_blas') } | ForEach-Object { [double]$_['request_to_blas'] })
    $readyToBlas = @($directBlas | Where-Object { $_.ContainsKey('ready_to_blas') } | ForEach-Object { [double]$_['ready_to_blas'] })
    $compaction = if ($compactionRows.Count -eq 1) { $compactionRows[0] } else { $null }
    return [pscustomobject]@{
        sequence = [int]$Entry.sequence; cycle = [int]$Entry.cycle; leg = [string]$Entry.leg
        policy = [int]$Entry.policy; compact = [bool]$Entry.compact; samples = $samples
        settingsKey = $settingsKeys[0]; populationKey = $populationKeys[0]
        manifestHash = [string]$fixed.strictFirstFrameRelease.manifestHash
        selectedBindings = [uint64]$fixed.strictFirstFrameRelease.selectedBindings
        activeInstances = [uint64]$fixed.strictFirstFrameRelease.activeInstances
        completeValues = [double[]]@($gpu | ForEach-Object { [double]$_['segment'] })
        traceValues = [double[]]@($gpu | ForEach-Object { [double]$_['trace_dispatch'] })
        sceneValues = [double[]]@($gpu | ForEach-Object { [double]$_['scene'] })
        worldTlasValues = [double[]]@($preloadVoxelGpu | Where-Object { $_.ContainsKey('world_tlas') } | ForEach-Object { [double]$_['world_tlas'] })
        loadMs = if ($lifecycleRows.Count -eq 1) { [double]$lifecycleRows[0].elapsed_ms } else { [double]$gateReleaseRows[0].elapsed_ms }
        meshResources = [uint32]$reconcile.mesh_resources
        preloadBlasGpuMs = if ($blasGpu.Count) { [Math]::Round((@($blasGpu) | Measure-Object -Sum).Sum, 3) } else { $null }
        blasGpuSamples = $blasGpu.Count
        requestToBlasMax = if ($requestToBlas.Count) { (@($requestToBlas) | Measure-Object -Maximum).Maximum } else { 0 }
        readyToBlasMax = if ($readyToBlas.Count) { (@($readyToBlas) | Measure-Object -Maximum).Maximum } else { 0 }
        memory = [pscustomobject][ordered]@{
            sceneBytes = [uint64]$memory.pv_scene_bytes; asBytes = [uint64]$memory.pv_as_bytes
            directBlasBytes = [uint64]$memory.direct_blas_bytes; rendererTrackedBytes = [uint64]$memory.renderer_tracked_bytes
            localUsageBytes = [uint64]$memory.local_usage_bytes; peakTrackedBytes = [uint64]$memory.peak_tracked_bytes
            vertexCommitted = [uint64]$memory.arena_vertex_committed; vertexUsed = [uint64]$memory.arena_vertex_used
            indexCommitted = [uint64]$memory.arena_index_committed; indexUsed = [uint64]$memory.arena_index_used
            primitiveCommitted = [uint64]$memory.arena_primitive_committed; primitiveUsed = [uint64]$memory.arena_primitive_used
        }
        compaction = if ($null -ne $compaction) { [pscustomobject][ordered]@{
            resources = [uint32]$compaction.resources; copied = [uint32]$compaction.copied
            originalBytes = [uint64]$compaction.original_bytes; compactedBytes = [uint64]$compaction.compacted_bytes
            savedBytes = [uint64]$compaction.saved_bytes
        } } else { $null }
    }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$runs = @($manifest.entries | ForEach-Object { Read-Entry $_ })
$identity = @($runs | ForEach-Object { "$($_.manifestHash)/$($_.selectedBindings)/$($_.activeInstances)/$($_.populationKey)" } | Sort-Object -Unique)
if ($identity.Count -ne 1) { throw 'Matrix legs did not retain one strict scene/voxel/light population identity.' }

$legs = @($runs.leg | Sort-Object -Unique | ForEach-Object {
    $name = $_
    $selected = @($runs | Where-Object { $_.leg -eq $name })
    $settings = @($selected.settingsKey | Sort-Object -Unique)
    if ($settings.Count -ne 1) { throw "Leg '$name' changed trace settings identity." }
    $complete = [double[]]@($selected | ForEach-Object { $_.completeValues })
    $trace = [double[]]@($selected | ForEach-Object { $_.traceValues })
    $scene = [double[]]@($selected | ForEach-Object { $_.sceneValues })
    $tlas = [double[]]@($selected | ForEach-Object { $_.worldTlasValues })
    [pscustomobject][ordered]@{
        leg = $name; policy = $selected[0].policy; compact = $selected[0].compact
        runs = $selected.Count; samples = $complete.Count; settingsKey = $settings[0]
        completeGpu = Get-Stats $complete; traceDispatch = Get-Stats $trace; scene = Get-Stats $scene
        worldTlas = if ($tlas.Count) { Get-Stats $tlas } else { $null }
        loadMs = Get-Stats ([double[]]@($selected.loadMs))
        preloadBlasGpuMs = if (@($selected.preloadBlasGpuMs | Where-Object { $null -ne $_ }).Count) {
            Get-Stats ([double[]]@($selected.preloadBlasGpuMs | Where-Object { $null -ne $_ }))
        } else { $null }
        requestToBlasMax = (@($selected.requestToBlasMax) | Measure-Object -Maximum).Maximum
        readyToBlasMax = (@($selected.readyToBlasMax) | Measure-Object -Maximum).Maximum
        memory = @($selected.memory)
        compaction = @($selected.compaction | Where-Object { $null -ne $_ })
    }
})

$summary = [pscustomobject][ordered]@{
    ok = $true; schema = 1; manifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
    generatedUtc = (Get-Date).ToUniversalTime().ToString('o'); identity = $identity[0]
    legs = $legs
    runs = @($runs | Select-Object sequence, cycle, leg, policy, compact, samples, settingsKey, populationKey, manifestHash,
        selectedBindings, activeInstances, meshResources, loadMs, preloadBlasGpuMs, blasGpuSamples, requestToBlasMax, readyToBlasMax, memory, compaction)
}
$parent = Split-Path -Parent $SummaryOutput
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
Write-Host "NRI voxel BLAS policy matrix passed: runs=$($runs.Count) legs=$($legs.Count) identity=$($identity[0]) summary=$SummaryOutput"
