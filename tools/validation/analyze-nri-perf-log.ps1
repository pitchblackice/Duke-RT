param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [string]$ScenarioPath,

    [string]$SummaryOutput,

    [string]$CsvPath,

    [int]$MinLoopTraceSamples = 1,

    [int]$WorstFrameCount = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-ObjectProperty {
    param(
        [object]$Object,
        [string]$Name,
        [object]$Default = $null
    )

    if ($null -ne $Object -and $Object.PSObject.Properties.Name.Contains($Name)) {
        return $Object.$Name
    }
    return $Default
}

function ConvertTo-DoubleOrNull {
    param([object]$Value)

    if ($null -eq $Value) {
        return $null
    }

    $text = [string]$Value
    if ($text.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $number = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }
    return $null
}

function Read-KeyValuePairs {
    param([Parameter(Mandatory = $true)][string]$Text)

    $pairs = [ordered]@{}
    foreach ($match in [regex]::Matches($Text, '([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\([^\)]*\)|[^\s]+)')) {
        $value = $match.Groups[2].Value
        if ($value.Length -ge 2 -and $value[0] -eq '"' -and $value[$value.Length - 1] -eq '"') {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $pairs[$match.Groups[1].Value] = $value
    }
    return $pairs
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if ($Values.Count -eq 0) {
        return 0.0
    }
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 1) {
        return [double]$sorted[0]
    }
    $rank = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, [int]$rank))
    return [double]$sorted[$index]
}

function Get-Stats {
    param([double[]]$Values)

    if ($Values.Count -eq 0) {
        return [pscustomobject]@{
            samples = 0
            min = 0.0
            avg = 0.0
            p50 = 0.0
            p90 = 0.0
            p95 = 0.0
            p99 = 0.0
            max = 0.0
        }
    }

    $measure = $Values | Measure-Object -Average -Minimum -Maximum
    return [pscustomobject]@{
        samples = $Values.Count
        min = [Math]::Round([double]$measure.Minimum, 3)
        avg = [Math]::Round([double]$measure.Average, 3)
        p50 = [Math]::Round((Get-Percentile -Values $Values -Percentile 50), 3)
        p90 = [Math]::Round((Get-Percentile -Values $Values -Percentile 90), 3)
        p95 = [Math]::Round((Get-Percentile -Values $Values -Percentile 95), 3)
        p99 = [Math]::Round((Get-Percentile -Values $Values -Percentile 99), 3)
        max = [Math]::Round([double]$measure.Maximum, 3)
    }
}

function Count-ValuesOver {
    param(
        [double[]]$Values,
        [double]$Threshold
    )

    return @($Values | Where-Object { $_ -gt $Threshold }).Count
}

function Get-ContiguousRangesOver {
    param(
        [object[]]$Frames,
        [double]$Threshold
    )

    $ranges = New-Object System.Collections.Generic.List[object]
    $active = $null
    foreach ($frame in $Frames) {
        if ([double]$frame.frame_ms -gt $Threshold) {
            if ($null -eq $active) {
                $active = [ordered]@{
                    startFrame = [int64]$frame.frame
                    endFrame = [int64]$frame.frame
                    count = 1
                    maxMs = [double]$frame.frame_ms
                }
            }
            else {
                $active.endFrame = [int64]$frame.frame
                $active.count = [int]$active.count + 1
                $active.maxMs = [Math]::Max([double]$active.maxMs, [double]$frame.frame_ms)
            }
        }
        elseif ($null -ne $active) {
            $ranges.Add([pscustomobject]$active)
            $active = $null
        }
    }
    if ($null -ne $active) {
        $ranges.Add([pscustomobject]$active)
    }
    return $ranges.ToArray()
}

function Get-FieldValue {
    param(
        [hashtable]$FieldMap,
        [string]$Key,
        [string]$Property
    )

    if (-not $FieldMap.ContainsKey($Key)) {
        return $null
    }
    return Get-ObjectProperty -Object $FieldMap[$Key] -Name $Property
}

if (-not (Test-Path -LiteralPath $LogPath)) {
    throw "LogPath not found: $LogPath"
}

$scenario = $null
if ($ScenarioPath) {
    $scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
    $capture = Get-ObjectProperty -Object $scenario -Name "capture"
    $scenarioLoopTraceFrames = Get-ObjectProperty -Object $capture -Name "loopTraceFrames"
    if ($MinLoopTraceSamples -le 1 -and $null -ne $scenarioLoopTraceFrames) {
        $MinLoopTraceSamples = [int]$scenarioLoopTraceFrames
    }
}

$requiredPrefixes = @("PERF loop trace:")
if ($null -ne $scenario -and $scenario.PSObject.Properties.Name.Contains("requiredPrefixes")) {
    $requiredPrefixes = @($scenario.requiredPrefixes)
}

$forbiddenPatterns = @(
    "Device removed",
    "device lost",
    "DXGI_ERROR_DEVICE",
    "DRED",
    "NRI render failed",
    "validation error",
    "failed to create",
    "assertion failed",
    "fatal error",
    "Failed to open savegame",
    "Failed to restore all objects in savegame",
    "Unknown object class",
    "Unknown code reference",
    "Unknown data reference",
    "NRI BeginCommandList blocked",
    "NRI SubmitWaitAndRestartCommandList failed"
)
if ($null -ne $scenario -and $scenario.PSObject.Properties.Name.Contains("forbiddenPatterns")) {
    $forbiddenPatterns = @($scenario.forbiddenPatterns)
}

$resolvedLog = Resolve-Path -LiteralPath $LogPath -ErrorAction Stop
$requiredHits = [ordered]@{}
foreach ($prefix in $requiredPrefixes) {
    $requiredHits[$prefix] = 0
}

$forbiddenHits = New-Object System.Collections.Generic.List[object]
$fieldSamples = @{}
$fieldMaxValues = @{}
$fieldMaxFrames = @{}
$loopFrames = New-Object System.Collections.Generic.List[object]
$recordsByFrame = @{}

$lineNumber = 0
$stream = [System.IO.File]::Open($resolvedLog.Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    $reader = [System.IO.StreamReader]::new($stream)
    try {
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) {
                break
            }
            $lineNumber++

            foreach ($prefix in $requiredPrefixes) {
                if ($line.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
                    $requiredHits[$prefix] = [int]$requiredHits[$prefix] + 1
                }
            }

            foreach ($pattern in $forbiddenPatterns) {
                if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $forbiddenHits.Add([pscustomobject]@{
                        line = $lineNumber
                        pattern = $pattern
                        text = $line
                    })
                    break
                }
            }

            if ($line -notmatch '^(PERF [^:]+):\s+(.*)$') {
                continue
            }

            $prefix = $Matches[1]
            $body = $Matches[2]
            if ($prefix -eq "PERF pt scene buffer upload domain NRI" -and $body -match '(?:^|\s)domain=([A-Za-z0-9_]+)') {
                $prefix = "$prefix/$($Matches[1])"
            }

            $pairs = Read-KeyValuePairs -Text $body
            $frame = $null
            if ($pairs.Contains("frame")) {
                $frameValue = ConvertTo-DoubleOrNull $pairs["frame"]
                if ($null -ne $frameValue) {
                    $frame = [int64]$frameValue
                    $frameKey = [string]$frame
                    if (-not $recordsByFrame.ContainsKey($frameKey)) {
                        $recordsByFrame[$frameKey] = @{}
                    }
                    $recordsByFrame[$frameKey][$prefix] = [pscustomobject]$pairs
                }
            }

            if ($prefix -eq "PERF loop trace" -and $pairs.Contains("frame_ms")) {
                $frameMs = ConvertTo-DoubleOrNull $pairs["frame_ms"]
                if ($null -ne $frameMs) {
                    $loopFrames.Add([pscustomobject]@{
                        frame = if ($null -ne $frame) { $frame } else { $null }
                        frame_ms = [Math]::Round($frameMs, 3)
                        line = $lineNumber
                    })
                }
            }

            foreach ($entry in $pairs.GetEnumerator()) {
                $number = ConvertTo-DoubleOrNull $entry.Value
                if ($null -eq $number) {
                    continue
                }

                $key = "$prefix/$($entry.Key)"
                if (-not $fieldSamples.ContainsKey($key)) {
                    $fieldSamples[$key] = [System.Collections.Generic.List[double]]::new()
                }
                $fieldSamples[$key].Add([double]$number)

                if (-not $fieldMaxValues.ContainsKey($key) -or [double]$number -gt [double]$fieldMaxValues[$key]) {
                    $fieldMaxValues[$key] = [double]$number
                    $fieldMaxFrames[$key] = if ($null -ne $frame) { $frame } else { $null }
                }
            }
        }
    }
    finally {
        $reader.Dispose()
    }
}
finally {
    $stream.Dispose()
}

$fieldRows = foreach ($entry in $fieldSamples.GetEnumerator()) {
    $values = [double[]]$entry.Value.ToArray()
    $slash = $entry.Key.LastIndexOf("/")
    $prefix = if ($slash -gt 0) { $entry.Key.Substring(0, $slash) } else { "" }
    $field = if ($slash -gt 0) { $entry.Key.Substring($slash + 1) } else { $entry.Key }
    $stats = Get-Stats -Values $values
    [pscustomobject]@{
        key = $entry.Key
        prefix = $prefix
        field = $field
        samples = $stats.samples
        min = $stats.min
        avg = $stats.avg
        p50 = $stats.p50
        p90 = $stats.p90
        p95 = $stats.p95
        p99 = $stats.p99
        max = $stats.max
        maxFrame = $fieldMaxFrames[$entry.Key]
    }
}
$fieldRows = @($fieldRows | Sort-Object @{ Expression = "p95"; Descending = $true }, @{ Expression = "avg"; Descending = $true }, key)
$fieldMap = @{}
foreach ($row in $fieldRows) {
    $fieldMap[$row.key] = $row
}

$loopValues = [double[]]@($loopFrames | ForEach-Object { [double]$_.frame_ms })
$loopStats = Get-Stats -Values $loopValues
$worstFrames = @($loopFrames | Sort-Object @{ Expression = "frame_ms"; Descending = $true } | Select-Object -First $WorstFrameCount | ForEach-Object {
    $frameKey = if ($null -ne $_.frame) { [string]$_.frame } else { "" }
    $records = if ($recordsByFrame.ContainsKey($frameKey)) { $recordsByFrame[$frameKey] } else { @{} }
    [pscustomobject]@{
        frame = $_.frame
        frame_ms = $_.frame_ms
        line = $_.line
        loop = if ($records.ContainsKey("PERF loop trace")) { $records["PERF loop trace"] } else { $null }
        render = if ($records.ContainsKey("PERF render trace NRI")) { $records["PERF render trace NRI"] } else { $null }
        shell = if ($records.ContainsKey("PERF pt shell trace NRI")) { $records["PERF pt shell trace NRI"] } else { $null }
        selectAccounting = if ($records.ContainsKey("PERF pt scene select accounting NRI")) { $records["PERF pt scene select accounting NRI"] } else { $null }
        sceneLight = if ($records.ContainsKey("PERF pt scene light detail NRI")) { $records["PERF pt scene light detail NRI"] } else { $null }
        texture = if ($records.ContainsKey("PERF pt texture detail NRI")) { $records["PERF pt texture detail NRI"] } else { $null }
    }
})

$thresholdFailures = New-Object System.Collections.Generic.List[object]
$thresholds = if ($null -ne $scenario) { Get-ObjectProperty -Object $scenario -Name "thresholds" } else { $null }
function Add-ThresholdFailure {
    param(
        [string]$Name,
        [double]$Actual,
        [double]$Limit
    )

    if ($Actual -gt $Limit) {
        $script:thresholdFailures.Add([pscustomobject]@{
            name = $Name
            actual = [Math]::Round($Actual, 3)
            limit = [Math]::Round($Limit, 3)
        })
    }
}

if ($null -ne $thresholds) {
    if ($thresholds.PSObject.Properties.Name.Contains("frameMsP95Max")) {
        Add-ThresholdFailure -Name "frameMsP95Max" -Actual ([double]$loopStats.p95) -Limit ([double]$thresholds.frameMsP95Max)
    }
    if ($thresholds.PSObject.Properties.Name.Contains("frameMsMaxMax")) {
        Add-ThresholdFailure -Name "frameMsMaxMax" -Actual ([double]$loopStats.max) -Limit ([double]$thresholds.frameMsMaxMax)
    }
    if ($thresholds.PSObject.Properties.Name.Contains("framesOver100Max")) {
        Add-ThresholdFailure -Name "framesOver100Max" -Actual ([double](Count-ValuesOver -Values $loopValues -Threshold 100.0)) -Limit ([double]$thresholds.framesOver100Max)
    }
    $thresholdFieldMap = @{
        renderTotalP95Max = "PERF render trace NRI/total"
        shellSelectP95Max = "PERF pt shell trace NRI/select"
        shellLightsP95Max = "PERF pt shell trace NRI/lights"
    }
    foreach ($property in $thresholdFieldMap.Keys) {
        if ($thresholds.PSObject.Properties.Name.Contains($property)) {
            $value = Get-FieldValue -FieldMap $fieldMap -Key $thresholdFieldMap[$property] -Property "p95"
            if ($null -ne $value) {
                Add-ThresholdFailure -Name $property -Actual ([double]$value) -Limit ([double]$thresholds.$property)
            }
        }
    }
}

$errors = New-Object System.Collections.Generic.List[string]
foreach ($entry in $requiredHits.GetEnumerator()) {
    if ([int]$entry.Value -le 0) {
        $errors.Add("missing required prefix '$($entry.Key)'")
    }
}
if ($forbiddenHits.Count -gt 0) {
    foreach ($hit in $forbiddenHits) {
        $errors.Add("forbidden pattern '$($hit.pattern)' at line $($hit.line)")
    }
}
if ($loopFrames.Count -lt $MinLoopTraceSamples) {
    $errors.Add("loop trace samples $($loopFrames.Count) < required $MinLoopTraceSamples")
}
if ($thresholdFailures.Count -gt 0) {
    foreach ($failure in $thresholdFailures) {
        $errors.Add("threshold '$($failure.name)' actual $($failure.actual) > limit $($failure.limit)")
    }
}

$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    path = $resolvedLog.Path
    scenario = $scenario
    requiredPrefixes = $requiredHits
    forbiddenHits = $forbiddenHits.ToArray()
    errors = $errors.ToArray()
    thresholdFailures = $thresholdFailures.ToArray()
    loopTrace = [pscustomobject]@{
        samples = $loopFrames.Count
        firstFrame = if ($loopFrames.Count -gt 0) { $loopFrames[0].frame } else { $null }
        lastFrame = if ($loopFrames.Count -gt 0) { $loopFrames[$loopFrames.Count - 1].frame } else { $null }
        min = $loopStats.min
        avg = $loopStats.avg
        p50 = $loopStats.p50
        p90 = $loopStats.p90
        p95 = $loopStats.p95
        p99 = $loopStats.p99
        max = $loopStats.max
        framesOver16_7 = Count-ValuesOver -Values $loopValues -Threshold 16.7
        framesOver33_3 = Count-ValuesOver -Values $loopValues -Threshold 33.3
        framesOver50 = Count-ValuesOver -Values $loopValues -Threshold 50.0
        framesOver100 = Count-ValuesOver -Values $loopValues -Threshold 100.0
        framesOver200 = Count-ValuesOver -Values $loopValues -Threshold 200.0
        rangesOver50 = Get-ContiguousRangesOver -Frames @($loopFrames.ToArray()) -Threshold 50.0
        rangesOver100 = Get-ContiguousRangesOver -Frames @($loopFrames.ToArray()) -Threshold 100.0
    }
    fields = $fieldRows
    worstFrames = $worstFrames
}

if ($CsvPath) {
    $csvDirectory = Split-Path -Parent $CsvPath
    if ($csvDirectory) {
        New-Item -ItemType Directory -Force -Path $csvDirectory | Out-Null
    }
    $fieldRows | Export-Csv -NoTypeInformation -LiteralPath $CsvPath
}

if ($SummaryOutput) {
    $summaryDirectory = Split-Path -Parent $SummaryOutput
    if ($summaryDirectory) {
        New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null
    }
    $summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
}

Write-Host "NRI perf log analyzed: ok=$($summary.ok) loop_samples=$($summary.loopTrace.samples) frame_p95=$($summary.loopTrace.p95) frame_max=$($summary.loopTrace.max) log=$($summary.path)"
if (-not $summary.ok) {
    foreach ($errorText in $summary.errors) {
        Write-Host "  $errorText"
    }
}
