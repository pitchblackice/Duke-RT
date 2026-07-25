param(
    [string[]]$ScenarioPath = @(
        "tools/validation/perf-scenarios/current-direct-trace-general-churn-d3d12.json",
        "tools/validation/perf-scenarios/current-direct-trace-large-voxel-d3d12.json"
    ),

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File,

    [int]$Runs = 1,

    [int]$TimeoutSeconds = 0,

    [switch]$Build,

    [switch]$PrepareOnly,

    [string]$OutputDirectory,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Set-ScenarioCvar {
    param([object]$Scenario, [string]$Name, [string]$Value)
    $args = @($Scenario.launch.extraArgs)
    for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
        if ([string]$args[$index] -eq "+set" -and [string]$args[$index + 1] -eq $Name) {
            $args[$index + 2] = $Value
            $Scenario.launch.extraArgs = $args
            return
        }
    }
    $Scenario.launch.extraArgs = @($args) + @("+set", $Name, $Value)
}

function New-LegScenario {
    param([object]$Profile, [object]$Leg, [string]$Directory)
    $scenario = $Profile.scenario | ConvertTo-Json -Depth 24 | ConvertFrom-Json
    foreach ($setting in $Leg.cvars.GetEnumerator()) {
        Set-ScenarioCvar -Scenario $scenario -Name $setting.Key -Value ([string]$setting.Value)
    }
    $scenario.name = "$($scenario.name)-$($Leg.name)"
    $scenario.description = "$($scenario.description) Matrix leg: $($Leg.name)."
    $matrixMetadata = [pscustomobject]@{
        profile = $Profile.profile
        leg = $Leg.name
        traceClass = $Leg.traceClass
        occurrenceMode = $Leg.occurrenceMode
        minimumWorkloadSchema = 3
        expectedTrace = [pscustomobject]$Leg.expectedTrace
    }
    $scenario | Add-Member -NotePropertyName directTraceMatrix -NotePropertyValue $matrixMetadata
    $path = Join-Path $Directory ("{0}-{1}.json" -f $Profile.profile, $Leg.name)
    $scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
    return [pscustomobject]@{ path = $path; profile = $Profile.profile; leg = $Leg.name }
}

if ($Runs -lt 1) { throw "Runs must be at least 1." }
if ($ScenarioPath.Count -lt 1) { throw "At least one profile scenario is required." }

$profiles = @($ScenarioPath | ForEach-Object {
    $resolved = (Resolve-Path -LiteralPath $_ -ErrorAction Stop).Path
    $scenario = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    $profile = [string]$scenario.directTraceProfile
    if ([string]::IsNullOrWhiteSpace($profile)) { throw "Scenario '$resolved' must declare directTraceProfile." }
    [pscustomobject]@{ path = $resolved; profile = $profile; scenario = $scenario }
})
if (@($profiles.profile | Sort-Object -Unique).Count -ne $profiles.Count) { throw "Profile names must be unique." }

if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/perf/current-direct-trace-matrix/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not $SummaryOutput) { $SummaryOutput = Join-Path $resolvedOutput "summary.json" }
$generatedScenarioDirectory = Join-Path $resolvedOutput "generated-scenarios"
New-Item -ItemType Directory -Force -Path $generatedScenarioDirectory | Out-Null

$legDefinitions = @(
    [pscustomobject]@{
        name = "default-full"; traceClass = "default"; occurrenceMode = "full"
        cvars = [ordered]@{ nri_ptlightbounces = 4; nri_ptmirrorbounces = 8; nri_ptportaldepth = 6; nri_ptemissivesamples = 4; nri_ptemissiveprimarybudget = 2; nri_ptvoxelomitoccurrences = "false" }
        expectedTrace = [ordered]@{ light_bounces = 4; mirror_bounces = 8; portal_depth = 6; emissive_samples = 2; emissive_requested = 4; emissive_budget = 2; direct_scene = 0; voxel_occurrence_control = 0 }
    },
    [pscustomobject]@{
        name = "candidate-full"; traceClass = "candidate"; occurrenceMode = "full"
        cvars = [ordered]@{ nri_ptlightbounces = 2; nri_ptmirrorbounces = 2; nri_ptportaldepth = 3; nri_ptemissivesamples = 1; nri_ptemissiveprimarybudget = 2; nri_ptvoxelomitoccurrences = "false" }
        expectedTrace = [ordered]@{ light_bounces = 2; mirror_bounces = 2; portal_depth = 3; emissive_samples = 1; emissive_requested = 1; emissive_budget = 2; direct_scene = 0; voxel_occurrence_control = 0 }
    },
    [pscustomobject]@{
        name = "default-omit"; traceClass = "default"; occurrenceMode = "omit"
        cvars = [ordered]@{ nri_ptlightbounces = 4; nri_ptmirrorbounces = 8; nri_ptportaldepth = 6; nri_ptemissivesamples = 4; nri_ptemissiveprimarybudget = 2; nri_ptvoxelomitoccurrences = "true" }
        expectedTrace = [ordered]@{ light_bounces = 4; mirror_bounces = 8; portal_depth = 6; emissive_samples = 2; emissive_requested = 4; emissive_budget = 2; direct_scene = 0; voxel_occurrence_control = 1 }
    },
    [pscustomobject]@{
        name = "candidate-omit"; traceClass = "candidate"; occurrenceMode = "omit"
        cvars = [ordered]@{ nri_ptlightbounces = 2; nri_ptmirrorbounces = 2; nri_ptportaldepth = 3; nri_ptemissivesamples = 1; nri_ptemissiveprimarybudget = 2; nri_ptvoxelomitoccurrences = "true" }
        expectedTrace = [ordered]@{ light_bounces = 2; mirror_bounces = 2; portal_depth = 3; emissive_samples = 1; emissive_requested = 1; emissive_budget = 2; direct_scene = 0; voxel_occurrence_control = 1 }
    }
)
$generatedScenarios = @{}
foreach ($profile in $profiles) {
    foreach ($leg in $legDefinitions) {
        $generatedScenarios["$($profile.profile)/$($leg.name)"] = New-LegScenario -Profile $profile -Leg $leg -Directory $generatedScenarioDirectory
    }
}

if ($PrepareOnly) {
    $plan = [pscustomobject]@{
        schema = 1
        profiles = @($profiles | ForEach-Object { $_.profile })
        legs = @($legDefinitions | ForEach-Object { $_.name })
        generatedScenarios = @($generatedScenarios.Values | Sort-Object profile, leg)
        note = "Preparation only; no game process was launched."
    }
    $plan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
    Write-Host "Direct trace matrix prepared without launch: summary=$SummaryOutput"
    exit 0
}

$runnerPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "run-nri-perf.ps1")).Path
$analyzerPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "analyze-nri-direct-trace-matrix.ps1")).Path
$powershellPath = (Get-Process -Id $PID).Path
$entries = New-Object System.Collections.Generic.List[object]
$sequence = 0
$buildPending = [bool]$Build

for ($cycle = 1; $cycle -le $Runs; ++$cycle) {
    $orderedLegs = if (($cycle % 2) -eq 1) { @($legDefinitions) } else { @($legDefinitions[3..0]) }
    $orderedProfiles = if (($cycle % 2) -eq 1) { @($profiles) } else { @($profiles[($profiles.Count - 1)..0]) }
    foreach ($leg in $orderedLegs) {
        foreach ($profile in $orderedProfiles) {
            $sequence++
            $matrixScenario = $generatedScenarios["$($profile.profile)/$($leg.name)"]
            $legOutput = Join-Path $resolvedOutput ("{0:D2}-{1}-{2}-run-{3:D2}" -f $sequence, $profile.profile, $leg.name, $cycle)
            $baseSummary = Join-Path $legOutput "summary.json"
            $runnerArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runnerPath, "-ScenarioPath", $matrixScenario.path,
                "-RazePath", $RazePath, "-Runs", "1", "-OutputDirectory", $legOutput, "-SummaryOutput", $baseSummary)
            if ($TimeoutSeconds -gt 0) { $runnerArgs += @("-TimeoutSeconds", [string]$TimeoutSeconds) }
            if ($GameGrp) { $runnerArgs += @("-GameGrp", $GameGrp) }
            if ($File) { $runnerArgs += @("-File", $File) }
            if ($buildPending) { $runnerArgs += "-Build"; $buildPending = $false }

            Write-Host "Direct trace matrix: sequence=$sequence cycle=$cycle profile=$($profile.profile) leg=$($leg.name)"
            & $powershellPath @runnerArgs
            $exitCode = $LASTEXITCODE
            $entries.Add([pscustomobject]@{
                sequence = $sequence; cycle = $cycle; profile = $profile.profile; leg = $leg.name
                scenarioPath = $matrixScenario.path; outputDirectory = $legOutput; logPath = Join-Path $legOutput "run-1.log"
                baseSummaryPath = $baseSummary; exitCode = $exitCode
            })
            if ($exitCode -ne 0) {
                [pscustomobject]@{ schema = 1; entries = $entries.ToArray() } | ConvertTo-Json -Depth 12 |
                    Set-Content -LiteralPath (Join-Path $resolvedOutput "matrix-manifest.json") -Encoding UTF8
                throw "Base perf runner failed for sequence $sequence with exit code $exitCode."
            }
        }
    }
}

$manifestPath = Join-Path $resolvedOutput "matrix-manifest.json"
$manifest = [pscustomobject]@{
    schema = 1; generatedUtc = (Get-Date).ToUniversalTime().ToString("o"); runsPerLeg = $Runs
    order = @($entries | ForEach-Object { "$($_.profile)/$($_.leg)" }); entries = $entries.ToArray()
}
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
& $powershellPath -NoProfile -ExecutionPolicy Bypass -File $analyzerPath -ManifestPath $manifestPath -SummaryOutput $SummaryOutput
exit $LASTEXITCODE
