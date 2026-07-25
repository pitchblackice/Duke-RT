param(
    [string]$RazePath = 'build/terminal-ninja/raze.exe',
    [string]$OutputDirectory,
    [string]$SaveDirectory = 'M:/Raze/tools/perf-saves/Duke.WorldTour',
    [string]$SaveName = 'generalchurn01',
    [string]$File = 'M:/Raze/full-voxel-overlay',
    [string]$GameGrp = 'C:/Program Files (x86)/Steam/steamapps/common/Duke Nukem 3D Twentieth Anniversary World Tour/DUKE3D.GRP',
    [int]$SettleTics = 512,
    [int]$TimeoutSeconds = 300,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($SaveName -notmatch '^[A-Za-z0-9_.-]+$') { throw 'SaveName contains unsupported console characters.' }
if ($SettleTics -lt 256) { throw 'SettleTics must be at least 256 for the quality comparison.' }
$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
$resolvedSaveDirectory = Resolve-Path -LiteralPath $SaveDirectory -ErrorAction Stop
if (-not (Test-Path -LiteralPath (Join-Path $resolvedSaveDirectory.Path "$SaveName.dsave"))) {
    throw "Save not found: $SaveName.dsave"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Get-Location) ('tools/logs/trace-profile-quality/{0}-{1}' -f $SaveName, (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

$commonSettings = [ordered]@{
    nri_ptwaitpresent = 'false'
    nri_validation = 'false'
    nri_apivalidation = 'false'
    nri_dred = 'true'
    nri_ptgputiming = 'false'
    nri_ptshaderstats = 'false'
    nri_ptbloom = 'true'
    nri_renderscale = '1'
    nri_upscaler = '0'
    nri_upscalermode = '0'
    nri_denoise = 'true'
    nri_nrddenoiser = '1'
    nri_pttaa = 'false'
    nri_ptdebug = '0'
    nri_ptbootstrap = 'false'
    nri_ptdirectscene = 'false'
    nri_ptdirectionallight = 'true'
    nri_ptvisiblechunkgate = 'true'
    nri_ptemissivefastshadow = 'true'
    nri_ptindirectsampling = '1'
    nri_ptautoexposure = 'true'
    nri_ptautoexposurefreeze = 'false'
    nri_ptslowdowntrace = 'false'
    nri_pttraceframes = '0'
    nri_ptscenestats = 'false'
    nri_voxelstats = 'false'
    vid_defwidth = '1920'
    vid_defheight = '1080'
    vid_fullscreen = 'false'
    vid_vsync = 'false'
    vid_maxfps = '500'
}
$profiles = @(
    [pscustomobject]@{ label = 'default-a'; light = 4; mirror = 8; portal = 6; emissive = 4; budget = 2 },
    [pscustomobject]@{ label = 'candidate'; light = 2; mirror = 2; portal = 3; emissive = 1; budget = 2 },
    [pscustomobject]@{ label = 'default-b'; light = 4; mirror = 8; portal = 6; emissive = 4; budget = 2 }
)

function Add-SettingArguments {
    param([Collections.Generic.List[object]]$Arguments, [Collections.IDictionary]$Settings)
    foreach ($entry in $Settings.GetEnumerator()) {
        $Arguments.Add('+set'); $Arguments.Add([string]$entry.Key); $Arguments.Add([string]$entry.Value)
    }
}

$scenarioPaths = [ordered]@{}
$artifactPaths = [ordered]@{}
foreach ($profile in $profiles) {
    $runDirectory = Join-Path $resolvedOutput $profile.label
    $shotDirectory = Join-Path $runDirectory 'screenshots'
    New-Item -ItemType Directory -Force -Path $shotDirectory | Out-Null
    $shotName = 'trace-profile-' + $profile.label
    $artifactPath = Join-Path $shotDirectory ($shotName + '_0000.png')
    if (Test-Path -LiteralPath $artifactPath) {
        throw "Refusing to mix a new comparison with an existing artifact: $artifactPath"
    }

    $settings = [ordered]@{}
    foreach ($entry in $commonSettings.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    $settings.nri_ptlightbounces = [string]$profile.light
    $settings.nri_ptmirrorbounces = [string]$profile.mirror
    $settings.nri_ptportaldepth = [string]$profile.portal
    $settings.nri_ptemissivesamples = [string]$profile.emissive
    $settings.nri_ptemissiveprimarybudget = [string]$profile.budget
    $settings.screenshot_dir = $shotDirectory.Replace('\', '/')
    $settings.screenshotname = $shotName
    $extraArguments = [Collections.Generic.List[object]]::new()
    Add-SettingArguments -Arguments $extraArguments -Settings $settings

    $scenario = [ordered]@{
        name = 'trace-profile-quality-' + $profile.label
        backend = 'd3d12'
        description = 'Display-referred bracket capture for the current non-SPATIAL direct path; not timing or linear-HDR evidence.'
        limitations = @(
            'The current branch has no nri_pthdrcapture command, so this is a tone-mapped 8-bit PNG comparison.',
            'Separate processes are bracketed by identical default captures to expose temporal and process-start variance.'
        )
        commands = "+wait 45; load $SaveName; wait 35; closemenu; nri_ptautoexposurefreeze false; nri_ptautoexposurereset; nri_ptreset; wait $SettleTics; nri_ptautoexposurefreeze true; wait 8; screenshot; perf_looptraceframes 8"
        save = [ordered]@{ dir = $resolvedSaveDirectory.Path.Replace('\', '/'); name = $SaveName }
        capture = [ordered]@{ loopTraceFrames = 8; timeoutSeconds = $TimeoutSeconds; runs = 1; stopWhenLoopTraceFramesCaptured = $true }
        launch = [ordered]@{ file = $File; gameGrp = $GameGrp; extraArgs = $extraArguments.ToArray() }
        requiredPrefixes = @('PERF loop trace:', 'screenshot saved')
        forbiddenPatterns = @(
            'Device removed', 'device lost', 'DXGI_ERROR_DEVICE', 'QueueSubmit failed',
            'QueuePresent failed', 'AcquireNextTexture(): failed', 'NRI render failed',
            'validation error', 'failed to create', 'assertion failed', 'fatal error',
            'NRI screenshot failed', 'Failed writing screenshot'
        )
        traceProfile = [ordered]@{
            lightBounces = $profile.light
            mirrorBounces = $profile.mirror
            portalDepth = $profile.portal
            emissiveRequestedSamples = $profile.emissive
            emissivePrimaryBudget = $profile.budget
            emissiveEffectiveSamples = [Math]::Min($profile.emissive, $profile.budget)
        }
    }
    $scenarioPath = Join-Path $runDirectory 'scenario.json'
    $scenario | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $scenarioPath -Encoding UTF8
    $scenarioPaths[$profile.label] = $scenarioPath
    $artifactPaths[$profile.label] = $artifactPath
}

$preflight = [ordered]@{
    schema = 1
    qualityEvidence = 'display-referred-tonemapped'
    linearHdrEvidence = $false
    reasonLinearHdrUnavailable = 'The current non-SPATIAL source has no nri_pthdrcapture or nri_pthdrcapturepair command.'
    processOrder = @('default-a', 'candidate', 'default-b')
    saveName = $SaveName
    settleTics = $SettleTics
    scenarios = $scenarioPaths
    expectedArtifacts = $artifactPaths
}
$preflightPath = Join-Path $resolvedOutput 'preflight.json'
$preflight | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $preflightPath -Encoding UTF8
if ($ValidateOnly) {
    Write-Host "NRI trace-profile quality preflight passed: output=$resolvedOutput"
    return
}

$runner = Join-Path $PSScriptRoot 'run-nri-perf.ps1'
$comparator = Join-Path $PSScriptRoot 'compare-nri-srgb-png.ps1'
foreach ($profile in $profiles) {
    $runDirectory = Join-Path (Join-Path $resolvedOutput $profile.label) 'run'
    & $runner -ScenarioPath $scenarioPaths[$profile.label] -RazePath $resolvedRaze.Path `
        -Runs 1 -TimeoutSeconds $TimeoutSeconds -OutputDirectory $runDirectory `
        -SummaryOutput (Join-Path $runDirectory 'summary.json')
    if (-not $?) { throw "Quality capture failed for $($profile.label)." }
    if (-not (Test-Path -LiteralPath $artifactPaths[$profile.label])) {
        throw "Quality capture did not publish $($artifactPaths[$profile.label])."
    }
}

$comparisons = [ordered]@{
    defaultControl = Join-Path $resolvedOutput 'default-a-vs-default-b.json'
    candidateFromDefaultA = Join-Path $resolvedOutput 'default-a-vs-candidate.json'
    candidateFromDefaultB = Join-Path $resolvedOutput 'default-b-vs-candidate.json'
}
& $comparator -ReferencePath $artifactPaths['default-a'] -CandidatePath $artifactPaths['default-b'] `
    -SummaryOutput $comparisons.defaultControl -DifferencePath (Join-Path $resolvedOutput 'default-control-difference-x4.png') | Out-Null
& $comparator -ReferencePath $artifactPaths['default-a'] -CandidatePath $artifactPaths.candidate `
    -SummaryOutput $comparisons.candidateFromDefaultA -DifferencePath (Join-Path $resolvedOutput 'default-a-candidate-difference-x4.png') | Out-Null
& $comparator -ReferencePath $artifactPaths['default-b'] -CandidatePath $artifactPaths.candidate `
    -SummaryOutput $comparisons.candidateFromDefaultB -DifferencePath (Join-Path $resolvedOutput 'default-b-candidate-difference-x4.png') | Out-Null

$result = [ordered]@{
    schema = 1
    ok = $true
    qualityEvidence = 'display-referred-tonemapped'
    linearHdrEvidence = $false
    preflight = $preflightPath
    artifacts = $artifactPaths
    comparisons = $comparisons
    interpretation = 'Treat default-a vs default-b as the noise floor. Candidate differences are credible only where both bracket comparisons materially exceed that control.'
}
$resultPath = Join-Path $resolvedOutput 'result.json'
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "NRI trace-profile quality comparison complete: result=$resultPath"
