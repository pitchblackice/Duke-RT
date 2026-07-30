param(
    [string]$ScenarioPath = 'tools/validation/perf-scenarios/current-direct-trace-large-voxel-d3d12.json',
    [string]$RazePath = 'build/terminal-ninja/raze.exe',
    [int]$Cycles = 3,
    [int]$Samples = 256,
    [int]$WarmupSamples = 128,
    [int[]]$Policies = @(0, 1, 3),
    [int]$CompactionPolicy = -1,
    [int]$TimeoutSeconds = 900,
    [string]$OutputDirectory,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Set-ScenarioCvar([object]$Scenario, [string]$Name, [string]$Value) {
    $args = @($Scenario.launch.extraArgs)
    for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
        if ([string]$args[$index] -eq '+set' -and [string]$args[$index + 1] -eq $Name) {
            $args[$index + 2] = $Value
            $Scenario.launch.extraArgs = $args
            return
        }
    }
    $Scenario.launch.extraArgs = @($args) + @('+set', $Name, $Value)
}

function New-MatrixScenario([object]$Base, [int]$Policy, [bool]$Compact, [string]$Directory) {
    $scenario = $Base | ConvertTo-Json -Depth 24 | ConvertFrom-Json
    $loopFrames = $Samples + $WarmupSamples + 3
    $leg = "policy-$Policy-compact-$([int]$Compact)"
    $settings = [ordered]@{
        cl_interpolate = 'false'
        nri_ptlightbounces = '2'
        nri_ptmirrorbounces = '2'
        nri_ptportaldepth = '3'
        nri_ptemissivesamples = '1'
        nri_ptemissiveprimarybudget = '2'
        nri_ptvoxelomitoccurrences = 'false'
		nri_ptvoxelblaspolicy = [string]$Policy
        nri_ptvoxelblascompact = $Compact.ToString().ToLowerInvariant()
        nri_ptvoxelarenapresize = 'true'
        nri_ptvoxelcomputepreloadterminalcommand = 'set nri_ptloadingtrace 1'
        nri_ptvoxelcomputepreloadreleasecommand = "set nri_ptloadingtrace 0; perf_looptraceframes $loopFrames; perf_fixedsimulationframes $($Samples + $WarmupSamples); perf_compactwarmupframes $WarmupSamples; perf_compactframes $Samples"
    }
    foreach ($setting in $settings.GetEnumerator()) {
        Set-ScenarioCvar -Scenario $scenario -Name $setting.Key -Value ([string]$setting.Value)
    }
    $scenario.name = "gpu-time-slice4-$leg-d3d12"
    $scenario.description = "Slice 4 fixed exact voxel BLAS matrix leg $leg."
    $scenario.capture.loopTraceFrames = $loopFrames
    $scenario.capture.runs = 1
    $scenario.capture.timeoutSeconds = $TimeoutSeconds
    $scenario.capture.stopWhenLoopTraceFramesCaptured = $false
    if (-not $scenario.capture.PSObject.Properties.Name.Contains('stopWhenPrefix')) {
        $scenario.capture | Add-Member -NotePropertyName stopWhenPrefix -NotePropertyValue 'PERF compact capture complete:'
        $scenario.capture | Add-Member -NotePropertyName stopWhenPrefixCount -NotePropertyValue 1
    }
    else {
        $scenario.capture.stopWhenPrefix = 'PERF compact capture complete:'
        $scenario.capture.stopWhenPrefixCount = 1
    }
    $metadata = [pscustomobject]@{ policy = $Policy; compact = $Compact; samples = $Samples }
    $scenario | Add-Member -Force -NotePropertyName voxelBlasPolicyMatrix -NotePropertyValue $metadata
    $path = Join-Path $Directory "$leg.json"
    $scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
    return [pscustomobject]@{ leg = $leg; policy = $Policy; compact = $Compact; path = $path }
}

if ($Cycles -lt 1) { throw 'Cycles must be at least one.' }
if ($Samples -lt 8 -or $Samples -gt 2048) { throw 'Samples must be in 8..2048.' }
if ($WarmupSamples -lt 0 -or $WarmupSamples -gt 2048 -or $Samples + $WarmupSamples -gt 4096) { throw 'WarmupSamples must fit the fixed-capture bounds.' }
if (@($Policies | Where-Object { $_ -notin @(0, 1, 3) }).Count -ne 0) { throw 'Policies may contain only 0, 1, and 3.' }
if ($CompactionPolicy -ne -1 -and $CompactionPolicy -notin $Policies) { throw 'CompactionPolicy must be -1 or one of Policies.' }

$base = Get-Content -LiteralPath (Resolve-Path -LiteralPath $ScenarioPath) -Raw | ConvertFrom-Json
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Get-Location) ('tools/logs/perf/gpu-time-slice4-blas-matrix/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not $SummaryOutput) { $SummaryOutput = Join-Path $resolvedOutput 'summary.json' }
$generatedDirectory = Join-Path $resolvedOutput 'generated-scenarios'
New-Item -ItemType Directory -Force -Path $generatedDirectory | Out-Null

$legs = [Collections.Generic.List[object]]::new()
foreach ($policy in $Policies) { $legs.Add((New-MatrixScenario -Base $base -Policy $policy -Compact $false -Directory $generatedDirectory)) }
if ($CompactionPolicy -ge 0) { $legs.Add((New-MatrixScenario -Base $base -Policy $CompactionPolicy -Compact $true -Directory $generatedDirectory)) }

$runner = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'run-nri-perf.ps1')).Path
$checker = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'check-nri-fixed-simulation-log.ps1')).Path
$analyzer = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'analyze-nri-voxel-blas-policy-matrix.ps1')).Path
$powershell = (Get-Process -Id $PID).Path
$entries = [Collections.Generic.List[object]]::new()
$sequence = 0

for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
    $ordered = @()
    $start = (($legs.Count - (($cycle - 1) % $legs.Count)) % $legs.Count)
    for ($offset = 0; $offset -lt $legs.Count; ++$offset) {
        $ordered += $legs[($start + $offset) % $legs.Count]
    }
    foreach ($leg in $ordered) {
        ++$sequence
        $legOutput = Join-Path $resolvedOutput ('{0:D2}-{1}-cycle-{2}' -f $sequence, $leg.leg, $cycle)
        $baseSummary = Join-Path $legOutput 'base-summary.json'
        & $powershell -NoProfile -ExecutionPolicy Bypass -File $runner -ScenarioPath $leg.path -RazePath $RazePath -Runs 1 -TimeoutSeconds $TimeoutSeconds -OutputDirectory $legOutput -SummaryOutput $baseSummary
        if ($LASTEXITCODE -ne 0) { throw "Perf runner failed for sequence $sequence." }
        $logPath = Join-Path $legOutput 'run-1.log'
        $fixedSummary = Join-Path $legOutput 'fixed-summary.json'
        & $powershell -NoProfile -ExecutionPolicy Bypass -File $checker -LogPath $logPath -ExpectedSamples $Samples -RouteMode 0 -ExpectedVisibleChunkGate 1 -SkipSpatialPublicationCheck -RequireStrictFirstFrameRelease -RequireRecovery -AllowInitialHistoryReset -SummaryOutput $fixedSummary
        if ($LASTEXITCODE -ne 0) { throw "Fixed-capture validation failed for sequence $sequence." }
        $entries.Add([pscustomobject]@{
            sequence = $sequence; cycle = $cycle; leg = $leg.leg; policy = $leg.policy; compact = $leg.compact
            scenarioPath = $leg.path; outputDirectory = $legOutput; logPath = $logPath
            baseSummaryPath = $baseSummary; fixedSummaryPath = $fixedSummary
        })
        [pscustomobject]@{ schema = 1; samples = $Samples; entries = $entries.ToArray() } |
            ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'matrix-manifest.json') -Encoding UTF8
    }
}

$manifestPath = Join-Path $resolvedOutput 'matrix-manifest.json'
& $powershell -NoProfile -ExecutionPolicy Bypass -File $analyzer -ManifestPath $manifestPath -SummaryOutput $SummaryOutput
exit $LASTEXITCODE
