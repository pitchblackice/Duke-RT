$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-CvarValue {
    param([object]$Scenario, [string]$Name)
    $args = @($Scenario.launch.extraArgs)
    for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
        if ([string]$args[$index] -eq "+set" -and [string]$args[$index + 1] -eq $Name) {
            return [string]$args[$index + 2]
        }
    }
    return $null
}

function Write-FixtureLog {
    param([string]$Path, [object]$Scenario, [int]$TraceCalls, [int]$BusyDrops)

    $metadata = $Scenario.traceShaderAttribution
    $expected = $metadata.expectedTrace
    $omit = [string]$metadata.leg -like "*-omit"
    $occurrences = if ($omit) { 0 } else { 7 }
    $primitives = if ($omit) { 0 } else { 7000 }
    $source = if ($omit) { "static" } else { "voxel" }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($sample in 0..([int]$metadata.compactIdentityFrames - 1)) {
        $lines.Add("PERF pt trace workload NRI: frame=$($sample + 10) nri_frame=$($sample + 20) renderer_frame=$($sample + 30) schema=3 settings_key=$($TraceCalls + 1) workload_key=$($TraceCalls + 2) light_bounces=$($expected.light_bounces) mirror_bounces=$($expected.mirror_bounces) portal_depth=$($expected.portal_depth) emissive_samples=$($expected.emissive_samples) emissive_requested=$($expected.emissive_requested) emissive_budget=$($expected.emissive_budget) direct_scene=$($expected.direct_scene) voxel_occurrences=$occurrences voxel_instance_prims=$primitives voxel_occurrence_control=$($expected.voxel_occurrence_control) compact=1 epoch=9 sample=$sample")
    }

    $lines.Add("PERF pt shader stats observer NRI: frame=100 stats_frame=0 valid=0 copies=1 recorded=1 busy=0 no_fence=0 published=0 superseded=0 abandoned=0 map_fail=0 pending=1 attribution_rows=10 attribution_bytes=200")
    foreach ($index in 1..3) {
        $copies = $index + 1 + $BusyDrops
        $lines.Add("PERF pt shader stats observer NRI: frame=$($index + 100) stats_frame=$($index + 200) valid=1 copies=$copies recorded=$($index + 1) busy=$BusyDrops no_fence=0 published=$index superseded=0 abandoned=0 map_fail=0 pending=1 attribution_rows=$(($index + 1) * 10) attribution_bytes=$(($index + 1) * 200)")
        $calls = $TraceCalls + $index
        $lines.Add("PERF pt shader trace NRI: frame=$($index + 100) stats_frame=$($index + 200) trace_calls=$calls primary=50 ungated=10 sun=8 point=7 emissive=6 fast_emissive=5 committed=60 miss=40 accept_static=20 accept_dynamic=10 accept_voxel=$occurrences skips=2 max_skip=1 limit=32")
        $lines.Add("PERF pt shader reject NRI: frame=$($index + 100) stats_frame=$($index + 200) reflection=1 visible=2 hidden_flat=3 oneway=4 transparent=5 noshadow=6 reject_static=7 reject_dynamic=8 reject_voxel=9 runtime_candidates=10 runtime_dist=11 runtime_lambert=12 runtime_shadow_rays=13 emissive_samples=14 emissive_shadow_rays=15 instance_committed_overflow=0 instance_accepted_overflow=0")
        $lines.Add("PERF pt shader phase NRI: frame=$($index + 100) stats_frame=$($index + 200) primary_hit=40 primary_miss=10 hit_static=20 hit_dynamic=10 hit_voxel=$occurrences fullbright=1 emissive_material=2 dir_shadow_tests=3 runtime_tile_nonempty=4 runtime_tile_max=5 runtime_shadow_visible=6 runtime_shadow_occluded=7 indirect_diffuse_calls=8 indirect_diffuse_bounces=9 indirect_diffuse_misses=10 indirect_specular_calls=11 indirect_specular_bounces=12 indirect_specular_misses=13 sun_shadow_calls=14 point_shadow_calls=15 fast_emissive_shadow_calls=16 runtime_soft_shadow_samples=17 runtime_soft_transport=18")
        $lines.Add("PERF pt shader emissive detail NRI: frame=$($index + 100) stats_frame=$($index + 200) candidate_none=1 light_zero=2 distance_reject=3 receiver_lambert_reject=4 emitter_lambert_reject=5 visibility_visible=6 visibility_occluded=7 contributed=8 traced_shadow_calls=9 fast_shadow_calls=10")
        $lines.Add("PERF pt shader hot instance NRI: frame=$($index + 100) stats_frame=$($index + 200) rank=1 instance=4 source=$source primitive_offset=100 primitive_count=1000 metadata0=11 metadata1=12 committed=30 accepted=20 primary=15 ungated=5 sun=4 point=3 emissive=2 fast_emissive=1")
    }
    $lines.Add("PERF compact capture complete: epoch=9 status=complete requested=$($metadata.compactIdentityFrames) eligible=$($metadata.compactIdentityFrames) observed=$($metadata.compactIdentityFrames) pending_gpu=0 dropped=0 reason=none")
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$runner = Join-Path $repoRoot "tools/validation/run-nri-trace-shader-attribution.ps1"
$analyzer = Join-Path $repoRoot "tools/validation/analyze-nri-trace-shader-attribution.ps1"
$contract = Join-Path $repoRoot "tools/validation/perf-scenarios/current-direct-trace-shader-attribution-d3d12.json"
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("nri-trace-attribution-tests-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
try {
    $prepareSummary = Join-Path $temporary "prepare.json"
    & $runner -ContractPath $contract -PrepareOnly -OutputDirectory $temporary -SummaryOutput $prepareSummary
    Assert-True ($LASTEXITCODE -eq 0) "Prepare-only attribution runner failed."
    $prepared = Get-Content -LiteralPath $prepareSummary -Raw | ConvertFrom-Json
    Assert-True ([bool]$prepared.preparedOnly) "Prepare-only summary was not marked preparedOnly."
    Assert-True (@($prepared.generatedScenarios).Count -eq 3) "Expected three generated attribution legs."

    $entries = New-Object System.Collections.Generic.List[object]
    $sequence = 0
    foreach ($generated in @($prepared.generatedScenarios | Sort-Object leg)) {
        $sequence++
        $scenario = Get-Content -LiteralPath $generated.path -Raw | ConvertFrom-Json
        $scenario.traceShaderAttribution.minimumPublishedSnapshots = 3
        $scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $generated.path -Encoding UTF8
        Assert-True ((Get-CvarValue $scenario "nri_ptshaderstats") -eq "true") "Shader stats were not enabled."
        Assert-True ((Get-CvarValue $scenario "nri_ptgputiming") -eq "false") "Attribution scenario retained GPU timing."
        Assert-True ((Get-CvarValue $scenario "nri_ptvoxelcomputepreloadterminalcommand") -eq $scenario.traceShaderAttribution.terminalCommand) "Terminal command metadata drifted."
        Assert-True ([int]$scenario.capture.loopTraceFrames -eq 68) "Bounded stop count must cover the overlapping attribution plus drain window."

        $logPath = Join-Path $temporary ("$($generated.leg).log")
        $baseSummary = Join-Path $temporary ("$($generated.leg).base.json")
        '{"ok":true}' | Set-Content -LiteralPath $baseSummary -Encoding UTF8
        $traceCalls = switch ([string]$generated.leg) {
            "default-full" { 100 }
            "default-omit" { 80 }
            "candidate-full" { 70 }
            default { throw "Unexpected generated leg." }
        }
        $busy = if ([string]$generated.leg -eq "default-full") { 1 } else { 0 }
        Write-FixtureLog -Path $logPath -Scenario $scenario -TraceCalls $traceCalls -BusyDrops $busy
        $entries.Add([pscustomobject]@{
            sequence = $sequence
            cycle = 1
            leg = [string]$generated.leg
            scenarioPath = [string]$generated.path
            logPath = $logPath
            baseSummaryPath = $baseSummary
            exitCode = 0
        })
    }

    $manifestPath = Join-Path $temporary "manifest.json"
    [pscustomobject]@{
        schema = 1
        runCommand = "fixture"
        entries = $entries.ToArray()
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $summaryPath = Join-Path $temporary "summary.json"
    & $analyzer -ManifestPath $manifestPath -SummaryOutput $summaryPath
    Assert-True ($LASTEXITCODE -eq 0) "Attribution analyzer rejected the valid fixture."
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    Assert-True ([bool]$summary.ok) "Attribution summary was not successful."
    $defaultFull = @($summary.legs | Where-Object leg -eq "default-full")[0]
    Assert-True ([int]$defaultFull.snapshotCount -eq 3) "Published shader snapshots were not parsed."
    Assert-True ([uint64]$defaultFull.observer.busyDrops -eq 1) "Observer busy drops were not reported."
    Assert-True ([uint64]$defaultFull.observer.attributionBytes -eq 800) "Attribution metadata bytes were not reported."
    Assert-True ([uint64]$defaultFull.observer.attributionBytesPerRow -eq 20) "Attribution metadata row size was not inferred."
    Assert-True ([double]$summary.comparisonsP50.defaultFullMinusDefaultOmit.trace.trace_calls -eq 20) "Default full/omit trace-call delta is wrong."
    Assert-True ([double]$summary.comparisonsP50.candidateFullMinusDefaultFull.trace.trace_calls -eq -30) "Candidate/default trace-call delta is wrong."
    Assert-True (@($defaultFull.hotSources | Where-Object source -eq "voxel").Count -eq 1) "Voxel hot-instance source attribution was not aggregated."

    $brokenLog = [string]$entries[0].logPath
    (Get-Content -LiteralPath $brokenLog) |
        Where-Object { -not $_.StartsWith("PERF pt shader emissive detail NRI:", [StringComparison]::Ordinal) } |
        Set-Content -LiteralPath $brokenLog -Encoding UTF8
    $rejected = $false
    try {
        & $analyzer -ManifestPath $manifestPath -SummaryOutput (Join-Path $temporary "broken.json") 2>$null
    }
    catch {
        $rejected = $true
    }
    Assert-True $rejected "Analyzer accepted a missing shader row family."

    Write-Host "Trace shader attribution tests passed."
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
