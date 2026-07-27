Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$manifestPath = Join-Path $repo "tools/validation/smoke-performance-fixtures.json"
$runnerPath = Join-Path $repo "tools/validation/run-smoke-performance-fixtures.ps1"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$runner = Get-Content -LiteralPath $runnerPath -Raw

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Get-CaseScenario([string]$Id) {
    $case = @($manifest.cases | Where-Object { [string]$_.id -eq $Id }) | Select-Object -First 1
    Assert-True ($null -ne $case) "fixture manifest is missing case '$Id'"
    $path = Join-Path $repo ([string]$case.scenario)
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "fixture case '$Id' scenario is missing"
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

Assert-True ([int]$manifest.version -eq 1) "fixture manifest version must remain explicit"
Assert-True ([string]$manifest.saveDirectory -eq "M:/Raze/tools/perf-saves/Duke.WorldTour") "fixture save directory drifted"
Assert-True (@($manifest.saves).Count -eq 2) "fixture manifest must pin exactly the two curated saves"
Assert-True (@($manifest.cases).Count -eq 6) "fixture manifest must retain all six smoke cases"

$offscreenSave = @($manifest.saves | Where-Object { [string]$_.id -eq "offscreen-smoke" }) | Select-Object -First 1
$occludedSave = @($manifest.saves | Where-Object { [string]$_.id -eq "occluded-smoke" }) | Select-Object -First 1
Assert-True ([string]$offscreenSave.name -eq "smoke-offscreen" -and [string]$offscreenSave.title -eq "OffscreenSmoke") "offscreen save identity drifted"
Assert-True ([string]$offscreenSave.sha256 -eq "c5e62f4e85b1200746cbe29b915ef433e278aec45bfeb580187bd2b5b387ddd3") "offscreen save hash drifted"
Assert-True ([string]$occludedSave.name -eq "smoke-occluded" -and [string]$occludedSave.title -eq "OccludedSmoke") "occluded save identity drifted"
Assert-True ([string]$occludedSave.sha256 -eq "75c598c0e4a9c99426a5e6b26b87f3e9b4203a85c15d49140c489ada0e55b9b5") "occluded save hash drifted"

$expectedCommands = @{
    offscreen = "+wait 45; load smoke-offscreen; wait 35; closemenu; wait 1; centerview; wait 334; nri_ptsmokestatus; perf_compactframes 192"
    visible = "+wait 45; load smoke-offscreen; wait 35; closemenu; wait 1; centerview; wait 299; turnaround; wait 35; nri_ptsmokestatus; perf_compactframes 192"
    reentry = "+wait 45; load smoke-offscreen; wait 35; closemenu; wait 1; centerview; wait 299; nri_ptsmokestatus; perf_compactframes 192; turnaround; wait 35; nri_ptsmokestatus"
    occluded = "+wait 45; load smoke-occluded; wait 35; closemenu; wait 335; nri_ptsmokestatus; perf_compactframes 192"
}

$commonPins = @(
    "vid_defwidth 1920", "vid_defheight 1080", "use_mouse false", "use_joystick false", "cl_viewbob 0",
    "nri_ptgputiming true", "nri_ptshaderstats false", "nri_ptlightbounces 2", "nri_ptmirrorbounces 2",
    "nri_ptportaldepth 3", "nri_ptemissivesamples 1", "nri_ptemissiveprimarybudget 2",
    "nri_ptvoxelblaspolicy 3", "nri_ptvoxelblascompact false", "nri_ptvoxelshadowproxybuild false",
    "nri_ptvoxelshadowproxyroute false", "nri_ptindirectradiancecache false", "nri_ptindirectradiancecacheaccept false",
    "nri_ptsmoke true", "nri_ptsmokerepresentation 1", "nri_ptsmokereadback true",
    "nri_ptsmokedirectreference 1", "nri_ptsmoketimescale 1", "nri_ptsmokesimrate 60",
    "nri_ptsmokemaxsubsteps 4", "nri_ptsmokequality 2", "nri_ptsmokelightmode 3",
    "nri_ptsmokelightsamples 4", "nri_ptsmokemaxlightcandidates 8", "nri_ptsmokepointlights true",
    "nri_ptsmokedirectionallight true", "nri_ptsmokeemissivelights true", "nri_ptsmokeemissivepoints 4",
    "nri_ptsmokeemissivebackend 2",
    "nri_ptsmokeemissivereference false", "nri_ptsmokeemissiveworldfilter false", "nri_ptsmokeemissivelocal true",
    "nri_ptsmokedirectreuse 2", "nri_ptsmokevolumehistory true", "nri_ptsmokeindirect false",
    "nri_ptsmokemultiplescatter false", "nri_ptsmokeselfshadow false",
    "nri_ptsmokefilteredvisibility true", "nri_ptsmokegridreclaimgrace 120"
)

$currentTimingFields = @(
    "PERF pt gpu timing NRI/smoke_total", "PERF pt smoke gpu timing NRI/detail_total",
    "PERF pt smoke gpu timing NRI/unattributed", "PERF pt smoke gpu timing NRI/grid_allocate",
    "PERF pt smoke gpu timing NRI/grid_initialize", "PERF pt smoke gpu timing NRI/grid_deposit",
    "PERF pt smoke gpu timing NRI/grid_halo", "PERF pt smoke gpu timing NRI/grid_simulate",
    "PERF pt smoke gpu timing NRI/grid_rebuild", "PERF pt smoke gpu timing NRI/world_active",
    "PERF pt smoke gpu timing NRI/world_link", "PERF pt smoke gpu timing NRI/world_proposal",
    "PERF pt smoke gpu timing NRI/world_seed", "PERF pt smoke gpu timing NRI/world_temporal",
    "PERF pt smoke gpu timing NRI/world_filter", "PERF pt smoke gpu timing NRI/world_scatter",
    "PERF pt smoke gpu timing NRI/carrier", "PERF pt smoke gpu timing NRI/view_prepare",
    "PERF pt smoke gpu timing NRI/materialize", "PERF pt smoke gpu timing NRI/view_point",
    "PERF pt smoke gpu timing NRI/view_directional", "PERF pt smoke gpu timing NRI/view_direct_reuse",
    "PERF pt smoke gpu timing NRI/view_emissive", "PERF pt smoke gpu timing NRI/view_indirect",
    "PERF pt smoke gpu timing NRI/integrate", "PERF pt smoke gpu timing NRI/reconstruction",
    "PERF pt smoke gpu timing NRI/invalid", "PERF pt smoke gpu timing NRI/dropped"
)

foreach ($id in @("offscreen", "visible", "reentry", "occluded", "saturation", "rpg-overload")) {
    $scenario = Get-CaseScenario $id
    $argsText = @($scenario.launch.extraArgs) -join " "
    foreach ($required in $commonPins) {
        Assert-Match $argsText ([regex]::Escape($required)) "fixture case '$id' is missing pinned argument '$required'"
    }
    foreach ($prefix in @("PERF loop trace:", "PERF pt gpu timing NRI:", "PERF pt smoke gpu timing NRI:", "NRI PT smoke grid status:", "PERF compact capture complete:")) {
        Assert-True (@($scenario.requiredPrefixes) -contains $prefix) "fixture case '$id' must require telemetry prefix '$prefix'"
    }
    foreach ($field in $currentTimingFields) {
        Assert-True (@($scenario.baselineCompare.fields) -contains $field) "fixture case '$id' must retain current timing field '$field'"
    }
    Assert-True (@($scenario.forbiddenPatterns) -contains "Unknown command") "fixture case '$id' must reject malformed console commands"
    Assert-True (@($scenario.forbiddenPatterns) -contains "Failed to open savegame") "fixture case '$id' must reject save-load failure"
    Assert-True ($argsText -notmatch "nri_ptsmokeworldcells|nri_ptsmokeviewcompact|nri_ptsmokespawnqueue") "fixture case '$id' must not pin unavailable smoke-follow-up controls"
}

foreach ($id in @("offscreen", "visible", "reentry", "occluded")) {
    $scenario = Get-CaseScenario $id
    Assert-True ([string]$scenario.commands -eq $expectedCommands[$id]) "fixture case '$id' command timeline drifted"
    Assert-True ([int]$scenario.capture.loopTraceFrames -eq 192 -and [int]$scenario.capture.runs -eq 3) "fixture case '$id' capture contract drifted"
    Assert-Match (@($scenario.launch.extraArgs) -join " ") "nri_ptsmokegridbricks 512" "fixture case '$id' must retain production grid capacity"
}

$occluded = Get-CaseScenario "occluded"
Assert-True ([string]$occluded.commands -notmatch "centerview|turnaround") "occluded fixture must preserve its authored pitch and yaw"

$saturation = Get-CaseScenario "saturation"
$saturationArgs = @($saturation.launch.extraArgs) -join " "
Assert-Match $saturationArgs "nri_ptsmokegridbricks 64" "saturation fixture must force the minimum grid capacity"
Assert-Match $saturationArgs "nri_ptsmokegridcellsize 4" "saturation fixture must increase simultaneous brick demand"
Assert-Match $saturationArgs "nri_ptsmoketrace 1" "saturation fixture must trace the four map emitters"
Assert-Match ([string]$saturation.commands) "nri_ptsmokestatus; perf_compactframes 192$" "saturation fixture must publish a pressure status before capture"

$rpg = Get-CaseScenario "rpg-overload"
$rpgArgs = @($rpg.launch.extraArgs) -join " "
Assert-Match $rpgArgs "nri_ptsmokegridbricks 64" "RPG fixture must run under deterministic allocation pressure"
Assert-Match $rpgArgs "nri_ptsmokegridcellsize 4" "RPG fixture must inherit the deterministic pressure resolution"
Assert-Match $rpgArgs "nri_ptsmoketrace 2" "RPG fixture must trace the concrete actor rule"
Assert-Match ([string]$rpg.commands) "slot 5;[\s\S]*?\+Fire; wait 8; -Fire" "RPG fixture must explicitly select and pulse the RPG"
Assert-Match ([string]$rpg.commands) "-Fire; wait 60; nri_ptsmokestatus$" "RPG status must publish while the compact capture remains live"
Assert-True (@($rpg.requiredPrefixes) -contains "NRI PT smoke emitter: event=frame-summary rule=duke_rpg_trail_continuous ") "RPG fixture must require the concrete continuous trail rule"

Assert-Match $runner "Get-FileHash -Algorithm SHA256" "fixture runner must verify save bytes"
Assert-Match $runner "Get-SaveInfo" "fixture runner must verify internal save metadata"
Assert-Match $runner "Assert-SmokeGridStatus" "fixture runner must validate current-branch grid status"
Assert-Match $runner "allocation_failures[\s\S]*rejected_mass_q[\s\S]*saturated" "fixture runner must verify deterministic pressure without future admission counters"
Assert-Match $runner "PERF pt gpu timing NRI/smoke_total" "fixture runner must require joined coarse smoke timing"
Assert-Match $runner "PERF pt smoke gpu timing NRI/detail_total" "fixture runner must require detailed smoke timing"
Assert-Match $runner "Smoke GPU characterization:" "fixture runner must report timing without enforcing a rejected quality budget"
Assert-Match $runner "expected four emitting map rules" "fixture runner must verify all four E1L1 emitters are active"
Assert-Match $runner "rule=duke_rpg_trail_continuous .* emitted=\[1-9\]" "fixture runner must verify nonzero RPG trail emission in every run log"
Assert-Match $runner '\[string\[\]\]\$AdditionalArgs\s*=\s*@\(\)' "fixture runner must accept profile/calibration overrides"
Assert-Match $runner '\$arguments\.AdditionalArgs\s*=\s*\$AdditionalArgs' "fixture runner must forward profile/calibration overrides"
Assert-Match $runner 'timingFrames\.Count\s+-lt\s+\$ExpectedSamples' "fixture runner must derive its join sample floor from the scenario"
Assert-True ($runner -notmatch "grid_admission_|Assert-SourceServed|queue_cap|SmokeGpuBudgetMs") "fixture runner must not claim future fairness/scheduler telemetry"

Write-Host "Smoke performance fixture contract tests passed."
