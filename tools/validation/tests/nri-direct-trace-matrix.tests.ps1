Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-GeneratedCvar([object[]]$Arguments, [string]$Name) {
    for ($index = 0; $index + 2 -lt $Arguments.Count; ++$index) {
        if ($Arguments[$index] -eq "+set" -and $Arguments[$index + 1] -eq $Name) { return [string]$Arguments[$index + 2] }
    }
    throw "Generated scenario is missing cvar '$Name'."
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$runner = Join-Path $root "tools/validation/run-nri-direct-trace-matrix.ps1"
$analyzer = Join-Path $root "tools/validation/analyze-nri-direct-trace-matrix.ps1"
$generalScenarioPath = Join-Path $root "tools/validation/perf-scenarios/current-direct-trace-general-churn-d3d12.json"
$largeVoxelScenarioPath = Join-Path $root "tools/validation/perf-scenarios/current-direct-trace-large-voxel-d3d12.json"

$runnerText = Get-Content -LiteralPath $runner -Raw
Assert-True ($runnerText.Contains("run-nri-perf.ps1")) "Matrix must reuse the committed base runner."
Assert-True ($runnerText.Contains("targetFrameMs")) "Matrix must preserve the scenario frame-time target."
Assert-True (-not $runnerText.Contains("-AdditionalArgs")) "Matrix must not depend on the dirty AdditionalArgs runner extension."
Assert-True (-not $runnerText.Contains("stopWhenPrefix")) "Matrix must not depend on the dirty terminal-prefix runner extension."

$generalScenario = Get-Content -LiteralPath $generalScenarioPath -Raw | ConvertFrom-Json
$largeVoxelScenario = Get-Content -LiteralPath $largeVoxelScenarioPath -Raw | ConvertFrom-Json
foreach ($scenario in @($generalScenario, $largeVoxelScenario)) {
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$scenario.directTraceProfile)) "Profile scenario must declare directTraceProfile."
    Assert-True ([bool]$scenario.capture.stopWhenLoopTraceFramesCaptured) "Profile scenario must use HEAD-compatible loop stopping."
    Assert-True ((@($scenario.requiredPrefixes) -contains "PERF pt voxel gpu timing NRI:")) "Profile scenario must require voxel GPU timing."
}
Assert-True ($runnerText.Contains('name = "default-full"')) "Matrix must include the default/full leg."
Assert-True ($runnerText.Contains('name = "candidate-full"')) "Matrix must include the candidate/full leg."
Assert-True ($runnerText.Contains('name = "default-omit"')) "Matrix must include the default/omit leg."
Assert-True ($runnerText.Contains('name = "candidate-omit"')) "Matrix must include the candidate/omit leg."

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("nri-direct-trace-matrix-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $preparedDirectory = Join-Path $temp "prepared"
    $preparedSummary = Join-Path $temp "prepared.json"
    & $runner -Runs 1 -PrepareOnly -OutputDirectory $preparedDirectory -SummaryOutput $preparedSummary
    $prepared = Get-Content -LiteralPath $preparedSummary -Raw | ConvertFrom-Json
    Assert-True (@($prepared.generatedScenarios).Count -eq 8) "Prepare-only must generate four occurrence/trace legs for both default profiles."
    foreach ($generated in @($prepared.generatedScenarios)) {
        $generatedScenario = Get-Content -LiteralPath $generated.path -Raw | ConvertFrom-Json
        Assert-True ([int]$generatedScenario.directTraceMatrix.minimumWorkloadSchema -eq 3) "Generated scenarios must require workload schema 3."
        Assert-True ((@($generatedScenario.requiredPrefixes) -contains "PERF pt voxel gpu timing NRI:")) "Generated scenarios must require voxel GPU timing."
        $launchArgs = @($generatedScenario.launch.extraArgs)
        Assert-True ((Get-GeneratedCvar -Arguments $launchArgs -Name "nri_ptlightbounces") -eq [string]$generatedScenario.directTraceMatrix.expectedTrace.light_bounces) "Generated light-bounce cvar does not match identity."
        Assert-True ((Get-GeneratedCvar -Arguments $launchArgs -Name "nri_ptmirrorbounces") -eq [string]$generatedScenario.directTraceMatrix.expectedTrace.mirror_bounces) "Generated mirror-bounce cvar does not match identity."
        Assert-True ((Get-GeneratedCvar -Arguments $launchArgs -Name "nri_ptportaldepth") -eq [string]$generatedScenario.directTraceMatrix.expectedTrace.portal_depth) "Generated portal-depth cvar does not match identity."
        Assert-True ((Get-GeneratedCvar -Arguments $launchArgs -Name "nri_ptemissivesamples") -eq [string]$generatedScenario.directTraceMatrix.expectedTrace.emissive_requested) "Generated emissive cvar does not match identity."
    }

    function New-FixtureScenario([string]$Leg, [string]$TraceClass, [string]$OccurrenceMode) {
        $scenario = $generalScenario | ConvertTo-Json -Depth 24 | ConvertFrom-Json
        $isDefault = $TraceClass -eq "default"
        $isOmit = $OccurrenceMode -eq "omit"
        $matrixMetadata = [pscustomobject]@{
            profile = "fixture"; leg = $Leg; traceClass = $TraceClass; occurrenceMode = $OccurrenceMode; targetFrameMs = 16.667; minimumWorkloadSchema = 3
            expectedTrace = [pscustomobject]@{
                light_bounces = if ($isDefault) { 4 } else { 2 }
                mirror_bounces = if ($isDefault) { 8 } else { 2 }
                portal_depth = if ($isDefault) { 6 } else { 3 }
                emissive_samples = if ($isDefault) { 2 } else { 1 }
                emissive_requested = if ($isDefault) { 4 } else { 1 }
                emissive_budget = 2; direct_scene = 0; voxel_occurrence_control = if ($isOmit) { 1 } else { 0 }
            }
        }
        $scenario | Add-Member -NotePropertyName directTraceMatrix -NotePropertyValue $matrixMetadata
        $path = Join-Path $temp ("scenario-$Leg.json")
        $scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
        return $path
    }

    function Write-FixtureRun([string]$Leg, [int]$Sequence, [double]$BaseMs, [string]$SettingsKey, [string]$WorkloadKey, [string]$ScenarioPath) {
        $directory = Join-Path $temp ("run-$Sequence")
        New-Item -ItemType Directory -Path $directory | Out-Null
        $logPath = Join-Path $directory "run-1.log"
        $summaryPath = Join-Path $directory "summary.json"
        [pscustomobject]@{ ok = $true } | ConvertTo-Json | Set-Content -LiteralPath $summaryPath -Encoding UTF8
        $lines = New-Object System.Collections.Generic.List[string]
        for ($sample = 0; $sample -lt 4; ++$sample) {
            $frame = 100 + $sample
            $segment = $BaseMs + $sample
            $trace = $segment - 2.0
            $scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
            $expected = $scenario.directTraceMatrix.expectedTrace
            $occurrences = if ([int]$expected.voxel_occurrence_control -eq 1) { 0 } else { 136 }
            $instancePrims = if ($occurrences -eq 0) { 0 } else { 5000000 }
            $lines.Add("PERF pt trace workload NRI: frame=$frame nri_frame=$frame renderer_frame=$frame schema=3 settings_key=$SettingsKey workload_key=$WorkloadKey render_w=1920 render_h=1080 output_w=1920 output_h=1080 dispatch_x=240 dispatch_y=135 dispatch_z=1 light_bounces=$($expected.light_bounces) mirror_bounces=$($expected.mirror_bounces) portal_depth=$($expected.portal_depth) emissive_samples=$($expected.emissive_samples) emissive_requested=$($expected.emissive_requested) emissive_budget=$($expected.emissive_budget) indirect_requested=1 indirect_effective=1 indirect_active=1 hit_recon=1 runtime_lights=0 light_tiles_x=0 light_tiles_y=0 light_tile_size=64 light_tile_indices=0 light_tile_max=0 emissive_prims=280 emissive_power=458000.406 flags=4096 debug=0 bootstrap=0 upscaler=0 upscaler_mode=0 denoiser=1 direct_scene=0 directional=1 directional_shadow=1 split_shadow=1 fast_emissive_shadow=1 visible_chunk_gate=1 voxel_occurrences=$occurrences voxel_instance_prims=$instancePrims voxel_occurrence_control=$($expected.voxel_occurrence_control) compact=1 epoch=9 sample=$sample")
            $lines.Add("PERF pt gpu timing NRI: frame=$frame nri_frame=$frame segment=$segment scene=$($segment - 0.1) trace=$trace trace_dispatch=$($trace - 0.01) denoise=1.5 compose=0.1 upscale=0.0 final=0.0 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=9 sample=$sample")
            $lines.Add("PERF pt voxel gpu timing NRI: renderer_frame=$frame presentation_gen=20 queued_slot=0 segment=$segment segment_valid=1 admission=0.0 upload=0.0 arena_copy=0.0 classify=0.0 scan=0.0 emit=0.0 finalize=0.0 voxel_blas=0.0 world_tlas=0.1 scopes=1 valid=1 invalid=0 dropped=0 compact=1 epoch=9 record=$sample")
        }
        $lines.Add("PERF compact capture complete: status=complete requested=4 eligible=4 pending_gpu=0 dropped=0 epoch=9")
        $lines | Set-Content -LiteralPath $logPath -Encoding UTF8
        return [pscustomobject]@{
            sequence = $Sequence; cycle = 1; profile = "fixture"; leg = $Leg; scenarioPath = $ScenarioPath
            outputDirectory = $directory; logPath = $logPath; baseSummaryPath = $summaryPath; exitCode = 0
        }
    }

    $defaultFullPath = New-FixtureScenario -Leg "default-full" -TraceClass "default" -OccurrenceMode "full"
    $candidateFullPath = New-FixtureScenario -Leg "candidate-full" -TraceClass "candidate" -OccurrenceMode "full"
    $defaultOmitPath = New-FixtureScenario -Leg "default-omit" -TraceClass "default" -OccurrenceMode "omit"
    $candidateOmitPath = New-FixtureScenario -Leg "candidate-omit" -TraceClass "candidate" -OccurrenceMode "omit"
    $entries = @(
        (Write-FixtureRun -Leg "default-full" -Sequence 1 -BaseMs 20.0 -SettingsKey "18446744073709551601" -WorkloadKey "18446744073709551501" -ScenarioPath $defaultFullPath),
        (Write-FixtureRun -Leg "candidate-full" -Sequence 2 -BaseMs 10.0 -SettingsKey "18446744073709551602" -WorkloadKey "18446744073709551502" -ScenarioPath $candidateFullPath),
        (Write-FixtureRun -Leg "default-omit" -Sequence 3 -BaseMs 12.0 -SettingsKey "18446744073709551603" -WorkloadKey "18446744073709551503" -ScenarioPath $defaultOmitPath),
        (Write-FixtureRun -Leg "candidate-omit" -Sequence 4 -BaseMs 7.0 -SettingsKey "18446744073709551604" -WorkloadKey "18446744073709551504" -ScenarioPath $candidateOmitPath)
    )
    $manifestPath = Join-Path $temp "manifest.json"
    $outputPath = Join-Path $temp "summary.json"
    [pscustomobject]@{ schema = 1; entries = $entries } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    & $analyzer -ManifestPath $manifestPath -SummaryOutput $outputPath
    $result = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    Assert-True ([bool]$result.ok) "Fixture matrix must pass."
    $profile = $result.profiles[0]
    Assert-True ([string]$profile.defaultFull.settingsKey -eq "18446744073709551601") "Exact uint64 settings key was not retained."
    Assert-True ([double]$profile.defaultFull.completeGpu.p50 -eq 21.0) "Default/full complete-GPU p50 is wrong."
    Assert-True ([int]$profile.defaultFull.completeGpu.overTargetCount -eq 4) "Target-miss accounting is wrong."
    Assert-True ([double]$profile.candidateFull.traceDispatch.p99 -eq 10.99) "Candidate/full TraceDispatch p99 is wrong."
    Assert-True ([double]$profile.deltasMs.candidateMinusDefaultFull.completeGpu.p50 -eq -10.0) "Candidate/default delta is wrong."
    Assert-True ([double]$profile.deltasMs.fullMinusOmitDefault.completeGpu.p50 -eq 8.0) "Full/omit delta is wrong."
    Assert-True ([double]$profile.defaultFull.voxelGpu.world_tlas.p95 -eq 0.1) "Voxel timing summary is wrong."
    Assert-True ([int]$profile.defaultFull.voxelGpuValidity.valid -eq 4) "Voxel timing validity summary is wrong."

    foreach ($scenarioPath in @($defaultFullPath, $candidateFullPath, $defaultOmitPath, $candidateOmitPath)) {
        $legacyScenario = Get-Content -LiteralPath $scenarioPath -Raw | ConvertFrom-Json
        $legacyScenario.directTraceMatrix.minimumWorkloadSchema = 2
        $legacyScenario.directTraceMatrix.expectedTrace.PSObject.Properties.Remove("voxel_occurrence_control")
        $legacyScenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $scenarioPath -Encoding UTF8
    }
    foreach ($entry in $entries) {
        $legacyLog = (Get-Content -LiteralPath $entry.logPath -Raw).Replace(" schema=3 ", " schema=2 ")
        $legacyLog = $legacyLog -replace ' voxel_occurrences=\d+ voxel_instance_prims=\d+ voxel_occurrence_control=\d+', ''
        $legacyLog | Set-Content -LiteralPath $entry.logPath -Encoding UTF8
    }
    $legacyOutputPath = Join-Path $temp "summary-schema2.json"
    & $analyzer -ManifestPath $manifestPath -SummaryOutput $legacyOutputPath
    $legacyResult = Get-Content -LiteralPath $legacyOutputPath -Raw | ConvertFrom-Json
    Assert-True ([bool]$legacyResult.ok) "Schema-2 compatibility fixture must pass."
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}

Write-Host "nri-direct-trace-matrix tests passed"
