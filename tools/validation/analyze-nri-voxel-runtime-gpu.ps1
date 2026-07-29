[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [double]$TargetMs = 16.667,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($TargetMs -le 0.0) { throw 'TargetMs must be positive.' }
if (-not (Test-Path -LiteralPath $LogPath)) { throw "Log not found: $LogPath" }

function Read-Pairs {
    param([string]$Line)

    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $pairs
}

function Read-Double {
    param([hashtable]$Row, [string]$Field, [string]$Context)

    if (-not $Row.ContainsKey($Field)) { throw "$Context is missing '$Field'." }
    $value = 0.0
    if (-not [double]::TryParse(
        [string]$Row[$Field],
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$value)) {
        throw "$Context has nonnumeric '$Field=$($Row[$Field])'."
    }
    return $value
}

function Read-UInt64 {
    param([hashtable]$Row, [string]$Field, [string]$Context)

    if (-not $Row.ContainsKey($Field)) { throw "$Context is missing '$Field'." }
    $value = [uint64]0
    if (-not [uint64]::TryParse([string]$Row[$Field], [ref]$value)) {
        throw "$Context has nonnumeric '$Field=$($Row[$Field])'."
    }
    return $value
}

function Get-Distribution {
    param([double[]]$Values)

    if ($Values.Count -eq 0) {
        return [pscustomobject]@{ samples = 0; p50 = $null; p95 = $null; p99 = $null; max = $null }
    }
    $sorted = @($Values | Sort-Object)
    function Pick([double]$Percentile) {
        $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
        return [double]$sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
    }
    return [pscustomobject]@{
        samples = $sorted.Count
        p50 = [Math]::Round((Pick 0.50), 6)
        p95 = [Math]::Round((Pick 0.95), 6)
        p99 = [Math]::Round((Pick 0.99), 6)
        max = [Math]::Round([double]$sorted[$sorted.Count - 1], 6)
    }
}

function Get-Counts {
    param([string[]]$Values)

    return @($Values | Group-Object | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ value = [string]$_.Name; count = $_.Count }
    })
}

function Get-OptionalDistribution {
    param([object[]]$Rows, [string]$Field)

    $values = @($Rows | Where-Object { $_.ContainsKey($Field) } | ForEach-Object {
        Read-Double -Row $_ -Field $Field -Context "admission summary"
    })
    return Get-Distribution -Values ([double[]]$values)
}

$resolvedLog = (Resolve-Path -LiteralPath $LogPath).Path
$completionRows = [System.Collections.Generic.List[hashtable]]::new()
$gpuRows = [System.Collections.Generic.List[hashtable]]::new()
$voxelRows = [System.Collections.Generic.List[hashtable]]::new()
$workloadRows = [System.Collections.Generic.List[hashtable]]::new()
$admissionRows = [System.Collections.Generic.List[hashtable]]::new()
$runtimeTailRows = [System.Collections.Generic.List[hashtable]]::new()

foreach ($line in [System.IO.File]::ReadLines($resolvedLog)) {
    if ($line.StartsWith('PERF compact capture complete:', [StringComparison]::Ordinal)) {
        $completionRows.Add((Read-Pairs $line))
    }
    elseif ($line.StartsWith('PERF pt gpu timing NRI:', [StringComparison]::Ordinal) -and $line.Contains(' compact=1 ')) {
        $gpuRows.Add((Read-Pairs $line))
    }
    elseif ($line.StartsWith('PERF pt voxel gpu timing NRI:', [StringComparison]::Ordinal) -and $line.Contains(' compact=1 ')) {
        $voxelRows.Add((Read-Pairs $line))
    }
    elseif ($line.StartsWith('PERF pt trace workload NRI:', [StringComparison]::Ordinal) -and $line.Contains(' compact=1 ')) {
        $workloadRows.Add((Read-Pairs $line))
    }
    elseif ($line.StartsWith('PERF pt voxel admission summary NRI:', [StringComparison]::Ordinal)) {
        $admissionRows.Add((Read-Pairs $line))
    }
    elseif ($line.StartsWith('PERF pt voxel runtime tail NRI:', [StringComparison]::Ordinal)) {
        $runtimeTailRows.Add((Read-Pairs $line))
    }
}

if ($completionRows.Count -ne 1) {
    throw "Expected exactly one compact completion row, found $($completionRows.Count)."
}
$completion = $completionRows[0]
$requested = [int](Read-UInt64 $completion 'requested' 'compact completion')
$eligible = [int](Read-UInt64 $completion 'eligible' 'compact completion')
$epoch = Read-UInt64 $completion 'epoch' 'compact completion'
$errors = [System.Collections.Generic.List[string]]::new()
if ([string]$completion.status -ne 'complete') { $errors.Add("compact status is '$($completion.status)'") }
if ($requested -ne $eligible) { $errors.Add("compact requested/eligible mismatch: $requested/$eligible") }
$completionPending = Read-UInt64 $completion 'pending_gpu' 'compact completion'
$completionDropped = Read-UInt64 $completion 'dropped' 'compact completion'
if ($completionPending -ne 0) { $errors.Add("compact pending_gpu is $completionPending") }
if ($completionDropped -ne 0) { $errors.Add("compact dropped is $completionDropped") }
if ($gpuRows.Count -ne $requested) { $errors.Add("expected $requested complete-GPU rows, found $($gpuRows.Count)") }
if ($voxelRows.Count -ne $requested) { $errors.Add("expected $requested voxel-GPU rows, found $($voxelRows.Count)") }
if ($workloadRows.Count -ne $requested) { $errors.Add("expected $requested trace-workload rows, found $($workloadRows.Count)") }

$gpuBySample = @{}
$primaryInvalid = [uint64]0
$primaryDropped = [uint64]0
$primaryExpected = [uint64]0
$primaryResolved = [uint64]0
foreach ($row in $gpuRows) {
    $sample = [int](Read-UInt64 $row 'sample' 'complete-GPU row')
    if ($gpuBySample.ContainsKey($sample)) { throw "Duplicate complete-GPU sample $sample." }
    if ((Read-UInt64 $row 'epoch' "complete-GPU sample $sample") -ne $epoch) {
        $errors.Add("complete-GPU sample $sample has the wrong epoch")
    }
    $primaryInvalid += Read-UInt64 $row 'invalid' "complete-GPU sample $sample"
    $primaryDropped += Read-UInt64 $row 'dropped' "complete-GPU sample $sample"
    $primaryExpected += Read-UInt64 $row 'expected' "complete-GPU sample $sample"
    $primaryResolved += Read-UInt64 $row 'resolved' "complete-GPU sample $sample"
    $null = Read-Double $row 'segment' "complete-GPU sample $sample"
    $gpuBySample[$sample] = $row
}
if ($primaryInvalid -ne 0) { $errors.Add("complete-GPU invalid timestamps total $primaryInvalid") }
if ($primaryDropped -ne 0) { $errors.Add("complete-GPU dropped timestamps total $primaryDropped") }
if ($primaryExpected -ne $primaryResolved) {
    $errors.Add("complete-GPU expected/resolved mismatch: $primaryExpected/$primaryResolved")
}

$workloadBySample = @{}
$sampleByRuntimeFrame = @{}
foreach ($row in $workloadRows) {
    $sample = [int](Read-UInt64 $row 'sample' 'trace-workload row')
    if ($workloadBySample.ContainsKey($sample)) { throw "Duplicate trace-workload sample $sample." }
    if ((Read-UInt64 $row 'epoch' "trace-workload sample $sample") -ne $epoch) {
        $errors.Add("trace-workload sample $sample has the wrong epoch")
    }
    $runtimeFrame = Read-UInt64 $row 'renderer_frame' "trace-workload sample $sample"
    if ($sampleByRuntimeFrame.ContainsKey([string]$runtimeFrame)) {
        throw "Duplicate trace-workload renderer_frame $runtimeFrame."
    }
    if (-not $gpuBySample.ContainsKey($sample)) {
        $errors.Add("trace-workload sample $sample has no complete-GPU sample")
    }
    elseif ((Read-UInt64 $row 'frame' "trace-workload sample $sample") -ne
            (Read-UInt64 $gpuBySample[$sample] 'frame' "complete-GPU sample $sample") -or
        (Read-UInt64 $row 'nri_frame' "trace-workload sample $sample") -ne
            (Read-UInt64 $gpuBySample[$sample] 'nri_frame' "complete-GPU sample $sample")) {
        $errors.Add("trace-workload/complete-GPU identity mismatch at sample $sample")
    }
    $workloadBySample[$sample] = $row
    $sampleByRuntimeFrame[[string]$runtimeFrame] = $sample
}

$stageFields = @('admission', 'upload', 'arena_copy', 'classify', 'scan', 'emit', 'finalize', 'voxel_blas', 'world_tlas')
$voxelByRecord = @{}
$voxelByRendererFrame = @{}
$voxelSegmentInvalidRows = 0
$voxelScopes = [uint64]0
$voxelValid = [uint64]0
$voxelInvalid = [uint64]0
$voxelDropped = [uint64]0
foreach ($row in $voxelRows) {
    $record = [int](Read-UInt64 $row 'record' 'voxel-GPU row')
    if ($voxelByRecord.ContainsKey($record)) { throw "Duplicate voxel-GPU record $record." }
    if ((Read-UInt64 $row 'epoch' "voxel-GPU record $record") -ne $epoch) {
        $errors.Add("voxel-GPU record $record has the wrong epoch")
    }
    $rendererFrame = Read-UInt64 $row 'renderer_frame' "voxel-GPU record $record"
    if ($voxelByRendererFrame.ContainsKey([string]$rendererFrame)) {
        throw "Duplicate voxel-GPU renderer_frame $rendererFrame."
    }
    $segmentValid = Read-UInt64 $row 'segment_valid' "voxel-GPU record $record"
    if ($segmentValid -ne 1) { $voxelSegmentInvalidRows++ }
    $voxelScopes += Read-UInt64 $row 'scopes' "voxel-GPU record $record"
    $voxelValid += Read-UInt64 $row 'valid' "voxel-GPU record $record"
    $voxelInvalid += Read-UInt64 $row 'invalid' "voxel-GPU record $record"
    $voxelDropped += Read-UInt64 $row 'dropped' "voxel-GPU record $record"
    foreach ($field in $stageFields) { $null = Read-Double $row $field "voxel-GPU record $record" }
    $voxelSegment = Read-Double $row 'segment' "voxel-GPU record $record"
    if (-not $gpuBySample.ContainsKey($record)) {
        $errors.Add("voxel-GPU record $record has no complete-GPU sample")
    }
    elseif ([Math]::Abs($voxelSegment - (Read-Double $gpuBySample[$record] 'segment' "complete-GPU sample $record")) -gt 0.01) {
        $errors.Add("voxel/complete segment mismatch at record $record")
    }
    $voxelByRecord[$record] = $row
    $voxelByRendererFrame[[string]$rendererFrame] = $row
}
if ($requested -gt 0) {
    foreach ($record in 0..($requested - 1)) {
        if (-not $gpuBySample.ContainsKey($record)) { $errors.Add("missing complete-GPU sample $record") }
        if (-not $voxelByRecord.ContainsKey($record)) { $errors.Add("missing voxel-GPU record $record") }
        if (-not $workloadBySample.ContainsKey($record)) { $errors.Add("missing trace-workload sample $record") }
    }
}
if ($voxelSegmentInvalidRows -ne 0) { $errors.Add("voxel-GPU segment-invalid rows total $voxelSegmentInvalidRows") }
if ($voxelInvalid -ne 0) { $errors.Add("voxel-GPU invalid scopes total $voxelInvalid") }
if ($voxelDropped -ne 0) { $errors.Add("voxel-GPU dropped scopes total $voxelDropped") }
if ($voxelScopes -ne ($voxelValid + $voxelInvalid)) {
    $errors.Add("voxel-GPU scope reconciliation failed: scopes=$voxelScopes valid=$voxelValid invalid=$voxelInvalid")
}

$completeValues = [double[]]@($gpuRows | ForEach-Object { Read-Double $_ 'segment' 'complete-GPU row' })
$targetMissCount = @($completeValues | Where-Object { $_ -gt $TargetMs }).Count
$stageSummaries = [ordered]@{}
foreach ($field in $stageFields) {
    $allValues = [double[]]@($voxelRows | ForEach-Object { Read-Double $_ $field "voxel-GPU $field" })
    $nonzeroValues = [double[]]@($allValues | Where-Object { $_ -gt 0.0 })
    $stageSummaries[$field] = [pscustomobject]@{
        nonzeroEventCount = $nonzeroValues.Count
        allFrames = Get-Distribution $allValues
        nonzeroEvents = Get-Distribution $nonzeroValues
    }
}

$admissionWorkFields = @('uploaded', 'direct_requests', 'direct_ready', 'required_admitted', 'optional_admitted', 'blas_used')
$admissionWorkRows = @($admissionRows | Where-Object {
    $row = $_
    @($admissionWorkFields | Where-Object {
        $row.ContainsKey($_) -and (Read-Double $row $_ 'admission summary') -gt 0.0
    }).Count -ne 0
}).Count
$admissionStats = [ordered]@{}
foreach ($field in @('ms_used', 'queued', 'ready', 'deferred', 'failed', 'uploaded', 'bytes_pending', 'bytes_uploaded',
        'runtime_pending', 'direct_requests', 'direct_ready', 'direct_pending', 'direct_failures',
        'cpu_geometry_avoided', 'cpu_geometry_fallback', 'required_admitted', 'optional_admitted', 'blas_used')) {
    $admissionStats[$field] = Get-OptionalDistribution -Rows $admissionRows.ToArray() -Field $field
}

$runtimeActionCounts = Get-Counts ([string[]]@($runtimeTailRows | ForEach-Object {
    if ($_.ContainsKey('action')) { [string]$_.action } else { '(missing)' }
}))
$runtimeFrames = @($runtimeTailRows | Where-Object { $_.ContainsKey('frame') } | ForEach-Object {
    [uint64]$_.frame
} | Sort-Object -Unique)
$matchedRuntimeFrames = 0
$runtimeFramesWithVoxelWork = 0
$runtimeFrameTargetMisses = 0
$runtimeStageCorrelations = [ordered]@{}
foreach ($field in $stageFields) { $runtimeStageCorrelations[$field] = 0 }
foreach ($frame in $runtimeFrames) {
    if (-not $sampleByRuntimeFrame.ContainsKey([string]$frame)) { continue }
    $sample = [int]$sampleByRuntimeFrame[[string]$frame]
    if (-not $voxelByRecord.ContainsKey($sample)) { continue }
    $matchedRuntimeFrames++
    $row = $voxelByRecord[$sample]
    $hasWork = $false
    foreach ($field in $stageFields) {
        if ((Read-Double $row $field "voxel-GPU frame $frame") -gt 0.0) {
            $runtimeStageCorrelations[$field]++
            $hasWork = $true
        }
    }
    if ($hasWork) { $runtimeFramesWithVoxelWork++ }
    if ((Read-Double $row 'segment' "voxel-GPU frame $frame") -gt $TargetMs) { $runtimeFrameTargetMisses++ }
}
if ($runtimeTailRows.Count -eq 0) { $errors.Add('no runtime-tail telemetry rows were found') }
if ($matchedRuntimeFrames -ne $runtimeFrames.Count) {
    $errors.Add("runtime-tail/workload frame bridge matched $matchedRuntimeFrames/$($runtimeFrames.Count) unique event frames")
}

$latencyStats = [ordered]@{}
foreach ($field in @('request_to_ready', 'ready_to_blas', 'blas_to_publish', 'publish_to_tlas', 'request_to_tlas')) {
    $latencyStats[$field] = Get-OptionalDistribution -Rows $runtimeTailRows.ToArray() -Field $field
}

$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    schema = 1
    logPath = $resolvedLog
    targetMs = [Math]::Round($TargetMs, 6)
    completeGpuMs = [pscustomobject]@{
        distribution = Get-Distribution $completeValues
        targetMissCount = $targetMissCount
        targetMissPercent = if ($completeValues.Count -ne 0) {
            [Math]::Round(100.0 * $targetMissCount / $completeValues.Count, 3)
        } else { $null }
    }
    stageTimingsMs = [pscustomobject]$stageSummaries
    timestampValidity = [pscustomobject]@{
        compactDropped = $completionDropped
        completeGpuRows = $gpuRows.Count
        completeGpuExpected = $primaryExpected
        completeGpuResolved = $primaryResolved
        completeGpuInvalid = $primaryInvalid
        completeGpuDropped = $primaryDropped
        voxelGpuRows = $voxelRows.Count
        voxelSegmentInvalidRows = $voxelSegmentInvalidRows
        voxelScopes = $voxelScopes
        voxelValid = $voxelValid
        voxelInvalid = $voxelInvalid
        voxelDropped = $voxelDropped
    }
    admissionTelemetry = [pscustomobject]@{
        rows = $admissionRows.Count
        workRows = $admissionWorkRows
        frameJoinAvailable = $false
        frameJoinReason = 'admission summary rows do not expose a frame key'
        phaseCounts = Get-Counts ([string[]]@($admissionRows | ForEach-Object {
            if ($_.ContainsKey('phase')) { [string]$_.phase } else { '(missing)' }
        }))
        stopReasonCounts = Get-Counts ([string[]]@($admissionRows | ForEach-Object {
            if ($_.ContainsKey('stop')) { [string]$_.stop } else { '(missing)' }
        }))
        fields = [pscustomobject]$admissionStats
    }
    runtimeTailCorrelation = [pscustomobject]@{
        rows = $runtimeTailRows.Count
        frameBridge = 'runtime-tail.frame -> trace-workload.renderer_frame -> compact sample/voxel record'
        workloadRows = $workloadRows.Count
        actionCounts = $runtimeActionCounts
        uniqueEventFrames = $runtimeFrames.Count
        matchedVoxelGpuFrames = $matchedRuntimeFrames
        unmatchedEventFrames = $runtimeFrames.Count - $matchedRuntimeFrames
        matchedFramesWithVoxelWork = $runtimeFramesWithVoxelWork
        completeGpuTargetMissesOnMatchedFrames = $runtimeFrameTargetMisses
        matchedFramesWithStageWork = [pscustomobject]$runtimeStageCorrelations
        latencyFrames = [pscustomobject]$latencyStats
    }
    notes = @(
        'Voxel admission is an aggregate scope that nests stage scopes; do not sum admission and child stages.',
        'All-frame distributions include zero when a valid frame recorded no scope for that stage.',
        'Runtime-tail event correlation uses trace-workload renderer-frame identity and its compact sample; voxel timing renderer_frame is a different backend frame domain.',
        'Admission summaries are aggregate-only because they have no frame key.'
    )
    errors = $errors.ToArray()
}

$json = $summary | ConvertTo-Json -Depth 16
if ($SummaryOutput) {
    $summaryParent = Split-Path -Parent $SummaryOutput
    if ($summaryParent) { New-Item -ItemType Directory -Force -Path $summaryParent | Out-Null }
    Set-Content -LiteralPath $SummaryOutput -Value $json -Encoding UTF8
}
$json
if (-not $summary.ok) { exit 1 }
