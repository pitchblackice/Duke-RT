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

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { throw "Cannot summarize an empty value set." }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
    return [double]$sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
}

function Get-FieldSummaries {
    param([object[]]$Rows, [string[]]$Exclude = @())
    if ($Rows.Count -eq 0) { return [pscustomobject]@{} }
    $result = [ordered]@{}
    foreach ($field in @($Rows[0].Keys | Sort-Object)) {
        if ($field -in $Exclude) { continue }
        $values = New-Object System.Collections.Generic.List[double]
        $numeric = $true
        foreach ($row in $Rows) {
            $value = 0.0
            if (-not $row.ContainsKey($field) -or
                -not [double]::TryParse([string]$row[$field], [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) {
                $numeric = $false
                break
            }
            $values.Add($value)
        }
        if (-not $numeric) { continue }
        $array = $values.ToArray()
        $result[$field] = [pscustomobject]@{
            samples = $array.Count
            p50 = [Math]::Round((Get-Percentile -Values $array -Percentile 0.50), 3)
            p95 = [Math]::Round((Get-Percentile -Values $array -Percentile 0.95), 3)
            total = [Math]::Round((($array | Measure-Object -Sum).Sum), 3)
        }
    }
    return [pscustomobject]$result
}

function Assert-ExpectedTrace {
    param([hashtable]$Row, [object]$Expected, [string]$Context)
    foreach ($property in $Expected.PSObject.Properties) {
        if (-not $Row.ContainsKey($property.Name)) { throw "$Context is missing '$($property.Name)'." }
        if ([string]$Row[$property.Name] -ne [string]$property.Value) {
            throw "$Context field '$($property.Name)' is '$($Row[$property.Name])', expected '$($property.Value)'."
        }
    }
}

function Get-RowKey {
    param([hashtable]$Row)
    if (-not $Row.ContainsKey("frame") -or -not $Row.ContainsKey("stats_frame")) {
        throw "Shader row is missing frame identity."
    }
    return "$($Row.frame)/$($Row.stats_frame)"
}

function Assert-MonotonicObserver {
    param([object[]]$Rows)
    $fields = @("copies", "recorded", "busy", "no_fence", "published", "superseded", "abandoned", "map_fail", "attribution_rows", "attribution_bytes")
    for ($index = 1; $index -lt $Rows.Count; ++$index) {
        foreach ($field in $fields) {
            if ([uint64]$Rows[$index][$field] -lt [uint64]$Rows[$index - 1][$field]) {
                throw "Shader observer field '$field' regressed at row $index."
            }
        }
    }
}

function Read-AttributionRun {
    param([object]$Entry)

    if ([int]$Entry.exitCode -ne 0) { throw "Attribution entry $($Entry.sequence) has exit code $($Entry.exitCode)." }
    if (-not (Test-Path -LiteralPath $Entry.logPath)) { throw "Missing attribution log: $($Entry.logPath)" }
    if (Test-Path -LiteralPath $Entry.baseSummaryPath) {
        $baseSummary = Get-Content -LiteralPath $Entry.baseSummaryPath -Raw | ConvertFrom-Json
        if (-not [bool]$baseSummary.ok) { throw "Base perf summary failed: $($Entry.baseSummaryPath)" }
    }

    $scenario = Get-Content -LiteralPath $Entry.scenarioPath -Raw | ConvertFrom-Json
    $metadata = $scenario.traceShaderAttribution
    if ([string]$metadata.leg -ne [string]$Entry.leg) { throw "Manifest/scenario leg mismatch for entry $($Entry.sequence)." }

    $rows = @{
        workload = New-Object System.Collections.Generic.List[object]
        observer = New-Object System.Collections.Generic.List[object]
        trace = New-Object System.Collections.Generic.List[object]
        reject = New-Object System.Collections.Generic.List[object]
        phase = New-Object System.Collections.Generic.List[object]
        emissive = New-Object System.Collections.Generic.List[object]
        hot = New-Object System.Collections.Generic.List[object]
        completion = New-Object System.Collections.Generic.List[object]
    }
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Entry.logPath).Path)) {
        if ($line.StartsWith("PERF pt trace workload NRI:", [StringComparison]::Ordinal) -and $line.Contains(" compact=1 ")) { $rows.workload.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader stats observer NRI:", [StringComparison]::Ordinal)) { $rows.observer.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader trace NRI:", [StringComparison]::Ordinal)) { $rows.trace.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader reject NRI:", [StringComparison]::Ordinal)) { $rows.reject.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader phase NRI:", [StringComparison]::Ordinal)) { $rows.phase.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader emissive detail NRI:", [StringComparison]::Ordinal)) { $rows.emissive.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF pt shader hot instance NRI:", [StringComparison]::Ordinal)) { $rows.hot.Add((Read-Pairs $line)) }
        elseif ($line.StartsWith("PERF compact capture complete:", [StringComparison]::Ordinal)) { $rows.completion.Add((Read-Pairs $line)) }
    }

    if ($rows.completion.Count -ne 1) { throw "Entry $($Entry.sequence) expected one compact completion row, found $($rows.completion.Count)." }
    $terminal = $rows.completion[0]
    $expectedCompact = [int]$metadata.compactIdentityFrames
    if ($terminal.status -ne "complete" -or [int]$terminal.requested -ne $expectedCompact -or
        [int]$terminal.eligible -ne $expectedCompact -or [int]$terminal.pending_gpu -ne 0 -or [int]$terminal.dropped -ne 0) {
        throw "Entry $($Entry.sequence) compact identity window did not close cleanly."
    }
    if ($rows.workload.Count -ne $expectedCompact) {
        throw "Entry $($Entry.sequence) expected $expectedCompact workload rows, found $($rows.workload.Count)."
    }
    foreach ($row in $rows.workload) {
        if ([int]$row.schema -lt 3) { throw "Entry $($Entry.sequence) requires workload schema 3." }
        Assert-ExpectedTrace -Row $row -Expected $metadata.expectedTrace -Context "entry $($Entry.sequence) workload sample $($row.sample)"
        if ([string]$metadata.leg -like "*-omit") {
            if ([uint64]$row.voxel_occurrences -ne 0 -or [uint64]$row.voxel_instance_prims -ne 0) {
                throw "Omit leg retained voxel occurrences at sample $($row.sample)."
            }
        }
        elseif ([uint64]$row.voxel_occurrences -eq 0 -or [uint64]$row.voxel_instance_prims -eq 0) {
            throw "Full leg has no voxel occurrences at sample $($row.sample)."
        }
    }
    $settingsKeys = @($rows.workload | ForEach-Object { [string]$_.settings_key } | Sort-Object -Unique)
    if ($settingsKeys.Count -ne 1) { throw "Entry $($Entry.sequence) changed settings identity inside the window." }

    if ($rows.observer.Count -eq 0) { throw "Entry $($Entry.sequence) has no shader observer rows." }
    Assert-MonotonicObserver -Rows $rows.observer.ToArray()
    $finalObserver = $rows.observer[$rows.observer.Count - 1]
    foreach ($field in @("copies", "recorded", "busy", "no_fence", "published", "superseded", "abandoned", "map_fail", "pending", "attribution_rows", "attribution_bytes")) {
        if (-not $finalObserver.ContainsKey($field)) { throw "Final observer row is missing '$field'." }
    }
    if ([uint64]$finalObserver.copies -ne [uint64]$finalObserver.recorded + [uint64]$finalObserver.busy + [uint64]$finalObserver.no_fence) {
        throw "Observer copy accounting does not close in entry $($Entry.sequence)."
    }
    if ([uint64]$finalObserver.recorded -ne [uint64]$finalObserver.published + [uint64]$finalObserver.superseded +
        [uint64]$finalObserver.abandoned + [uint64]$finalObserver.map_fail + [uint64]$finalObserver.pending) {
        throw "Observer readback accounting does not close in entry $($Entry.sequence)."
    }
    if ([uint64]$finalObserver.no_fence -ne 0 -or [uint64]$finalObserver.abandoned -ne 0 -or [uint64]$finalObserver.map_fail -ne 0) {
        throw "Entry $($Entry.sequence) has a no-fence, abandoned, or map-failure observer error."
    }
    if ([uint64]$finalObserver.published -lt [uint64]$metadata.minimumPublishedSnapshots) {
        throw "Entry $($Entry.sequence) published only $($finalObserver.published) shader snapshots."
    }
    if ([uint64]$finalObserver.attribution_rows -eq 0 -or [uint64]$finalObserver.attribution_bytes -eq 0 -or
        ([uint64]$finalObserver.attribution_bytes % [uint64]$finalObserver.attribution_rows) -ne 0) {
        throw "Entry $($Entry.sequence) has invalid copied attribution metadata accounting."
    }

    $families = @($rows.trace, $rows.reject, $rows.phase, $rows.emissive)
    $traceKeys = @($rows.trace | ForEach-Object { Get-RowKey $_ })
    if (@($traceKeys | Sort-Object -Unique).Count -ne $traceKeys.Count) { throw "Entry $($Entry.sequence) has duplicate shader trace snapshots." }
    foreach ($family in $families) {
        $keys = @($family | ForEach-Object { Get-RowKey $_ } | Sort-Object)
        if ($keys.Count -ne $traceKeys.Count -or (Compare-Object @($traceKeys | Sort-Object) $keys)) {
            throw "Entry $($Entry.sequence) shader row families do not join by frame/stats_frame."
        }
    }
    $validObserverKeys = @($rows.observer | Where-Object { [int]$_.valid -eq 1 } | ForEach-Object { Get-RowKey $_ } | Sort-Object)
    if ($validObserverKeys.Count -ne $traceKeys.Count -or (Compare-Object @($traceKeys | Sort-Object) $validObserverKeys)) {
        throw "Entry $($Entry.sequence) observer and shader snapshots do not join."
    }
    if ([uint64]$finalObserver.published -ne [uint64]$traceKeys.Count) {
        throw "Entry $($Entry.sequence) published count does not match shader snapshots."
    }
    if ($rows.hot.Count -eq 0) { throw "Entry $($Entry.sequence) has no hot-instance attribution rows." }
    foreach ($group in @($rows.hot | Group-Object { Get-RowKey $_ })) {
        $ranks = @($group.Group | ForEach-Object { [int]$_.rank } | Sort-Object)
        if ($ranks.Count -gt 8 -or ($ranks -join ',') -ne ((1..$ranks.Count) -join ',')) {
            throw "Entry $($Entry.sequence) has invalid hot-instance ranks for snapshot $($group.Name)."
        }
    }

    $sourceRows = @($rows.hot | Group-Object { [string]$_['source'] } | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{
            source = [string]$_.Name
            rows = $_.Count
            committed = [uint64](($_.Group | ForEach-Object { [uint64]$_.committed } | Measure-Object -Sum).Sum)
            accepted = [uint64](($_.Group | ForEach-Object { [uint64]$_.accepted } | Measure-Object -Sum).Sum)
            primary = [uint64](($_.Group | ForEach-Object { [uint64]$_.primary } | Measure-Object -Sum).Sum)
            emissive = [uint64](($_.Group | ForEach-Object { [uint64]$_.emissive } | Measure-Object -Sum).Sum)
        }
    })
    return [pscustomobject]@{
        sequence = [int]$Entry.sequence
        cycle = [int]$Entry.cycle
        leg = [string]$Entry.leg
        settingsKey = $settingsKeys[0]
        workloadKeys = @($rows.workload | ForEach-Object { [string]$_.workload_key } | Sort-Object -Unique)
        workloadSamples = $rows.workload.Count
        observer = [pscustomobject]@{
            rows = $rows.observer.Count
            copies = [uint64]$finalObserver.copies
            recorded = [uint64]$finalObserver.recorded
            busyDrops = [uint64]$finalObserver.busy
            noFenceDrops = [uint64]$finalObserver.no_fence
            published = [uint64]$finalObserver.published
            superseded = [uint64]$finalObserver.superseded
            abandoned = [uint64]$finalObserver.abandoned
            mapFailures = [uint64]$finalObserver.map_fail
            pending = [uint64]$finalObserver.pending
            attributionRows = [uint64]$finalObserver.attribution_rows
            attributionBytes = [uint64]$finalObserver.attribution_bytes
            attributionBytesPerRow = [uint64]$finalObserver.attribution_bytes / [uint64]$finalObserver.attribution_rows
            busyDropPercent = [Math]::Round(100.0 * [uint64]$finalObserver.busy / [Math]::Max(1.0, [uint64]$finalObserver.copies), 3)
        }
        snapshotCount = $rows.trace.Count
        trace = Get-FieldSummaries -Rows $rows.trace.ToArray() -Exclude @("frame", "stats_frame")
        reject = Get-FieldSummaries -Rows $rows.reject.ToArray() -Exclude @("frame", "stats_frame")
        phase = Get-FieldSummaries -Rows $rows.phase.ToArray() -Exclude @("frame", "stats_frame")
        emissiveDetail = Get-FieldSummaries -Rows $rows.emissive.ToArray() -Exclude @("frame", "stats_frame")
        hotSources = $sourceRows
        raw = [pscustomobject]@{
            trace = $rows.trace.ToArray()
            reject = $rows.reject.ToArray()
            phase = $rows.phase.ToArray()
            emissive = $rows.emissive.ToArray()
            hot = $rows.hot.ToArray()
        }
    }
}

function Merge-Leg {
    param([object[]]$Runs, [string]$Leg)
    $selected = @($Runs | Where-Object { $_.leg -eq $Leg })
    if ($selected.Count -eq 0) { throw "Attribution manifest has no '$Leg' entry." }
    $settings = @($selected.settingsKey | Sort-Object -Unique)
    $bytesPerRow = @($selected.observer.attributionBytesPerRow | Sort-Object -Unique)
    if ($settings.Count -ne 1 -or $bytesPerRow.Count -ne 1) { throw "Leg '$Leg' changed settings or metadata row size across runs." }
    $trace = @($selected | ForEach-Object { $_.raw.trace })
    $reject = @($selected | ForEach-Object { $_.raw.reject })
    $phase = @($selected | ForEach-Object { $_.raw.phase })
    $emissive = @($selected | ForEach-Object { $_.raw.emissive })
    $hot = @($selected | ForEach-Object { $_.raw.hot })
    return [pscustomobject]@{
        leg = $Leg
        runs = $selected.Count
        settingsKey = $settings[0]
        snapshotCount = $trace.Count
        observer = [pscustomobject]@{
            copies = [uint64](($selected.observer.copies | Measure-Object -Sum).Sum)
            recorded = [uint64](($selected.observer.recorded | Measure-Object -Sum).Sum)
            busyDrops = [uint64](($selected.observer.busyDrops | Measure-Object -Sum).Sum)
            noFenceDrops = [uint64](($selected.observer.noFenceDrops | Measure-Object -Sum).Sum)
            published = [uint64](($selected.observer.published | Measure-Object -Sum).Sum)
            superseded = [uint64](($selected.observer.superseded | Measure-Object -Sum).Sum)
            abandoned = [uint64](($selected.observer.abandoned | Measure-Object -Sum).Sum)
            mapFailures = [uint64](($selected.observer.mapFailures | Measure-Object -Sum).Sum)
            pending = [uint64](($selected.observer.pending | Measure-Object -Sum).Sum)
            attributionRows = [uint64](($selected.observer.attributionRows | Measure-Object -Sum).Sum)
            attributionBytes = [uint64](($selected.observer.attributionBytes | Measure-Object -Sum).Sum)
            attributionBytesPerRow = [uint64]$bytesPerRow[0]
        }
        trace = Get-FieldSummaries -Rows $trace -Exclude @("frame", "stats_frame")
        reject = Get-FieldSummaries -Rows $reject -Exclude @("frame", "stats_frame")
        phase = Get-FieldSummaries -Rows $phase -Exclude @("frame", "stats_frame")
        emissiveDetail = Get-FieldSummaries -Rows $emissive -Exclude @("frame", "stats_frame")
        hotSources = @($hot | Group-Object { [string]$_['source'] } | Sort-Object Name | ForEach-Object {
            [pscustomobject]@{
                source = [string]$_.Name
                rows = $_.Count
                committed = [uint64](($_.Group | ForEach-Object { [uint64]$_.committed } | Measure-Object -Sum).Sum)
                accepted = [uint64](($_.Group | ForEach-Object { [uint64]$_.accepted } | Measure-Object -Sum).Sum)
            }
        })
    }
}

function Get-P50Delta {
    param([object]$Left, [object]$Right)
    $result = [ordered]@{}
    foreach ($family in @("trace", "reject", "phase", "emissiveDetail")) {
        $fields = [ordered]@{}
        foreach ($property in $Left.$family.PSObject.Properties) {
            if ($null -ne $Right.$family.PSObject.Properties[$property.Name]) {
                $fields[$property.Name] = [Math]::Round([double]$property.Value.p50 - [double]$Right.$family.$($property.Name).p50, 3)
            }
        }
        $result[$family] = [pscustomobject]$fields
    }
    return [pscustomobject]$result
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$runSummaries = @($manifest.entries | ForEach-Object { Read-AttributionRun -Entry $_ })
$legNames = @($runSummaries.leg | Sort-Object -Unique)
$legs = @($legNames | ForEach-Object { Merge-Leg -Runs $runSummaries -Leg $_ })
$byName = @{}
foreach ($leg in $legs) { $byName[$leg.leg] = $leg }
if (-not $byName.ContainsKey("default-full") -or -not $byName.ContainsKey("default-omit")) {
    throw "Attribution comparison requires default-full and default-omit legs."
}

$comparisons = [ordered]@{
    defaultFullMinusDefaultOmit = Get-P50Delta -Left $byName["default-full"] -Right $byName["default-omit"]
}
if ($byName.ContainsKey("candidate-full")) {
    $comparisons.candidateFullMinusDefaultFull = Get-P50Delta -Left $byName["candidate-full"] -Right $byName["default-full"]
}

$summary = [pscustomobject]@{
    ok = $true
    schema = 1
    generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
    manifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
    runCommand = [string]$manifest.runCommand
    legs = $legs
    comparisonsP50 = [pscustomobject]$comparisons
    runs = @($runSummaries | Select-Object sequence, cycle, leg, settingsKey, workloadKeys, workloadSamples, observer, snapshotCount, hotSources)
    notes = @(
        "Shader counters are workload attribution, not GPU durations; use the stats-disabled authority matrix for GPU timing.",
        "Busy and superseded readbacks are reported rather than hidden; no-fence, abandoned, and map failures are hard failures.",
        "Attribution metadata is copied per recorded snapshot and its cumulative row/byte cost is reported."
    )
}
$summaryDirectory = Split-Path -Parent $SummaryOutput
if ($summaryDirectory) { New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null }
$summary | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
Write-Host "Trace shader attribution complete: legs=$($legs.Count) runs=$($runSummaries.Count) summary=$SummaryOutput"
