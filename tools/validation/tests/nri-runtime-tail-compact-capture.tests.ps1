$ErrorActionPreference = "Stop"

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message)
    if ($Text -notmatch $Pattern) { throw $Message }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../../..")
$renderer = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_renderer.cpp")
$cvars = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "source/common/rendering/nri/renderer/nri_cvars.h")
$scenario = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "tools/validation/perf-scenarios/compute-voxel-runtime-tail-e3l10-d3d12.json") | ConvertFrom-Json

$release = [regex]::Match(
    $renderer,
    'void NRIRenderer::OnLevelFirstFrameRelease\(\)([\s\S]*?)(?=\nNRIRenderer::LevelTransitionSnapshot)').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($release)) { throw "OnLevelFirstFrameRelease was not found." }

Assert-Match $cvars 'EXTERN_CVAR\(Int, perf_compactframes\)' `
    'The renderer must consume the engine-owned compact capture control through an extern declaration.'
Assert-Match $release 'runtimeCaptureFrames[\s\S]*?perf_looptraceframes\s*=\s*runtimeCaptureFrames;[\s\S]*?perf_compactframes\s*=\s*runtimeCaptureFrames;' `
    'Runtime-tail release must arm loop and compact captures for the same bounded frame count.'
Assert-Match $release 'PERF pt voxel preload runtime tail capture NRI:[^\n]*compact=1' `
    'Runtime-tail capture diagnostics must identify compact GPU capture as armed.'

$args = @($scenario.launch.extraArgs)
$settings = @{}
for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
    if ($args[$index] -eq '+set') { $settings[[string]$args[$index + 1]] = [string]$args[$index + 2] }
}
if ($settings['nri_ptgputiming'] -ne 'true') { throw 'Runtime-tail scenario must enable fence-retired GPU timing.' }
foreach ($prefix in @('PERF pt gpu timing NRI:', 'PERF pt voxel gpu timing NRI:', 'PERF compact capture complete:')) {
    if (@($scenario.requiredPrefixes) -notcontains $prefix) { throw "Runtime-tail scenario is missing required prefix '$prefix'." }
}

Write-Host "NRI runtime-tail compact capture contract tests passed."
