param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,

    [Parameter(Mandatory = $true)][string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-Pairs {
    param([string]$Line)
    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $pairs
}

function Get-Stats {
    param([double[]]$Values, [double]$TargetMs = 0.0)
    if ($Values.Count -eq 0) { throw "Cannot summarize an empty timing set." }
    $sorted = @($Values | Sort-Object)
    function Pick([double]$Percentile) {
        $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
        return [double]$sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
    }
    $stats = [ordered]@{
        samples = $sorted.Count
        p50 = [Math]::Round((Pick 0.50), 3)
        p95 = [Math]::Round((Pick 0.95), 3)
        p99 = [Math]::Round((Pick 0.99), 3)
        max = [Math]::Round([double]$sorted[$sorted.Count - 1], 3)
    }
    if ($TargetMs -gt 0.0) {
        $overTarget = @($sorted | Where-Object { $_ -gt $TargetMs }).Count
        $stats.targetMs = [Math]::Round($TargetMs, 3)
        $stats.overTargetCount = $overTarget
        $stats.overTargetPercent = [Math]::Round(100.0 * $overTarget / $sorted.Count, 3)
    }
    return [pscustomobject]$stats
}

function Get-ExactCounts {
    param([string[]]$Values)
    return @($Values | Group-Object | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ value = [string]$_.Name; count = $_.Count }
    })
}

function Assert-ExpectedTrace {
    param([hashtable]$Row, [object]$Expected, [string]$Context)
    foreach ($property in $Expected.PSObject.Properties) {
        if (-not $Row.ContainsKey($property.Name)) {
            throw "$Context is missing expected trace field '$($property.Name)'."
        }
        if ([string]$Row[$property.Name] -ne [string]$property.Value) {
            throw "$Context field '$($property.Name)' is '$($Row[$property.Name])', expected '$($property.Value)'."
        }
    }
}

function Get-WorkloadShape {
    param([hashtable]$Row, [switch]$IncludeOccurrence)
    $fields = @(
        "schema", "render_w", "render_h", "output_w", "output_h", "dispatch_x", "dispatch_y", "dispatch_z",
        "indirect_requested", "indirect_effective", "indirect_active", "hit_recon",
        "light_tiles_x", "light_tiles_y", "light_tile_size",
        "flags", "debug", "bootstrap", "upscaler", "upscaler_mode",
        "denoiser", "direct_scene", "directional", "directional_shadow", "split_shadow",
        "fast_emissive_shadow", "visible_chunk_gate"
    )
    if ($IncludeOccurrence -and [int]$Row.schema -ge 3) {
        $fields += "voxel_occurrence_control"
    }
    return ($fields | ForEach-Object {
        if (-not $Row.ContainsKey($_)) { throw "Trace workload row is missing identity field '$_'." }
        "$_=$($Row[$_])"
    }) -join ";"
}

function Read-Run {
    param([object]$Entry)

    if ([int]$Entry.exitCode -ne 0) { throw "Matrix entry $($Entry.sequence) has exit code $($Entry.exitCode)." }
    if (-not (Test-Path -LiteralPath $Entry.logPath)) { throw "Missing matrix log: $($Entry.logPath)" }
    if (Test-Path -LiteralPath $Entry.baseSummaryPath) {
        $baseSummary = Get-Content -LiteralPath $Entry.baseSummaryPath -Raw | ConvertFrom-Json
        if (-not [bool]$baseSummary.ok) { throw "Base perf summary failed: $($Entry.baseSummaryPath)" }
    }

    $scenario = Get-Content -LiteralPath $Entry.scenarioPath -Raw | ConvertFrom-Json
    $matrix = $scenario.directTraceMatrix
    if ([string]$matrix.leg -ne [string]$Entry.leg) { throw "Manifest/scenario leg mismatch for entry $($Entry.sequence)." }
    $minimumSchema = [int]$matrix.minimumWorkloadSchema
    $targetFrameMs = [double]$matrix.targetFrameMs
    if ($targetFrameMs -le 0.0) { throw "Entry $($Entry.sequence) has no positive targetFrameMs." }

    $completion = @()
    $loops = @()
    $gpu = @()
    $workloads = @()
    $voxelGpu = @()
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Entry.logPath).Path)) {
        if ($line.StartsWith("PERF compact capture complete:", [StringComparison]::Ordinal)) { $completion += ,(Read-Pairs $line) }
        elseif ($line.StartsWith("PERF loop trace:", [StringComparison]::Ordinal) -and $line.Contains(" compact=1 ")) { $loops += ,(Read-Pairs $line) }
        elseif ($line.StartsWith("PERF pt gpu timing NRI:", [StringComparison]::Ordinal) -and $line.Contains(" compact=1 ")) { $gpu += ,(Read-Pairs $line) }
        elseif ($line.StartsWith("PERF pt trace workload NRI:", [StringComparison]::Ordinal) -and $line.Contains(" compact=1 ")) { $workloads += ,(Read-Pairs $line) }
        elseif ($line.StartsWith("PERF pt voxel gpu timing NRI:", [StringComparison]::Ordinal) -and $line.Contains(" compact=1 ")) { $voxelGpu += ,(Read-Pairs $line) }
    }

    if ($completion.Count -ne 1) { throw "Entry $($Entry.sequence) expected one compact completion row, found $($completion.Count)." }
    $terminal = $completion[0]
    if ($terminal.status -ne "complete" -or [int]$terminal.requested -ne [int]$terminal.eligible -or
        [int]$terminal.pending_gpu -ne 0 -or [int]$terminal.dropped -ne 0) {
        throw "Entry $($Entry.sequence) compact capture did not close cleanly."
    }
    $requested = [int]$terminal.requested
    if ($loops.Count -ne $requested -or $gpu.Count -ne $requested -or $workloads.Count -ne $requested) {
        throw "Entry $($Entry.sequence) expected $requested loop/GPU/workload rows; found $($loops.Count)/$($gpu.Count)/$($workloads.Count)."
    }

    $gpuBySample = @{}
    foreach ($row in $gpu) {
        $sample = [int]$row.sample
        if ($gpuBySample.ContainsKey($sample)) { throw "Duplicate GPU sample $sample in entry $($Entry.sequence)." }
        if ([int]$row.invalid -ne 0 -or [int]$row.dropped -ne 0 -or [int]$row.expected -ne [int]$row.resolved -or
            [double]$row.segment -le 0.0 -or [double]$row.scene -le 0.0 -or [double]$row.trace_dispatch -le 0.0) {
            throw "Invalid primary GPU timing row at sample $sample in entry $($Entry.sequence)."
        }
        $gpuBySample[$sample] = $row
    }
    foreach ($sample in 0..($requested - 1)) {
        if (-not $gpuBySample.ContainsKey($sample)) { throw "Missing GPU sample $sample in entry $($Entry.sequence)." }
    }

    $loopBySample = @{}
    $loopSampleByPresentation = @{}
    foreach ($row in $loops) {
        $sample = [int]$row.sample
        if ($loopBySample.ContainsKey($sample)) { throw "Duplicate loop sample $sample in entry $($Entry.sequence)." }
        if (-not $gpuBySample.ContainsKey($sample) -or [uint64]$row.frame -ne [uint64]$gpuBySample[$sample].frame) {
            throw "Loop/GPU join failed at sample $sample in entry $($Entry.sequence)."
        }
        $presentationIdentity = "$([uint64]$row.epoch)/$([uint64]$row.presentation_gen)"
        if ($loopSampleByPresentation.ContainsKey($presentationIdentity)) {
            throw "Duplicate loop presentation identity $presentationIdentity in entry $($Entry.sequence)."
        }
        $loopBySample[$sample] = $row
        $loopSampleByPresentation[$presentationIdentity] = $sample
    }

    $workloadBySample = @{}
    $settingsKeys = New-Object System.Collections.Generic.List[string]
    $workloadKeys = New-Object System.Collections.Generic.List[string]
    $shapes = New-Object System.Collections.Generic.List[string]
    $baseShapes = New-Object System.Collections.Generic.List[string]
    foreach ($row in $workloads) {
        $sample = [int]$row.sample
        if ($workloadBySample.ContainsKey($sample)) { throw "Duplicate workload sample $sample in entry $($Entry.sequence)." }
        if ([int]$row.schema -lt $minimumSchema) { throw "Workload schema $($row.schema) is below required $minimumSchema in entry $($Entry.sequence)." }
        if ([int]$row.schema -ge 3) {
            foreach ($field in @("voxel_occurrences", "voxel_instance_prims", "voxel_occurrence_control")) {
                if (-not $row.ContainsKey($field) -or [uint64]$row[$field] -lt 0) { throw "Schema-3 workload is missing '$field'." }
            }
            $occurrences = [uint64]$row.voxel_occurrences
            $instancePrims = [uint64]$row.voxel_instance_prims
            if ([string]$matrix.occurrenceMode -eq "omit" -and ($occurrences -ne 0 -or $instancePrims -ne 0)) {
                throw "Omit-occurrence workload retained voxel occurrences at sample $sample."
            }
            if ([string]$matrix.occurrenceMode -eq "full" -and ($occurrences -eq 0 -or $instancePrims -eq 0)) {
                throw "Full-occurrence workload has no voxel occurrences at sample $sample."
            }
        }
        Assert-ExpectedTrace -Row $row -Expected $matrix.expectedTrace -Context "entry $($Entry.sequence) sample $sample"
        if (-not $gpuBySample.ContainsKey($sample) -or [uint64]$row.frame -ne [uint64]$gpuBySample[$sample].frame -or
            [uint64]$row.nri_frame -ne [uint64]$gpuBySample[$sample].nri_frame) {
            throw "Workload/GPU join failed at sample $sample in entry $($Entry.sequence)."
        }
        $workloadBySample[$sample] = $row
        $settingsKeys.Add([string]$row.settings_key)
        $workloadKeys.Add([string]$row.workload_key)
        $shapes.Add((Get-WorkloadShape -Row $row -IncludeOccurrence))
        $baseShapes.Add((Get-WorkloadShape -Row $row))
    }

    $voxelBySample = @{}
    foreach ($row in $voxelGpu) {
        if (-not $row.ContainsKey("record") -or -not $row.ContainsKey("presentation_gen") -or -not $row.ContainsKey("epoch")) {
            throw "Voxel GPU timing row is missing deferred identity in entry $($Entry.sequence)."
        }
        $record = [int]$row.record
        if ([uint64]$row.epoch -ne [uint64]$terminal.epoch) {
            throw "Voxel GPU timing row at record $record has the wrong capture epoch in entry $($Entry.sequence)."
        }
        $presentationIdentity = "$([uint64]$row.epoch)/$([uint64]$row.presentation_gen)"
        if (-not $loopSampleByPresentation.ContainsKey($presentationIdentity)) {
            continue
        }
        $sample = [int]$loopSampleByPresentation[$presentationIdentity]
        if ($voxelBySample.ContainsKey($sample)) { throw "Duplicate voxel timing sample $sample in entry $($Entry.sequence)." }
        if (-not $row.ContainsKey("queued_slot") -or [uint64]$row.presentation_gen -eq 0 -or [int]$row.segment_valid -ne 1 -or
            [int]$row.invalid -ne 0 -or [int]$row.dropped -ne 0 -or [int]$row.scopes -ne ([int]$row.valid + [int]$row.invalid)) {
            throw "Invalid voxel GPU timing row at record $record in entry $($Entry.sequence)."
        }
        if (-not $gpuBySample.ContainsKey($sample) -or -not $workloadBySample.ContainsKey($sample) -or
            [Math]::Abs([double]$row.segment - [double]$gpuBySample[$sample].segment) -gt 0.01) {
            throw "Voxel timing identity join failed at sample $sample (record $record) in entry $($Entry.sequence)."
        }
        $voxelBySample[$sample] = $row
    }
    $acceptedVoxelGpu = @(foreach ($sample in 0..($requested - 1)) {
        if (-not $voxelBySample.ContainsKey($sample)) { throw "Missing voxel timing for compact sample $sample in entry $($Entry.sequence)." }
        $voxelBySample[$sample]
    })

    $settingsIdentity = @(Get-ExactCounts -Values $settingsKeys.ToArray())
    $shapeIdentity = @(Get-ExactCounts -Values $shapes.ToArray())
    $baseShapeIdentity = @(Get-ExactCounts -Values $baseShapes.ToArray())
    if ($settingsIdentity.Count -ne 1 -or $shapeIdentity.Count -ne 1 -or $baseShapeIdentity.Count -ne 1) {
        throw "Entry $($Entry.sequence) did not retain one exact settings/workload-shape identity."
    }

    $voxelFields = @("admission", "upload", "arena_copy", "classify", "scan", "emit", "finalize", "voxel_blas", "world_tlas")
    $voxelStats = [ordered]@{}
    foreach ($field in $voxelFields) {
        $voxelStats[$field] = Get-Stats -Values ([double[]]@($acceptedVoxelGpu | ForEach-Object { [double]$_[$field] }))
    }
    $populationStats = [ordered]@{}
    foreach ($field in @("runtime_lights", "light_tile_indices", "light_tile_max", "emissive_prims", "emissive_power")) {
        $populationStats[$field] = Get-Stats -Values ([double[]]@($workloads | ForEach-Object { [double]$_[$field] }))
    }
    return [pscustomobject]@{
        sequence = [int]$Entry.sequence
        cycle = [int]$Entry.cycle
        profile = [string]$Entry.profile
        leg = [string]$Entry.leg
        traceClass = [string]$matrix.traceClass
        occurrenceMode = [string]$matrix.occurrenceMode
        samples = $requested
        targetFrameMs = $targetFrameMs
        settingsKeys = $settingsIdentity
        workloadKeys = Get-ExactCounts -Values $workloadKeys.ToArray()
        workloadShape = $shapeIdentity[0].value
        baseWorkloadShape = $baseShapeIdentity[0].value
        completeGpu = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.segment })) -TargetMs $targetFrameMs
        scene = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.scene }))
        traceDispatch = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.trace_dispatch }))
        workloadPopulation = [pscustomobject]$populationStats
        voxelGpu = [pscustomobject]$voxelStats
        voxelGpuValidity = [pscustomobject]@{
            rows = $acceptedVoxelGpu.Count
            rawRows = $voxelGpu.Count
            unmatchedRows = $voxelGpu.Count - $acceptedVoxelGpu.Count
            scopes = (@($acceptedVoxelGpu | ForEach-Object { [int]$_.scopes }) | Measure-Object -Sum).Sum
            valid = (@($acceptedVoxelGpu | ForEach-Object { [int]$_.valid }) | Measure-Object -Sum).Sum
            invalid = (@($acceptedVoxelGpu | ForEach-Object { [int]$_.invalid }) | Measure-Object -Sum).Sum
            dropped = (@($acceptedVoxelGpu | ForEach-Object { [int]$_.dropped }) | Measure-Object -Sum).Sum
        }
        rawGpu = $gpu
        rawVoxelGpu = $acceptedVoxelGpu
        rawWorkloads = $workloads
    }
}

function Merge-Leg {
    param([object[]]$Runs, [string]$Profile, [string]$Leg)
    $selected = @($Runs | Where-Object { $_.profile -eq $Profile -and $_.leg -eq $Leg })
    if ($selected.Count -eq 0) { throw "Matrix has no '$Profile/$Leg' entries." }
    $settings = @($selected | ForEach-Object { $_.settingsKeys[0].value } | Sort-Object -Unique)
    $targets = @($selected | ForEach-Object { $_.targetFrameMs } | Sort-Object -Unique)
    $shapes = @($selected | ForEach-Object { $_.workloadShape } | Sort-Object -Unique)
    $baseShapes = @($selected | ForEach-Object { $_.baseWorkloadShape } | Sort-Object -Unique)
    if ($settings.Count -ne 1 -or $targets.Count -ne 1 -or $shapes.Count -ne 1 -or $baseShapes.Count -ne 1) { throw "The '$Profile/$Leg' runs changed target, settings, or workload-shape identity." }
    $gpu = @($selected | ForEach-Object { $_.rawGpu })
    $voxel = @($selected | ForEach-Object { $_.rawVoxelGpu })
    $voxelStats = [ordered]@{}
    foreach ($field in @("admission", "upload", "arena_copy", "classify", "scan", "emit", "finalize", "voxel_blas", "world_tlas")) {
        $voxelStats[$field] = Get-Stats -Values ([double[]]@($voxel | ForEach-Object { [double]$_[$field] }))
    }
    $populationStats = [ordered]@{}
    foreach ($field in @("runtime_lights", "light_tile_indices", "light_tile_max", "emissive_prims", "emissive_power")) {
        $populationStats[$field] = Get-Stats -Values ([double[]]@($selected | ForEach-Object { $_.rawWorkloads } | ForEach-Object { [double]$_[$field] }))
    }
    return [pscustomobject]@{
        profile = $Profile
        leg = $Leg
        runs = $selected.Count
        samples = $gpu.Count
        targetFrameMs = [double]$targets[0]
        settingsKey = $settings[0]
        workloadKeys = Get-ExactCounts -Values ([string[]]@($selected | ForEach-Object { $_.workloadKeys | ForEach-Object { $_.value } }))
        workloadShape = $shapes[0]
        baseWorkloadShape = $baseShapes[0]
        completeGpu = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.segment })) -TargetMs ([double]$targets[0])
        scene = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.scene }))
        traceDispatch = Get-Stats -Values ([double[]]@($gpu | ForEach-Object { [double]$_.trace_dispatch }))
        workloadPopulation = [pscustomobject]$populationStats
        voxelGpu = [pscustomobject]$voxelStats
        voxelGpuValidity = [pscustomobject]@{
            rows = $voxel.Count
            scopes = (@($voxel | ForEach-Object { [int]$_.scopes }) | Measure-Object -Sum).Sum
            valid = (@($voxel | ForEach-Object { [int]$_.valid }) | Measure-Object -Sum).Sum
            invalid = (@($voxel | ForEach-Object { [int]$_.invalid }) | Measure-Object -Sum).Sum
            dropped = (@($voxel | ForEach-Object { [int]$_.dropped }) | Measure-Object -Sum).Sum
        }
    }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$runSummaries = @($manifest.entries | ForEach-Object { Read-Run -Entry $_ })
function Get-Delta {
    param([object]$Left, [object]$Right)
    $delta = [ordered]@{}
    foreach ($owner in @("completeGpu", "scene", "traceDispatch")) {
        $metrics = [ordered]@{}
        foreach ($metric in @("p50", "p95", "p99", "max")) {
            $metrics[$metric] = [Math]::Round([double]$Left.$owner.$metric - [double]$Right.$owner.$metric, 3)
        }
        $delta[$owner] = [pscustomobject]$metrics
    }
    return [pscustomobject]$delta
}

$profileSummaries = @($runSummaries.profile | Sort-Object -Unique | ForEach-Object {
    $profile = $_
    $defaultFull = Merge-Leg -Runs $runSummaries -Profile $profile -Leg "default-full"
    $candidateFull = Merge-Leg -Runs $runSummaries -Profile $profile -Leg "candidate-full"
    $defaultOmit = Merge-Leg -Runs $runSummaries -Profile $profile -Leg "default-omit"
    $candidateOmit = Merge-Leg -Runs $runSummaries -Profile $profile -Leg "candidate-omit"
    if ($defaultFull.workloadShape -ne $candidateFull.workloadShape -or $defaultOmit.workloadShape -ne $candidateOmit.workloadShape) {
        throw "Profile '$profile' changed workload shape between trace-setting legs."
    }
    $baseShapes = @($defaultFull.baseWorkloadShape, $candidateFull.baseWorkloadShape, $defaultOmit.baseWorkloadShape, $candidateOmit.baseWorkloadShape | Sort-Object -Unique)
    if (@($baseShapes | Sort-Object -Unique).Count -ne 1) { throw "Profile '$profile' changed base workload shape between occurrence legs." }
    if ($defaultFull.settingsKey -eq $candidateFull.settingsKey -or $defaultOmit.settingsKey -eq $candidateOmit.settingsKey) {
        throw "Profile '$profile' default/candidate settings keys are not distinct."
    }
    [pscustomobject]@{
        profile = $profile
        defaultFull = $defaultFull
        candidateFull = $candidateFull
        defaultOmit = $defaultOmit
        candidateOmit = $candidateOmit
        deltasMs = [pscustomobject]@{
            candidateMinusDefaultFull = Get-Delta -Left $candidateFull -Right $defaultFull
            candidateMinusDefaultOmit = Get-Delta -Left $candidateOmit -Right $defaultOmit
            fullMinusOmitDefault = Get-Delta -Left $defaultFull -Right $defaultOmit
            fullMinusOmitCandidate = Get-Delta -Left $candidateFull -Right $candidateOmit
        }
    }
})

$summary = [pscustomobject]@{
    ok = $true
    schema = 1
    manifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
    generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
    profiles = $profileSummaries
    runs = @($runSummaries | Select-Object -Property sequence, cycle, profile, leg, samples, settingsKeys, workloadKeys, completeGpu, scene, traceDispatch, workloadPopulation, voxelGpu, voxelGpuValidity)
    notes = @(
        "Voxel admission is an aggregate/nesting scope; stage fields are attribution and are not summed into admission.",
        "A zero voxel stage with a valid row means no scope for that stage in the captured record."
    )
}
$summaryDirectory = Split-Path -Parent $SummaryOutput
if ($summaryDirectory) { New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null }
$summary | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
Write-Host "Direct trace matrix complete: profiles=$($profileSummaries.Count) runs=$($runSummaries.Count) summary=$SummaryOutput"
