[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$LogPath,

    [double]$TargetMs = 16.667,

    [double]$GuardedEnvelopeMs = 14.5,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Pairs([string]$Line)
{
    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)'))
    {
        $pairs[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $pairs
}

function Read-Number([hashtable]$Row, [string]$Name, [double]$Default = [double]::NaN)
{
    if (-not $Row.ContainsKey($Name))
    {
        if (-not [double]::IsNaN($Default)) { return $Default }
        throw "Missing numeric field '$Name'."
    }
    $value = 0.0
    if (-not [double]::TryParse([string]$Row[$Name], [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$value))
    {
        throw "Nonnumeric field '$Name=$($Row[$Name])'."
    }
    return $value
}

function Get-Distribution([double[]]$Values)
{
    if ($Values.Count -eq 0)
    {
        return [pscustomobject]@{ samples = 0; min = $null; p01 = $null; p05 = $null; p50 = $null; p95 = $null; p99 = $null; max = $null }
    }
    $sorted = @($Values | Sort-Object)
    function Pick([double]$Fraction)
    {
        $index = [Math]::Ceiling($Fraction * $sorted.Count) - 1
        return [Math]::Round([double]$sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))], 6)
    }
    return [pscustomobject]@{
        samples = $sorted.Count
        min = [Math]::Round([double]$sorted[0], 6)
        p01 = Pick 0.01
        p05 = Pick 0.05
        p50 = Pick 0.50
        p95 = Pick 0.95
        p99 = Pick 0.99
        max = [Math]::Round([double]$sorted[$sorted.Count - 1], 6)
    }
}

if ($TargetMs -le 0 -or $GuardedEnvelopeMs -le 0) { throw 'Timing targets must be positive.' }

$stageNames = @('admission', 'upload', 'arena_copy', 'classify', 'scan', 'emit', 'finalize', 'voxel_blas', 'world_tlas')
$allFrames = [System.Collections.Generic.List[object]]::new()
$perLog = [System.Collections.Generic.List[object]]::new()
$errors = [System.Collections.Generic.List[string]]::new()

foreach ($inputPath in $LogPath)
{
    $resolved = (Resolve-Path -LiteralPath $inputPath -ErrorAction Stop).Path
    $gpu = @{}
    $voxel = @{}
    $smoke = @{}
    foreach ($line in [System.IO.File]::ReadLines($resolved))
    {
        if ($line.StartsWith('PERF pt gpu timing NRI:') -and $line.Contains(' compact=1 '))
        {
            $row = Read-Pairs $line
            $gpu["$($row.epoch)/$($row.sample)"] = $row
        }
        elseif ($line.StartsWith('PERF pt voxel gpu timing NRI:') -and $line.Contains(' compact=1 '))
        {
            $row = Read-Pairs $line
            $voxel["$($row.epoch)/$($row.record)"] = $row
        }
        elseif ($line.StartsWith('PERF pt smoke gpu timing NRI:') -and $line.Contains(' compact=1 '))
        {
            $row = Read-Pairs $line
            $smoke["$($row.epoch)/$($row.record)"] = $row
        }
    }

    $frames = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @($gpu.Keys | Sort-Object))
    {
        if (-not $voxel.ContainsKey($key))
        {
            $errors.Add("${resolved}: GPU sample $key has no voxel timing row")
            continue
        }
        $g = $gpu[$key]
        $v = $voxel[$key]
        $complete = Read-Number $g 'segment'
        $trace = Read-Number $g 'trace_dispatch'
        $smokeTotal = Read-Number $g 'smoke_total' 0.0
        $admission = Read-Number $v 'admission'
        $frame = [pscustomobject]@{
            key = $key
            complete = $complete
            trace = $trace
            smoke = $smokeTotal
            admission = $admission
            preSmoke = $complete - $smokeTotal
            smokeHeadroom = $TargetMs - ($complete - $smokeTotal)
            postSmokeSlack = $TargetMs - $complete
            guardedSlack = $GuardedEnvelopeMs - $complete
            stages = $v
        }
        $frames.Add($frame)
        $allFrames.Add($frame)

        foreach ($field in @('invalid', 'dropped'))
        {
            if ((Read-Number $g $field) -ne 0) { $errors.Add("${resolved}: GPU sample $key has $field=$($g[$field])") }
        }
        if ((Read-Number $g 'resolved') -ne (Read-Number $g 'expected'))
        {
            $errors.Add("${resolved}: GPU sample $key expected/resolved mismatch")
        }
        if ($smoke.ContainsKey($key))
        {
            $s = $smoke[$key]
            if ((Read-Number $s 'invalid') -ne 0 -or (Read-Number $s 'dropped') -ne 0)
            {
                $errors.Add("${resolved}: smoke sample $key has invalid or dropped scopes")
            }
        }
    }

    $perLog.Add([pscustomobject]@{
        path = $resolved
        samples = $frames.Count
        completeGpuMs = Get-Distribution ([double[]]@($frames | ForEach-Object complete))
        traceDispatchMs = Get-Distribution ([double[]]@($frames | ForEach-Object trace))
        smokeMs = Get-Distribution ([double[]]@($frames | ForEach-Object smoke))
        voxelAdmissionMs = Get-Distribution ([double[]]@($frames | ForEach-Object admission))
        smokeHeadroomMs = Get-Distribution ([double[]]@($frames | ForEach-Object smokeHeadroom))
        postSmokeSlackMs = Get-Distribution ([double[]]@($frames | ForEach-Object postSmokeSlack))
        misses = [pscustomobject]@{
            target = @($frames | Where-Object { $_.complete -gt $TargetMs }).Count
            guardedEnvelope = @($frames | Where-Object { $_.complete -gt $GuardedEnvelopeMs }).Count
            withoutSmoke = @($frames | Where-Object { $_.preSmoke -gt $TargetMs }).Count
            withoutAdmission = @($frames | Where-Object { ($_.complete - $_.admission) -gt $TargetMs }).Count
            withoutSmokeOrAdmission = @($frames | Where-Object { ($_.preSmoke - $_.admission) -gt $TargetMs }).Count
        }
    })
}

$stageSummary = [ordered]@{}
foreach ($stage in $stageNames)
{
    $values = [double[]]@($allFrames | ForEach-Object { Read-Number $_.stages $stage })
    $stageSummary[$stage] = [pscustomobject]@{
        allFrames = Get-Distribution $values
        nonzeroEvents = Get-Distribution ([double[]]@($values | Where-Object { $_ -gt 0 }))
    }
}

$pooledHeadroom = [double[]]@($allFrames | ForEach-Object smokeHeadroom)
$pooledMisses = [pscustomobject]@{
    target = @($allFrames | Where-Object { $_.complete -gt $TargetMs }).Count
    guardedEnvelope = @($allFrames | Where-Object { $_.complete -gt $GuardedEnvelopeMs }).Count
    withoutSmoke = @($allFrames | Where-Object { $_.preSmoke -gt $TargetMs }).Count
    withoutAdmission = @($allFrames | Where-Object { ($_.complete - $_.admission) -gt $TargetMs }).Count
    withoutSmokeOrAdmission = @($allFrames | Where-Object { ($_.preSmoke - $_.admission) -gt $TargetMs }).Count
}
$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    schema = 1
    targetMs = $TargetMs
    guardedEnvelopeMs = $GuardedEnvelopeMs
    logs = $perLog.ToArray()
    pooled = [pscustomobject]@{
        samples = $allFrames.Count
        completeGpuMs = Get-Distribution ([double[]]@($allFrames | ForEach-Object complete))
        traceDispatchMs = Get-Distribution ([double[]]@($allFrames | ForEach-Object trace))
        smokeMs = Get-Distribution ([double[]]@($allFrames | ForEach-Object smoke))
        voxelAdmissionMs = Get-Distribution ([double[]]@($allFrames | ForEach-Object admission))
        smokeHeadroomMs = Get-Distribution $pooledHeadroom
        postSmokeSlackMs = Get-Distribution ([double[]]@($allFrames | ForEach-Object postSmokeSlack))
        misses = $pooledMisses
        stageTimingsMs = [pscustomobject]$stageSummary
    }
    notes = @(
        'smokeHeadroom is computed per frame as target - (complete GPU - measured smoke); percentiles are not subtracted.',
        'Voxel admission is inclusive. Child stages are attribution only and are not added to admission.',
        'Smoke and voxel rows join to compact GPU rows by epoch/record -> epoch/sample.'
    )
    errors = $errors.ToArray()
}

$json = $summary | ConvertTo-Json -Depth 16
if ($SummaryOutput)
{
    $parent = Split-Path -Parent $SummaryOutput
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    Set-Content -LiteralPath $SummaryOutput -Value $json -Encoding UTF8
}
$json
if (-not $summary.ok) { exit 1 }
