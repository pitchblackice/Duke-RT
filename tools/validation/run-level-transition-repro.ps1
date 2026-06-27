param(
    [string]$ScenarioPath = "tools/validation/perf-scenarios/level-transition-button-d3d12.json",

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",

    [string]$File = "M:\Raze\full-voxel-overlay",

    [int]$TimeoutSeconds = 0,

    [string]$BaselinePath = "tools/validation/transition-baselines/level-transition-button-d3d12.json",

    [switch]$SkipCompare,

    [string]$OutputDirectory,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/level-transition/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $OutputDirectory "summary.json"
}

$perfSummaryPath = Join-Path $OutputDirectory "perf-summary.json"
$runArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", "tools/validation/run-nri-perf.ps1",
    "-ScenarioPath", $ScenarioPath,
    "-RazePath", $RazePath,
    "-GameGrp", $GameGrp,
    "-File", $File,
    "-Runs", "1",
    "-OutputDirectory", $OutputDirectory,
    "-SummaryOutput", $perfSummaryPath
)
if ($TimeoutSeconds -gt 0) {
    $runArgs += @("-TimeoutSeconds", [string]$TimeoutSeconds)
}

& powershell.exe @runArgs
$perfExitCode = $LASTEXITCODE

$transitionSummaries = @()
$runLogs = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter "run-*.log" -File | Sort-Object Name)
foreach ($runLog in $runLogs) {
    $transitionSummaryPath = Join-Path $OutputDirectory ("{0}.transition-summary.json" -f [System.IO.Path]::GetFileNameWithoutExtension($runLog.Name))
    & powershell.exe -ExecutionPolicy Bypass -File "tools/validation/analyze-level-transition-log.ps1" -LogPath $runLog.FullName -SummaryOutput $transitionSummaryPath
    $transitionSummaries += (Get-Content -LiteralPath $transitionSummaryPath -Raw | ConvertFrom-Json)
}

$perfSummary = $null
if (Test-Path -LiteralPath $perfSummaryPath) {
    $perfSummary = Get-Content -LiteralPath $perfSummaryPath -Raw | ConvertFrom-Json
}

$compareResult = [pscustomobject]@{
    ran = $false
    ok = $null
    baselinePath = $BaselinePath
}
$ok = ($perfExitCode -eq 0) -and (@($transitionSummaries | Where-Object { [int]$_.windowCount -lt 2 }).Count -eq 0)
$summary = [pscustomobject]@{
    ok = $ok
    scenarioPath = (Resolve-Path -LiteralPath $ScenarioPath).Path
    outputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
    perfSummaryPath = if (Test-Path -LiteralPath $perfSummaryPath) { (Resolve-Path -LiteralPath $perfSummaryPath).Path } else { $perfSummaryPath }
    perf = $perfSummary
    compare = $compareResult
    transitionRuns = $transitionSummaries
}
$summary | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

Write-Host "Level transition repro complete: ok=$ok summary=$SummaryOutput"
foreach ($transition in $transitionSummaries) {
    foreach ($window in @($transition.windows)) {
        Write-Host ("  window {0}: frames {1}-{2}, p50={3} p95={4} p99={5} max={6}, >50={7}, >100={8}" -f `
            $window.index, `
            $window.firstFrame, `
            $window.lastFrame, `
            $window.frame.p50, `
            $window.frame.p95, `
            $window.frame.p99, `
            $window.frame.max, `
            $window.frame.over50ms, `
            $window.frame.over100ms)
    }
}

if (-not $SkipCompare -and $BaselinePath -and (Test-Path -LiteralPath $BaselinePath)) {
    & powershell.exe -ExecutionPolicy Bypass -File "tools/validation/compare-level-transition-baseline.ps1" -SummaryPath $SummaryOutput -BaselinePath $BaselinePath
    $compareResult.ran = $true
    $compareResult.ok = ($LASTEXITCODE -eq 0)
    if (-not $compareResult.ok) {
        $ok = $false
    }
}

$summary.ok = $ok
$summary | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

if (-not $ok) {
    exit 1
}
