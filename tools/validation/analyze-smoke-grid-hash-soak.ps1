param(
    [Parameter(Mandatory = $true)][string[]]$LogPath,
    [ValidateRange(1, 30)][int]$CycleSeconds = 30,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$checkpoints = @(30, 60, 120)

function ConvertFrom-StatusLine([string]$Line) {
    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $pairs
}

function Get-UInt64([hashtable]$Row, [string]$Name, [string]$Context) {
    if (-not $Row.ContainsKey($Name)) { throw "$Context is missing '$Name'" }
    $parsed = [uint64]0
    if (-not [uint64]::TryParse([string]$Row[$Name], [ref]$parsed)) {
        throw "$Context has nonnumeric '$Name=$($Row[$Name])'"
    }
    return $parsed
}

function Get-ProbeBins([hashtable]$Row, [string]$Context) {
    if (-not $Row.ContainsKey('control_probe_bins')) { throw "$Context is missing 'control_probe_bins'" }
    $parts = @([string]$Row.control_probe_bins -split '/')
    if ($parts.Count -ne 5) { throw "$Context has invalid control_probe_bins='$($Row.control_probe_bins)'" }
    return @($parts | ForEach-Object { [uint64]$_ })
}

function Get-Trend([object[]]$Values) {
    if ($Values.Count -lt 2) { return 'insufficient' }
    $allEqual = $true
    $nondecreasing = $true
    $increased = $false
    for ($i = 1; $i -lt $Values.Count; ++$i) {
        if ($Values[$i] -ne $Values[$i - 1]) { $allEqual = $false }
        if ($Values[$i] -lt $Values[$i - 1]) { $nondecreasing = $false }
        if ($Values[$i] -gt $Values[$i - 1]) { $increased = $true }
    }
    if ($allEqual) { return 'stable-exact' }
    if ($nondecreasing -and $increased) { return 'monotonic-growth' }
    return 'non-monotonic'
}

function Test-HashAccounting([hashtable]$Row, [string]$Context, [System.Collections.Generic.List[string]]$Errors) {
    try {
        $hashCapacity = Get-UInt64 $Row 'hash' $Context
        $states = @('hash_empty', 'hash_claimed', 'hash_resident', 'hash_new', 'hash_tombstone', 'hash_invalid_state')
        [uint64]$sum = 0
        foreach ($field in $states) { $sum += Get-UInt64 $Row $field $Context }
        if ($sum -ne $hashCapacity) { $Errors.Add("$Context hash-state accounting is $sum/$hashCapacity") }
        if ((Get-UInt64 $Row 'hash_invalid_state' $Context) -ne 0 -or
            (Get-UInt64 $Row 'hash_invalid_mapping' $Context) -ne 0) {
            $Errors.Add("$Context reports invalid hash state or reverse mapping")
        }
        foreach ($field in @(
            'allocation_failures', 'probe_failures', 'lookup_probe_limit_failures',
            'insertion_probe_limit_failures', 'insertion_capacity_failures',
            'insertion_active_failures', 'reclaim_invalid_mapping_failures',
            'hash_rebuild_failures', 'nan'
        )) {
            if ((Get-UInt64 $Row $field $Context) -ne 0) { $Errors.Add("$Context reports $field") }
        }
        $resident = Get-UInt64 $Row 'resident' $Context
        $free = Get-UInt64 $Row 'free' $Context
        $bricks = Get-UInt64 $Row 'bricks' $Context
        $allocated = Get-UInt64 $Row 'allocated' $Context
        $reclaimed = Get-UInt64 $Row 'reclaimed' $Context
        if ($resident -ne 0 -or $free -ne $bricks -or ($allocated - $reclaimed) -ne $resident) {
            $Errors.Add("$Context did not reach drained resident/free closure")
        }
        if ((Get-UInt64 $Row 'hash_claimed' $Context) -ne 0 -or
            (Get-UInt64 $Row 'hash_resident' $Context) -ne 0 -or
            (Get-UInt64 $Row 'hash_new' $Context) -ne 0) {
            $Errors.Add("$Context retained live hash mappings after its drain interval")
        }
    }
    catch { $Errors.Add($_.Exception.Message) }
}

$failurePattern = 'Device removed|DRED page fault|DRED breadcrumbs|DXGI_ERROR_DEVICE|device lost|QueueSubmit failed|NRI render failed|validation error|failed to create|assertion failed|Fatal error|NRI render crash|nan=[1-9]|inf=[1-9]|Unknown command'
$errors = [System.Collections.Generic.List[string]]::new()
$runs = [System.Collections.Generic.List[object]]::new()

foreach ($path in $LogPath) {
    $resolved = Resolve-Path -LiteralPath $path -ErrorAction Stop
    $lines = @(Get-Content -LiteralPath $resolved.Path)
    $failures = @($lines | Where-Object { $_ -match $failurePattern })
    if ($failures.Count -ne 0) { $errors.Add("$($resolved.Path): runtime failure strings=$($failures.Count)") }
    $rows = @($lines | Where-Object { $_ -like 'NRI PT smoke grid status:*' } | ForEach-Object { ConvertFrom-StatusLine $_ })
    $validRows = @($rows | Where-Object { $_.gpu_stats -eq 'valid' -and $_.resources -eq 'ready' })
    if ($validRows.Count -lt 4) {
        $errors.Add("$($resolved.Path): expected baseline plus three valid soak snapshots, found $($validRows.Count)")
        continue
    }
    $selected = @($validRows | Select-Object -Last 4)
    $baseline = $selected[0]
    Test-HashAccounting $baseline 'baseline' $errors
    $previous = $baseline
    $previousSecond = 0
    $samples = [System.Collections.Generic.List[object]]::new()
    foreach ($index in 0..2) {
        $second = $checkpoints[$index]
        $row = $selected[$index + 1]
        $context = "$second-second checkpoint"
        Test-HashAccounting $row $context $errors
        try {
            $intervalSeconds = $second - $previousSecond
            $cycles = $intervalSeconds / $CycleSeconds
            $probeTotal = Get-UInt64 $row 'control_probe_total' $context
            $previousProbeTotal = Get-UInt64 $previous 'control_probe_total' 'previous checkpoint'
            if ($probeTotal -lt $previousProbeTotal) { throw "$context control probe total regressed" }
            $lookupTotal = Get-UInt64 $row 'lookup_probe_total' $context
            $previousLookupTotal = Get-UInt64 $previous 'lookup_probe_total' 'previous checkpoint'
            $insertionTotal = Get-UInt64 $row 'insertion_probe_total' $context
            $previousInsertionTotal = Get-UInt64 $previous 'insertion_probe_total' 'previous checkpoint'
            $bins = Get-ProbeBins $row $context
            $previousBins = Get-ProbeBins $previous 'previous checkpoint'
            $binDeltas = @()
            for ($bin = 0; $bin -lt 5; ++$bin) {
                if ($bins[$bin] -lt $previousBins[$bin]) { throw "$context probe bin $bin regressed" }
                $binDeltas += $bins[$bin] - $previousBins[$bin]
            }
            [uint64]$operationDelta = 0
            foreach ($value in $binDeltas) { $operationDelta += $value }
            $controlDelta = $probeTotal - $previousProbeTotal
            $averageProbe = if ($operationDelta -eq 0) { 0.0 } else { [double]$controlDelta / [double]$operationDelta }
            $samples.Add([ordered]@{
                seconds = $second
                intervalSeconds = $intervalSeconds
                cycles = $cycles
                hashCapacity = Get-UInt64 $row 'hash' $context
                hashEmpty = Get-UInt64 $row 'hash_empty' $context
                hashClaimed = Get-UInt64 $row 'hash_claimed' $context
                hashResident = Get-UInt64 $row 'hash_resident' $context
                hashNew = Get-UInt64 $row 'hash_new' $context
                hashTombstone = Get-UInt64 $row 'hash_tombstone' $context
                hashInvalidState = Get-UInt64 $row 'hash_invalid_state' $context
                hashInvalidMapping = Get-UInt64 $row 'hash_invalid_mapping' $context
                controlProbeTotal = $probeTotal
                controlProbeDelta = $controlDelta
                controlProbeDeltaPerCycle = [double]$controlDelta / $cycles
                lookupProbeTotal = $lookupTotal
                lookupProbeDelta = $lookupTotal - $previousLookupTotal
                insertionProbeTotal = $insertionTotal
                insertionProbeDelta = $insertionTotal - $previousInsertionTotal
                probeBinTotals = $bins
                probeBinDeltas = $binDeltas
                probeOperationDelta = $operationDelta
                averageProbeLength = $averageProbe
                maximumProbe = Get-UInt64 $row 'max_probe' $context
                allocated = Get-UInt64 $row 'allocated' $context
                reclaimed = Get-UInt64 $row 'reclaimed' $context
                hashRebuildAttempts = Get-UInt64 $row 'hash_rebuild_attempts' $context
                hashRebuildSuccesses = Get-UInt64 $row 'hash_rebuild_successes' $context
                hashRebuildFailures = Get-UInt64 $row 'hash_rebuild_failures' $context
            })
            $previous = $row
            $previousSecond = $second
        }
        catch { $errors.Add("$($resolved.Path): $($_.Exception.Message)") }
    }
    if ($samples.Count -eq 3) {
        $tombstoneTrend = Get-Trend @($samples | ForEach-Object { $_.hashTombstone })
        $averageProbeTrend = Get-Trend @($samples | ForEach-Object { $_.averageProbeLength })
        $maximumProbeTrend = Get-Trend @($samples | ForEach-Object { $_.maximumProbe })
        $runs.Add([ordered]@{
            logPath = $resolved.Path
            statusSnapshots = $rows.Count
            validStatusSnapshots = $validRows.Count
            samples = @($samples)
            classification = [ordered]@{
                tombstones = $tombstoneTrend
                averageProbeLength = $averageProbeTrend
                maximumProbe = $maximumProbeTrend
                stable = $tombstoneTrend -eq 'stable-exact' -and
                    $averageProbeTrend -eq 'stable-exact' -and $maximumProbeTrend -eq 'stable-exact'
                monotonicGrowthObserved = $tombstoneTrend -eq 'monotonic-growth' -or
                    $averageProbeTrend -eq 'monotonic-growth' -or $maximumProbeTrend -eq 'monotonic-growth'
                interpretation = 'Evidence only; no automatic hash rebuild threshold is selected by this analyzer.'
            }
            failureMatches = $failures.Count
        })
    }
}

$summary = [ordered]@{
    checkpointsSeconds = $checkpoints
    cycleSeconds = $CycleSeconds
    runsRequested = $LogPath.Count
    runsAnalyzed = $runs.Count
    runs = @($runs)
    integrityPassed = $errors.Count -eq 0 -and $runs.Count -eq $LogPath.Count
    errors = @($errors)
    rebuildPolicy = 'none-inferred'
}
if ($SummaryOutput) {
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
}
$summary | ConvertTo-Json -Depth 8
if (-not $summary.integrityPassed) { exit 1 }
