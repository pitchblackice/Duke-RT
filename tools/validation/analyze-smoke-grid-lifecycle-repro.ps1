param(
    [Parameter(Mandatory = $true)][string[]]$LogPath,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

function Test-DrainClosure([hashtable]$Row, [string]$Context, [System.Collections.Generic.List[string]]$Errors) {
    try {
        $activeCounts = @([string]$Row.active -split '/')
        $activePing = [int](Get-UInt64 $Row 'active_ping' $Context)
        if ($activeCounts.Count -ne 2 -or $activePing -notin @(0, 1)) { throw "$Context has invalid active tuple" }
        $effectiveActive = [uint64]$activeCounts[$activePing]
        $resident = Get-UInt64 $Row 'resident' $Context
        $free = Get-UInt64 $Row 'free' $Context
        $bricks = Get-UInt64 $Row 'bricks' $Context
        $allocated = Get-UInt64 $Row 'allocated' $Context
        $reclaimed = Get-UInt64 $Row 'reclaimed' $Context
        if ($Row.gpu_stats -ne 'valid') { $Errors.Add("$Context has invalid GPU stats") }
        if ($effectiveActive -ne 0) { $Errors.Add("$Context retained optically active bricks: $effectiveActive") }
        if ($resident -ne 0 -or $free -ne $bricks -or ($allocated - $reclaimed) -ne $resident) {
            $Errors.Add("$Context did not close resident/free accounting")
        }
        if ((Get-UInt64 $Row 'occupied' $Context) -ne 0 -or (Get-UInt64 $Row 'empty' $Context) -ne 0) {
            $Errors.Add("$Context retained occupied or empty topology")
        }
        if ([string]$Row.field_hash -ne '0000000000000000') { $Errors.Add("$Context retained a nonzero optical field hash") }
        foreach ($field in @('allocation_failures', 'probe_failures', 'deposition_rejected', 'rejected_mass_q', 'nan')) {
            if ((Get-UInt64 $Row $field $Context) -ne 0) { $Errors.Add("$Context reports $field") }
        }

        # Slice 2 adds exact hash gauges. Remain compatible with a pre-Slice-2
        # binary, but require complete closure as soon as those gauges exist.
        $hashFields = @('hash_empty', 'hash_tombstone', 'hash_resident', 'hash_new', 'hash_claimed', 'hash_invalid_state', 'hash_invalid_mapping')
        $present = @($hashFields | Where-Object { $Row.ContainsKey($_) })
        if ($present.Count -ne 0 -and $present.Count -ne $hashFields.Count) {
            $Errors.Add("$Context has an incomplete exact hash-health row")
        }
        if ($present.Count -eq $hashFields.Count) {
            $hashCapacity = Get-UInt64 $Row 'hash' $Context
            $empty = Get-UInt64 $Row 'hash_empty' $Context
            $tombstones = Get-UInt64 $Row 'hash_tombstone' $Context
            if ($empty + $tombstones -ne $hashCapacity) { $Errors.Add("$Context hash slots do not close to capacity") }
            foreach ($field in @('hash_resident', 'hash_new', 'hash_claimed', 'hash_invalid_state', 'hash_invalid_mapping')) {
                if ((Get-UInt64 $Row $field $Context) -ne 0) { $Errors.Add("$Context retained $field hash slots") }
            }
        }
    }
    catch { $Errors.Add($_.Exception.Message) }
}

$failurePattern = 'Device removed|DRED page fault|DRED breadcrumbs|DXGI_ERROR_DEVICE|device lost|QueueSubmit failed|NRI render failed|validation error|failed to create|assertion failed|Fatal error|NRI render crash|nan=[1-9]|inf=[1-9]|Unknown command'
$errors = [System.Collections.Generic.List[string]]::new()
$runSummaries = [System.Collections.Generic.List[object]]::new()
foreach ($path in $LogPath) {
    $resolved = Resolve-Path -LiteralPath $path -ErrorAction Stop
    $lines = @(Get-Content -LiteralPath $resolved.Path)
    $failures = @($lines | Where-Object { $_ -match $failurePattern })
    if ($failures.Count -ne 0) { $errors.Add("$($resolved.Path): runtime failure strings=$($failures.Count)") }
    $rows = @($lines | Where-Object { $_ -like 'NRI PT smoke grid status:*' } | ForEach-Object { ConvertFrom-StatusLine $_ })
    $validRows = @($rows | Where-Object { $_.gpu_stats -eq 'valid' -and $_.resources -eq 'ready' })
    if ($validRows.Count -lt 5) {
        $errors.Add("$($resolved.Path): expected five valid lifecycle snapshots, found $($validRows.Count)")
        continue
    }
    $cycle = @($validRows | Select-Object -Last 5)
    $cold, $activeOne, $drainedOne, $activeTwo, $drainedTwo = $cycle
    try {
        if ((Get-UInt64 $cold 'commands' 'cold snapshot') -ne 0 -or
            (Get-UInt64 $cold 'resident' 'cold snapshot') -ne 0 -or
            (Get-UInt64 $cold 'occupied' 'cold snapshot') -ne 0) {
            $errors.Add("$($resolved.Path): cold snapshot was not source-free")
        }
        Test-DrainClosure $cold 'cold snapshot' $errors
        $firstAllocated = Get-UInt64 $activeOne 'allocated' 'first active snapshot'
        $secondAllocated = Get-UInt64 $activeTwo 'allocated' 'second active snapshot'
        if ($firstAllocated -eq 0 -or (Get-UInt64 $activeOne 'resident' 'first active snapshot') -eq 0) {
            $errors.Add("$($resolved.Path): first injection did not create resident topology")
        }
        if ($secondAllocated -le (Get-UInt64 $drainedOne 'allocated' 'first drain snapshot') -or
            (Get-UInt64 $activeTwo 'resident' 'second active snapshot') -eq 0) {
            $errors.Add("$($resolved.Path): second injection did not allocate fresh topology without reset")
        }
        Test-DrainClosure $drainedOne 'first drain snapshot' $errors
        Test-DrainClosure $drainedTwo 'second drain snapshot' $errors
        if ((Get-UInt64 $drainedTwo 'allocated' 'second drain snapshot') -le (Get-UInt64 $drainedOne 'allocated' 'first drain snapshot')) {
            $errors.Add("$($resolved.Path): cumulative allocation did not advance in cycle two")
        }
        $runSummaries.Add([ordered]@{
            logPath = $resolved.Path
            statusSnapshots = $rows.Count
            validLifecycleSnapshots = $validRows.Count
            firstAllocated = $firstAllocated
            secondAllocated = $secondAllocated
            firstReclaimed = Get-UInt64 $drainedOne 'reclaimed' 'first drain snapshot'
            secondReclaimed = Get-UInt64 $drainedTwo 'reclaimed' 'second drain snapshot'
            failureMatches = $failures.Count
        })
    }
    catch { $errors.Add("$($resolved.Path): $($_.Exception.Message)") }
}

$summary = [ordered]@{
    runsRequested = $LogPath.Count
    runsAnalyzed = $runSummaries.Count
    runs = @($runSummaries)
    errors = @($errors)
    passed = $errors.Count -eq 0 -and $runSummaries.Count -eq $LogPath.Count
}
if ($SummaryOutput) {
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
}
$summary | ConvertTo-Json -Depth 6
if (-not $summary.passed) { exit 1 }
