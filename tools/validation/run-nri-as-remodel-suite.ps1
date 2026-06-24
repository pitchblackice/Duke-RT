param(
    [string[]]$Scenario = @(),

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",

    [string]$File = "M:\Raze\full-voxel-overlay",

    [int]$TimeoutSeconds = 180,

    [switch]$IncludeExplosion,

    [int]$ExplosionRuns = 3,

    [int]$ExplosionTraceFrames = 256,

    [int]$ExplosionWaitTics = 180,

    [switch]$SkipBaselineCompare,

    [switch]$SkipFailureScan,

    [string]$OutputDirectory,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-AsScenarioPath {
    param([Parameter(Mandatory = $true)][string]$NameOrPath)

    if (Test-Path -LiteralPath $NameOrPath) {
        return (Resolve-Path -LiteralPath $NameOrPath).Path
    }

    $candidate = Join-Path $PSScriptRoot "scenarios/$NameOrPath.json"
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    throw "AS remodel scenario not found: $NameOrPath"
}

function Get-ScenarioName {
    param([Parameter(Mandatory = $true)][string]$Path)

    $scenarioJson = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($scenarioJson.PSObject.Properties.Name.Contains("name")) {
        return [string]$scenarioJson.name
    }
    return [System.IO.Path]::GetFileNameWithoutExtension($Path)
}

function Invoke-SuiteCommand {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    & powershell.exe @Arguments | Out-Host
    return [int]$LASTEXITCODE
}

function Get-FailureScanHits {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,

        [int]$MaxHits = 200
    )

    if (-not (Test-Path -LiteralPath $Directory)) {
        return @()
    }

    $patterns = @(
        "Device removed",
        "device lost",
        "DXGI_ERROR_DEVICE",
        "QueueSubmit failed",
        "QueuePresent failed",
        "AcquireNextTexture(): failed",
        "NRI render failed",
        "validation error",
        "failed to create",
        "assertion failed",
        "fatal error",
        "DRED after",
        "DRED breadcrumb",
        "DRED page fault",
        "DRED page-fault",
        "DeviceRemovedExtendedData"
    )

    $hits = New-Object System.Collections.Generic.List[object]
    $suppressed = 0
    $logFiles = @(Get-ChildItem -LiteralPath $Directory -Filter "*.log" -File -Recurse | Sort-Object FullName)
    foreach ($logFile in $logFiles) {
        $lineNumber = 0
        foreach ($line in [System.IO.File]::ReadLines($logFile.FullName)) {
            $lineNumber++
            foreach ($pattern in $patterns) {
                if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    if ($hits.Count -lt $MaxHits) {
                        $hits.Add([pscustomobject]@{
                            path = $logFile.FullName
                            line = $lineNumber
                            pattern = $pattern
                            text = $line
                        })
                    }
                    else {
                        $suppressed++
                    }
                    break
                }
            }
        }
    }

    if ($suppressed -gt 0) {
        $hits.Add([pscustomobject]@{
            path = $Directory
            line = 0
            pattern = "suppressed"
            text = "$suppressed additional failure-scan hits suppressed"
        })
    }

    return $hits.ToArray()
}

if ($Scenario.Count -eq 0) {
    $Scenario = @(
        "as-opening-d3d12-beauty-e3l6",
        "as-opening-d3d12-beauty-e1l5",
        "as-opening-d3d12-beauty-e2l4",
        "as-opening-d3d12-e1l1-voxels",
        "as-loading-d3d12-e1l1-voxels",
        "as-loading-d3d12-e1l1-shared-blas-route"
    )
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/as-remodel-suite/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$scenarioResults = New-Object System.Collections.Generic.List[object]
$errors = New-Object System.Collections.Generic.List[string]
$baselineDirectory = Join-Path $PSScriptRoot "baselines/as-remodel"

foreach ($scenarioInput in $Scenario) {
    $scenarioPath = Get-AsScenarioPath -NameOrPath $scenarioInput
    $name = Get-ScenarioName -Path $scenarioPath
    $logPath = Join-Path $OutputDirectory "$name.log"
    $summaryPath = Join-Path $OutputDirectory "$name.summary.json"
    Write-Host "NRI AS remodel scenario: $name"

    $runArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run-nri-validation.ps1"),
        "-ScenarioPath", $scenarioPath,
        "-RazePath", $RazePath,
        "-GameGrp", $GameGrp,
        "-File", $File,
        "-TimeoutSeconds", ([string]$TimeoutSeconds),
        "-LogPath", $logPath,
        "-SummaryOutput", $summaryPath
    )
    $runExit = Invoke-SuiteCommand -Arguments $runArgs
    $compareExit = 0
    $baselinePath = Join-Path $baselineDirectory "$name.json"
    if ($runExit -eq 0 -and -not $SkipBaselineCompare -and (Test-Path -LiteralPath $baselinePath)) {
        $compareArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "compare-nri-baseline.ps1"),
            "-SummaryPath", $summaryPath,
            "-BaselinePath", $baselinePath
        )
        $compareExit = Invoke-SuiteCommand -Arguments $compareArgs
    }
    elseif ($runExit -eq 0 -and -not $SkipBaselineCompare) {
        Write-Host "NRI AS remodel baseline missing; skipped compare: $baselinePath"
    }

    $ok = $runExit -eq 0 -and $compareExit -eq 0
    if (-not $ok) {
        $errors.Add("scenario failed: $name")
    }
    $scenarioResults.Add([pscustomobject]@{
        name = $name
        scenarioPath = $scenarioPath
        logPath = $logPath
        summaryPath = $summaryPath
        baselinePath = $baselinePath
        runExitCode = $runExit
        compareExitCode = $compareExit
        ok = $ok
    })
}

$explosionResult = $null
if ($IncludeExplosion) {
    $explosionOutput = Join-Path $OutputDirectory "explosion-repro"
    $explosionArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run-explosion-repro.ps1"),
        "-RazePath", $RazePath,
        "-GameGrp", $GameGrp,
        "-File", $File,
        "-Runs", ([string]$ExplosionRuns),
        "-TraceFrames", ([string]$ExplosionTraceFrames),
        "-WaitTics", ([string]$ExplosionWaitTics),
        "-TimeoutSeconds", ([string]$TimeoutSeconds),
        "-OutputDirectory", $explosionOutput
    )
    $explosionExit = Invoke-SuiteCommand -Arguments $explosionArgs
    $explosionResult = [pscustomobject]@{
        outputDirectory = $explosionOutput
        exitCode = $explosionExit
        ok = $explosionExit -eq 0
    }
    if ($explosionExit -ne 0) {
        $errors.Add("explosion repro failed")
    }
}

$failureHits = @()
if (-not $SkipFailureScan) {
    $failureHits = @(Get-FailureScanHits -Directory $OutputDirectory)
    foreach ($hit in $failureHits) {
        $errors.Add("failure-scan hit $($hit.pattern): $($hit.path):$($hit.line)")
    }
}

$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    outputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
    scenarios = $scenarioResults.ToArray()
    explosion = $explosionResult
    failureHits = $failureHits
    errors = $errors.ToArray()
}

if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $OutputDirectory "summary.json"
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

Write-Host "NRI AS remodel suite complete: ok=$($summary.ok) scenarios=$($scenarioResults.Count) failure_hits=$($failureHits.Count) summary=$SummaryOutput"
if (-not $summary.ok) {
    foreach ($errorText in $errors) {
        Write-Host "  $errorText"
    }
    exit 1
}
