Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$mainloop = Get-Content -LiteralPath (Join-Path $repo 'source\core\mainloop.cpp') -Raw
$timeHeader = Get-Content -LiteralPath (Join-Path $repo 'source\common\utility\i_time.h') -Raw
$captureHeader = Get-Content -LiteralPath (Join-Path $repo 'source\common\engine\perf_capture.h') -Raw
$captureOwner = Get-Content -LiteralPath (Join-Path $repo 'source\common\engine\perf_capture.cpp') -Raw
$cvars = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\renderer\nri_cvars.cpp') -Raw
$preload = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\renderer\nri_preload_coordinator.cpp') -Raw
$renderer = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\renderer\nri_renderer.cpp') -Raw

Assert-True ($mainloop -match 'CUSTOM_CVAR\(Int, perf_fixedsimulationframes, 0, 0\)[\s\S]*?self > 4096') 'fixed control must be session-only and bounded'
$tryRun = $mainloop.IndexOf('void TryRunTics')
$fixedReturn = $mainloop.IndexOf('if ((int)perf_fixedsimulationframes > 0 &&', $tryRun)
$ticWait = $mainloop.IndexOf('I_WaitForTic (oldentertics)', $tryRun)
$netUpdate = $mainloop.IndexOf('NetUpdate ();', $tryRun)
Assert-True ($fixedReturn -ge 0 -and $fixedReturn -lt $ticWait -and $fixedReturn -lt $netUpdate) 'fixed presentations must bypass tic waits and network update'
Assert-True ($mainloop -match '!perfFixedSimulationOwnsTimeFreeze && !I_IsTimeFrozen\(\)[\s\S]*?I_FreezeTime\(true\)[\s\S]*?perfFixedSimulationOwnsTimeFreeze = true') 'fixed control must only claim an unfrozen clock'
Assert-True ($mainloop -match 'if \(perfFixedSimulationOwnsTimeFreeze\)[\s\S]*?I_FreezeTime\(false\)[\s\S]*?perfFixedSimulationOwnsTimeFreeze = false') 'fixed control must release only its own clock freeze'
Assert-True ($timeHeader -match 'bool I_IsTimeFrozen\(\);') 'clock ownership query is missing'

$ticker = $mainloop.IndexOf('GameTicker();')
$armBoundary = $mainloop.IndexOf('fixedSimulationSuppressedTailTicks = (uint32_t)counts;')
$loadBoundary = $mainloop.IndexOf('if (UsePathTracingLevelLoadClockPolicy() && counts > 0)', $ticker)
Assert-True ($ticker -ge 0 -and $armBoundary -gt $ticker -and $loadBoundary -gt $armBoundary) 'delayed arm must suppress a stale catch-up tail before the load-clock boundary'
Assert-True ($captureHeader -match 'fixedSimulationReturn[\s\S]*?fixedSimulationSuppressedTailTicks') 'compact contract lacks fixed-capture identity'
Assert-True ($captureOwner -match 'fixed_return=%d fixed_tail_suppressed=%u') 'compact rows lack fixed-capture identity'
Assert-True ($mainloop -match 'fixed_return=%d fixed_tail_suppressed=%u') 'ordinary rows lack fixed-capture identity'

Assert-True ($cvars -match 'CVAR\(String, nri_ptvoxelcomputepreloadreleasecommand, "", 0\)') 'strict release command must be session-only'
Assert-True ($preload -match 'ArmStrictPreloadFirstFrameReleaseCommand[\s\S]*?reason=strict-complete') 'strict closure must arm the first-frame command'
Assert-True ($preload -match 'QueueStrictPreloadFirstFrameReleaseCommand[\s\S]*?strictTerminalLogged[\s\S]*?active-work-pending[\s\S]*?AddCommandString') 'release must revalidate closure before queueing'
Assert-True ($renderer -match 'OnLevelFirstFrameRelease\(\)[\s\S]*?QueueStrictPreloadFirstFrameReleaseCommand') 'release command must queue only at the first-main-view boundary'
Assert-True ($preload -notmatch 'Spatial') 'current-branch capture support must not import spatial-rebuild policy'

Write-Host 'current-branch fixed-simulation tests passed'
