param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [string]$SummaryOutput,

    [int]$WorstFrameCount = 12,

    [int]$WindowFrameGap = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

function ConvertTo-Int64OrNull {
    param([object]$Value)

    if ($null -eq $Value) {
        return $null
    }
    $number = 0L
    if ([long]::TryParse([string]$Value, [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }
    return $null
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
            p95 = 0.0
            p99 = 0.0
            max = 0.0
            over25ms = 0
            over33ms = 0
            over50ms = 0
            over100ms = 0
            over200ms = 0
        }
    }

    $measure = $Values | Measure-Object -Average -Minimum -Maximum
    return [pscustomobject]@{
        samples = $Values.Count
        min = [Math]::Round([double]$measure.Minimum, 3)
        avg = [Math]::Round([double]$measure.Average, 3)
        p50 = [Math]::Round((Get-Percentile -Values $Values -Percentile 50), 3)
        p95 = [Math]::Round((Get-Percentile -Values $Values -Percentile 95), 3)
        p99 = [Math]::Round((Get-Percentile -Values $Values -Percentile 99), 3)
        max = [Math]::Round([double]$measure.Maximum, 3)
        over25ms = @($Values | Where-Object { $_ -gt 25.0 }).Count
        over33ms = @($Values | Where-Object { $_ -gt 33.0 }).Count
        over50ms = @($Values | Where-Object { $_ -gt 50.0 }).Count
        over100ms = @($Values | Where-Object { $_ -gt 100.0 }).Count
        over200ms = @($Values | Where-Object { $_ -gt 200.0 }).Count
    }
}

function New-Record {
    param(
        [int]$Line,
        [string]$Kind,
        [string]$Text
    )

    $fields = Read-KeyValuePairs -Text $Text
    return [pscustomobject]@{
        line = $Line
        kind = $Kind
        fields = [pscustomobject]$fields
        text = $Text.Trim()
    }
}

if (-not (Test-Path -LiteralPath $LogPath)) {
    throw "LogPath not found: $LogPath"
}

$resolvedLog = (Resolve-Path -LiteralPath $LogPath).Path
$loopFrames = New-Object System.Collections.Generic.List[object]
$records = New-Object System.Collections.Generic.List[object]
$failures = New-Object System.Collections.Generic.List[object]

$failurePatterns = @(
    "Device removed",
    "device lost",
    "DXGI_ERROR_DEVICE",
    "DRED after",
    "DRED breadcrumb",
    "DRED page fault",
    "DRED page-fault",
    "DeviceRemovedExtendedData",
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

$lineNumber = 0
foreach ($line in Get-Content -LiteralPath $resolvedLog) {
    $lineNumber++

    foreach ($pattern in $failurePatterns) {
        if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $failures.Add([pscustomobject]@{
                line = $lineNumber
                pattern = $pattern
                text = $line.Trim()
            })
            break
        }
    }

    if ($line.StartsWith("PERF loop trace:", [System.StringComparison]::Ordinal)) {
        $fields = Read-KeyValuePairs -Text $line
        $frame = ConvertTo-Int64OrNull $fields["frame"]
        $frameMs = ConvertTo-DoubleOrNull $fields["frame_ms"]
        if ($null -ne $frame -and $null -ne $frameMs) {
            $loopFrames.Add([pscustomobject]@{
                line = $lineNumber
                frame = $frame
                frame_ms = $frameMs
                try_ms = ConvertTo-DoubleOrNull $fields["try_ms"]
                display_ms = ConvertTo-DoubleOrNull $fields["display_ms"]
                display_render_ms = ConvertTo-DoubleOrNull $fields["display_render_ms"]
                display_overlay_ms = ConvertTo-DoubleOrNull $fields["display_overlay_ms"]
                display_update_ms = ConvertTo-DoubleOrNull $fields["display_update_ms"]
                state = [string]$fields["state"]
                fields = [pscustomobject]$fields
                text = $line.Trim()
            })
        }
        continue
    }

    if ($line.StartsWith("NRI PT level transition:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "level_transition" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT loading gate:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "loading_gate" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT loading summary:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "loading_summary" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT loading voxel gpu requests:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "voxel_gpu_requests" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT voxel residency reconcile:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "voxel_residency_reconcile" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT voxel admission summary:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "voxel_admission_summary" -Text $line))
        continue
    }
    if ($line.StartsWith("NRI PT loading voxel shared blas:", [System.StringComparison]::Ordinal)) {
        $records.Add((New-Record -Line $lineNumber -Kind "voxel_shared_blas" -Text $line))
        continue
    }
}

$windows = New-Object System.Collections.Generic.List[object]
$current = New-Object System.Collections.Generic.List[object]
$previousFrame = $null
foreach ($frame in $loopFrames) {
    if ($current.Count -gt 0 -and $null -ne $previousFrame -and ([long]$frame.frame - [long]$previousFrame) -gt $WindowFrameGap) {
        $windows.Add($current.ToArray())
        $current = New-Object System.Collections.Generic.List[object]
    }
    $current.Add($frame)
    $previousFrame = $frame.frame
}
if ($current.Count -gt 0) {
    $windows.Add($current.ToArray())
}

$windowSummaries = New-Object System.Collections.Generic.List[object]
$windowIndex = 0
foreach ($window in $windows) {
    $frameTimes = @($window | ForEach-Object { [double]$_.frame_ms })
    $tryTimes = @($window | Where-Object { $null -ne $_.try_ms } | ForEach-Object { [double]$_.try_ms })
    $displayTimes = @($window | Where-Object { $null -ne $_.display_ms } | ForEach-Object { [double]$_.display_ms })
    $renderTimes = @($window | Where-Object { $null -ne $_.display_render_ms } | ForEach-Object { [double]$_.display_render_ms })
    $updateTimes = @($window | Where-Object { $null -ne $_.display_update_ms } | ForEach-Object { [double]$_.display_update_ms })
    $worstFrames = @($window | Sort-Object -Property frame_ms -Descending | Select-Object -First $WorstFrameCount)

    $windowSummaries.Add([pscustomobject]@{
        index = $windowIndex
        firstFrame = [long]$window[0].frame
        lastFrame = [long]$window[$window.Count - 1].frame
        lineStart = [int]$window[0].line
        lineEnd = [int]$window[$window.Count - 1].line
        frame = Get-Stats -Values $frameTimes
        try = Get-Stats -Values $tryTimes
        display = Get-Stats -Values $displayTimes
        displayRender = Get-Stats -Values $renderTimes
        displayUpdate = Get-Stats -Values $updateTimes
        worstFrames = $worstFrames
    })
    $windowIndex++
}

$recordCounts = [ordered]@{}
foreach ($record in $records) {
    if (-not $recordCounts.Contains($record.kind)) {
        $recordCounts[$record.kind] = 0
    }
    $recordCounts[$record.kind] = [int]$recordCounts[$record.kind] + 1
}

$summary = [pscustomobject]@{
    logPath = $resolvedLog
    generatedAt = (Get-Date).ToString("o")
    loopTraceFrameCount = $loopFrames.Count
    windowCount = $windowSummaries.Count
    windows = $windowSummaries
    recordCounts = [pscustomobject]$recordCounts
    records = $records
    failureCount = $failures.Count
    failures = $failures
}

Write-Host "Level transition log summary: $resolvedLog"
Write-Host ("  loop frames: {0}, windows: {1}, failures: {2}" -f $loopFrames.Count, $windowSummaries.Count, $failures.Count)
foreach ($window in $windowSummaries) {
    Write-Host ("  window {0}: frames {1}-{2}, samples={3}, frame p50={4} p95={5} p99={6} max={7}, >50ms={8}, >100ms={9}" -f `
        $window.index,
        $window.firstFrame,
        $window.lastFrame,
        $window.frame.samples,
        $window.frame.p50,
        $window.frame.p95,
        $window.frame.p99,
        $window.frame.max,
        $window.frame.over50ms,
        $window.frame.over100ms)
}
foreach ($name in $recordCounts.Keys) {
    Write-Host ("  {0}: {1}" -f $name, $recordCounts[$name])
}

if ($SummaryOutput) {
    $summaryDir = Split-Path -Parent $SummaryOutput
    if ($summaryDir -and -not (Test-Path -LiteralPath $summaryDir)) {
        New-Item -ItemType Directory -Path $summaryDir | Out-Null
    }
    $summary | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
    Write-Host "  wrote summary: $SummaryOutput"
}
