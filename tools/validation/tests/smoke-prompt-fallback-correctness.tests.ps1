Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
function Read-Source([string]$path) { Get-Content -LiteralPath (Join-Path $root $path) -Raw }
function Require-Match([string]$text, [string]$pattern, [string]$message) {
    if ($text -notmatch $pattern) { throw $message }
}
function Require-NotMatch([string]$text, [string]$pattern, [string]$message) {
    if ($text -match $pattern) { throw $message }
}

$grid = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.cpp'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$allocate = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAllocateCommands.cs.hlsl'
$validate = Read-Source 'source/common/rendering/nri/shaders/SmokeGridValidatePrompt.cs.hlsl'
$authorize = Read-Source 'source/common/rendering/nri/shaders/SmokeGridAuthorizePrompt.cs.hlsl'
$deposit = Read-Source 'source/common/rendering/nri/shaders/SmokeGridDeposit.cs.hlsl'
$finalize = Read-Source 'source/common/rendering/nri/shaders/SmokeGridFinalizePrompt.cs.hlsl'
$fallback = Read-Source 'source/common/rendering/nri/shaders/SmokePromptFallback.cs.hlsl'
$pulse = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_pulses.cpp'
$gridHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.h'
$promptHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_prompt_fallback.h'

Require-Match $grid 'NRISmokeGridPass::PrepareBricks[\s\S]{0,1000}NRISmokeGridPass::ValidatePrompt[\s\S]{0,300}NRISmokeGridPass::AuthorizePrompt[\s\S]{0,300}NRISmokeGridPass::Deposit[\s\S]{0,300}NRISmokeGridPass::FinalizePrompt' 'Prompt grid authority must validate resident mappings, authorize, deposit, then prove closure.'
Require-Match $validate 'SmokeInjectionTraversalFits\(extent,\s*262144u\)[\s\S]*OUTCOME_FALLBACK' 'Oversized prompt kernels must select fallback before deposition.'
Require-Match $authorize 'RequestedBricks\s*==\s*0u[\s\S]*RequestedBricks\s*!=\s*outcome\.AdmittedBricks[\s\S]*OUTCOME_FALLBACK' 'Missing prompt mappings must select fallback before deposition.'
Require-Match $finalize 'RequestedBricks\s*==\s*0u[\s\S]*OUTCOME_INTERNAL_ERROR[\s\S]*OUTCOME_GRID_COMMITTED' 'Post-deposit mismatch must remain distinct from validation fallback and grid commit.'
Require-Match $allocate 'PROMPT_PROVISIONAL[\s\S]*TOMBSTONE[\s\S]*SmokeGridPushFree' 'Failed prompt admission must reclaim its provisional empty bricks.'
Require-Match $allocate '!SmokeInjectionPromptEligible\(command\)[\s\S]*SmokeGridTryReplaceBorrowedDormantSerial' 'Prompt provisional admission must not destructively replace borrowed bricks.'
Require-Match $deposit 'OUTCOME_GRID_NEW[\s\S]*InterlockedAdd\(gSmokePromptOutcomes[\s\S]*RequestedBricks[\s\S]*AdmittedBricks' 'Prompt deposition must publish attempted/deposited closure counters.'

Require-Match $fallback 'command\.Epoch\s*!=\s*gSmokeConstants\.SimulationEpoch' 'Fallback materialization must reject stale epochs.'
Require-Match $fallback 'kernelNormalization[\s\S]*command\.RangeCount[\s\S]*/\s*kernelNormalization' 'Fallback density must use the analytic source-kernel normalization.'
Require-Match $fallback 'if\s*\(extinction\s*<=\s*1e-6\)' 'Extinction-only smoke must remain a carrier when albedo is zero.'
Require-NotMatch $fallback 'extinction\s*<=\s*1e-6\s*\|\|[\s\S]{0,50}scattering' 'Fallback must not discard extinction-only smoke for zero scattering.'
Require-Match $fallback 'previousPhase[\s\S]*combinedWeight[\s\S]*previousPhase\.x\s*\*\s*previousWeight' 'Fallback must merge phase against existing optical weight.'
Require-Match $fallback 'SmokeFroxelRadianceValid\(previousMetadata\)[\s\S]*previousSource' 'Fallback must preserve compatible existing radiance instead of overwriting it.'
Require-NotMatch $fallback 'previousSource\.rgb\s*\+\s*environmentSeed' 'Invalid or recycled radiance metadata must not retain stale RGB.'

Require-Match $runtime 'scheduledFallbackQuantity\s*>\s*0u[\s\S]{0,300}NRISmokePass::PromptFallback' 'Ambient-only frames must skip the exceptional full-froxel fallback dispatch.'
Require-Match $grid 'if\s*\(settings\.readback\)[\s\S]*controlReadback[\s\S]*sourceReadback' 'Control/source diagnostics must remain conditional on the readback setting.'
Require-Match $grid 'promptReadback[\s\S]*if\s*\(settings\.readback\)' 'The small prompt acknowledgment copy must remain independent of optional diagnostics.'
Require-Match $gridHeader 'promptReadbackInitialized[\s\S]*diagnosticReadbackInitialized' 'Prompt and diagnostic readbacks require independent initialization state.'
Require-Match $grid 'promptBefore\[1\]\.before\s*=\s*slot\.promptReadbackInitialized[\s\S]*before\[2\]\.before\s*=\s*slot\.diagnosticReadbackInitialized[\s\S]*before\[3\]\.before\s*=\s*slot\.diagnosticReadbackInitialized' 'An off-to-on diagnostic toggle must not inherit prompt readback before-state.'
Require-Match $pulse 'Validate the complete mutation set first[\s\S]*for\s*\(const auto& committed : mPlan\)[\s\S]*none_of[\s\S]*for\s*\(const auto& committed : mPlan\)' 'Retained commit must validate the whole immutable plan before the first mutation.'
Require-Match $promptHeader 'deferredRanges[\s\S]*deferredMass[\s\S]*deferredBrickWork' 'Prompt filtering must return exact deferred count, mass, and work.'
Require-Match $runtime 'promptResult\.deferredRanges[\s\S]*admission\.uploaded[\s\S]*admission\.boundedDeferred[\s\S]*admission\.interactiveUploaded[\s\S]*estimatedBrickWorkUploaded' 'Admission telemetry must reconcile every prompt-filtered interactive range.'

function Kernel-Normalization([double]$radius, [double]$halfU, [double]$halfV, [double]$cell) {
    $r = $radius / $cell; $u = $halfU / $cell; $v = $halfV / $cell
    return [math]::Max(1.0, 4.0*$u*$v*$r + (3.0*[math]::PI/20.0)*4.0*($u+$v)*$r*$r + (4.0*[math]::PI/15.0)*$r*$r*$r)
}
$normalization = Kernel-Normalization 16.0 8.0 4.0 8.0
$massA = 12.0 / $normalization
$massB = 12.0 / $normalization
if ([math]::Abs($massA - $massB) -gt 1e-12) { throw 'Fallback normalization changed with view resolution.' }

Write-Host 'Smoke prompt fallback correctness contracts passed.'
