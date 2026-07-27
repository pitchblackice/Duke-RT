param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [Parameter(Mandatory = $true)]
    [string]$ProfileSpecPath,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File,

    [int]$TimeoutSeconds = 0,

    [string[]]$AdditionalArgs = @(),

    [string]$OutputDirectory,

    [string]$SummaryOutput,

    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RequiredProperty {
    param([object]$Object, [string]$Name, [string]$Context)

    if ($null -eq $Object -or -not $Object.PSObject.Properties.Name.Contains($Name)) {
        throw "$Context is missing '$Name'"
    }
    return $Object.$Name
}

function ConvertTo-DoubleOrNull {
    param([string]$Value)

    if ($Value.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) { return $null }
    $number = 0.0
    if ([double]::TryParse($Value, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }
    return $null
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)

    $sorted = @($Values | Sort-Object)
    $rank = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, [int]$rank))
    return [double]$sorted[$index]
}

function Get-PooledFields {
    param([string[]]$LogPaths)

    $samples = @{}
    foreach ($logPath in $LogPaths) {
        foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $logPath).Path)) {
            if ($line -notmatch '^(PERF [^:]+):\s+(.*)$') { continue }
            $prefix = $Matches[1]
            $body = $Matches[2]
            if ($body -notmatch '(?:^|\s)compact=1(?:\s|$)') { continue }
            foreach ($match in [regex]::Matches($body, '([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\([^\)]*\)|[^\s]+)')) {
                $number = ConvertTo-DoubleOrNull $match.Groups[2].Value
                if ($null -eq $number) { continue }
                $key = "$prefix/$($match.Groups[1].Value)"
                if (-not $samples.ContainsKey($key)) {
                    $samples[$key] = [Collections.Generic.List[double]]::new()
                }
                $samples[$key].Add([double]$number)
            }
        }
    }

    return @($samples.GetEnumerator() | ForEach-Object {
        $values = [double[]]$_.Value.ToArray()
        $measure = $values | Measure-Object -Average -Maximum
        [pscustomobject]@{
            key = $_.Key
            samples = $values.Count
            avg = [Math]::Round([double]$measure.Average, 3)
            p50 = [Math]::Round((Get-Percentile $values 50), 3)
            p95 = [Math]::Round((Get-Percentile $values 95), 3)
            p99 = [Math]::Round((Get-Percentile $values 99), 3)
            max = [Math]::Round([double]$measure.Maximum, 3)
        }
    } | Sort-Object key)
}

$resolvedScenario = Resolve-Path -LiteralPath $ScenarioPath -ErrorAction Stop
$resolvedProfiles = Resolve-Path -LiteralPath $ProfileSpecPath -ErrorAction Stop
$scenario = Get-Content -LiteralPath $resolvedScenario.Path -Raw | ConvertFrom-Json
$spec = Get-Content -LiteralPath $resolvedProfiles.Path -Raw | ConvertFrom-Json
if ([int](Get-RequiredProperty $spec "version" "profile spec") -ne 1) {
    throw "unsupported smoke profile spec version"
}
$profiles = @(Get-RequiredProperty $spec "profiles" "profile spec")
if ($profiles.Count -lt 2) { throw "profile spec must contain at least two profiles" }

$capture = Get-RequiredProperty $scenario "capture" "scenario"
$captureFrames = [int](Get-RequiredProperty $capture "loopTraceFrames" "scenario capture")
if ($captureFrames -lt 256) { throw "smoke profile sweep requires at least 256 loop-trace frames" }
$scenarioText = $scenario | ConvertTo-Json -Depth 30 -Compress
if ($scenarioText -notmatch 'perf_compactframes\s+256') {
    throw "smoke profile sweep scenario must request perf_compactframes 256"
}

$profileIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($profile in $profiles) {
    $id = [string](Get-RequiredProperty $profile "id" "profile")
    if ($id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { throw "invalid profile id '$id'" }
    if (-not $profileIds.Add($id)) { throw "duplicate profile id '$id'" }
    $profileArgs = @(Get-RequiredProperty $profile "additionalArgs" "profile '$id'")
    if ($profileArgs.Count -eq 0) { throw "profile '$id' has no additionalArgs" }
}

$schedule = [Collections.Generic.List[object]]::new()
for ($repeat = 0; $repeat -lt 3; ++$repeat) {
    for ($slot = 0; $slot -lt $profiles.Count; ++$slot) {
        $profileIndex = ($slot + $repeat) % $profiles.Count
        $schedule.Add([pscustomobject]@{
            repeat = $repeat + 1
            slot = $slot + 1
            profileIndex = $profileIndex
            profileId = [string]$profiles[$profileIndex].id
        })
    }
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Get-Location) ("tools/logs/perf/smoke-profile-sweep/" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
if (-not $SummaryOutput) { $SummaryOutput = Join-Path $OutputDirectory "summary.json" }

if ($ValidateOnly) {
    Write-Host "Smoke profile sweep valid: profiles=$($profiles.Count) repeats=3 frames_per_repeat=256"
    foreach ($entry in $schedule) {
        Write-Host "  repeat=$($entry.repeat) slot=$($entry.slot) profile=$($entry.profileId)"
    }
    return
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$perfRunner = Join-Path $PSScriptRoot "run-nri-perf.ps1"
$results = [Collections.Generic.List[object]]::new()
$profileLogs = @{}
foreach ($profile in $profiles) { $profileLogs[[string]$profile.id] = [Collections.Generic.List[string]]::new() }

foreach ($entry in $schedule) {
    $profile = $profiles[$entry.profileIndex]
    $runDirectory = Join-Path $OutputDirectory ("repeat-{0}/slot-{1}-{2}" -f $entry.repeat, $entry.slot, $entry.profileId)
    $arguments = @{
        ScenarioPath = $resolvedScenario.Path
        RazePath = $RazePath
        Runs = 1
        OutputDirectory = $runDirectory
        AdditionalArgs = @($AdditionalArgs) + @($profile.additionalArgs | ForEach-Object { [string]$_ })
    }
    if ($GameGrp) { $arguments.GameGrp = $GameGrp }
    if ($File) { $arguments.File = $File }
    if ($TimeoutSeconds -gt 0) { $arguments.TimeoutSeconds = $TimeoutSeconds }
    & $perfRunner @arguments

    $summaryPath = Join-Path $runDirectory "summary.json"
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if (-not $summary.ok) { throw "profile '$($entry.profileId)' repeat $($entry.repeat) failed: $summaryPath" }
    $logPath = Join-Path $runDirectory "run-1.log"
    $completion = Select-String -LiteralPath $logPath -Pattern '^PERF compact capture complete: .*status=complete .*requested=256 .*eligible=256 .*observed=256 .*dropped=0 ' | Select-Object -Last 1
    if ($null -eq $completion) {
        throw "profile '$($entry.profileId)' repeat $($entry.repeat) did not complete exactly 256 accepted compact frames"
    }
    $profileLogs[$entry.profileId].Add($logPath)
    $results.Add([pscustomobject]@{
        repeat = $entry.repeat
        slot = $entry.slot
        profileId = $entry.profileId
        logPath = $logPath
        summaryPath = $summaryPath
        acceptedFrames = 256
    })
}

$profileSummaries = foreach ($profile in $profiles) {
    $id = [string]$profile.id
    $logs = $profileLogs[$id].ToArray()
    [pscustomobject]@{
        id = $id
        workFamily = if ($profile.PSObject.Properties.Name.Contains("workFamily")) { [string]$profile.workFamily } else { $null }
        workQuantity = if ($profile.PSObject.Properties.Name.Contains("workQuantity")) { [double]$profile.workQuantity } else { $null }
        repeats = $logs.Count
        acceptedFrames = $logs.Count * 256
        logs = $logs
        pooledFields = Get-PooledFields -LogPaths $logs
    }
}

$output = [pscustomobject]@{
    schema = 1
    ok = $true
    scenarioPath = $resolvedScenario.Path
    profileSpecPath = $resolvedProfiles.Path
    interleave = "cyclic"
    repeats = 3
    framesPerRepeat = 256
    schedule = $schedule.ToArray()
    runs = $results.ToArray()
    profiles = @($profileSummaries)
}
$summaryDirectory = Split-Path -Parent $SummaryOutput
if ($summaryDirectory) { New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null }
$output | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
Write-Host "Smoke profile sweep complete: profiles=$($profiles.Count) accepted_per_profile=768 summary=$SummaryOutput"
