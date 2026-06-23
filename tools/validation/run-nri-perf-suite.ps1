param(
    [string[]]$PerfScenario = @(),

    [int]$PerfRuns = 0,

    [switch]$SkipPerf,

    [switch]$SkipSelfTests,

    [switch]$IncludeExplosion,

    [switch]$SkipFailureScan,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",

    [string]$File = "M:\Raze\full-voxel-overlay",

    [int]$SelfTestTimeoutSeconds = 180,

    [int]$ExplosionRuns = 3,

    [int]$ExplosionTraceFrames = 256,

    [int]$ExplosionWaitTics = 180,

    [int]$ExplosionTimeoutSeconds = 180,

    [string]$OutputDirectory,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-SuiteCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & powershell.exe @Arguments | Out-Host
    $exitCode = $LASTEXITCODE
    return [int]$exitCode
}

function Get-ScenarioName {
    param([Parameter(Mandatory = $true)][string]$Path)

    $scenario = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($scenario.PSObject.Properties.Name.Contains("name")) {
        return [string]$scenario.name
    }
    return [System.IO.Path]::GetFileNameWithoutExtension($Path)
}

function Get-DefaultPerfScenarios {
    $baselineDirectory = Join-Path $PSScriptRoot "perf-baselines"
    if (-not (Test-Path -LiteralPath $baselineDirectory)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $baselineDirectory -Filter "*.json" -File |
        Sort-Object Name |
        ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_.Name) })
}

function Resolve-PerfScenarioPath {
    param([Parameter(Mandatory = $true)][string]$NameOrPath)

    if (Test-Path -LiteralPath $NameOrPath) {
        return (Resolve-Path -LiteralPath $NameOrPath).Path
    }

    $candidate = Join-Path $PSScriptRoot "perf-scenarios/$NameOrPath.json"
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    throw "perf scenario not found: $NameOrPath"
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
            text = "failure scan hits suppressed: $suppressed"
        })
    }

    return $hits.ToArray()
}

if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/perf-suite/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $OutputDirectory "summary.json"
}

$perfResults = @()
if (-not $SkipPerf) {
    $perfScenarioNames = if ($PerfScenario.Count -gt 0) { @($PerfScenario) } else { Get-DefaultPerfScenarios }
    foreach ($scenarioNameOrPath in $perfScenarioNames) {
        $scenarioPath = Resolve-PerfScenarioPath -NameOrPath $scenarioNameOrPath
        $scenarioName = Get-ScenarioName -Path $scenarioPath
        $baselinePath = Join-Path $PSScriptRoot "perf-baselines/$scenarioName.json"
        $scenarioOutput = Join-Path $OutputDirectory "perf/$scenarioName"
        $scenarioSummary = Join-Path $scenarioOutput "summary.json"

        $runArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "run-nri-perf.ps1"),
            "-ScenarioPath", $scenarioPath,
            "-RazePath", $RazePath,
            "-OutputDirectory", $scenarioOutput,
            "-SummaryOutput", $scenarioSummary
        )
        if ($PerfRuns -gt 0) {
            $runArgs += @("-Runs", [string]$PerfRuns)
        }

        Write-Host "NRI perf suite scenario: $scenarioName"
        $runExit = Invoke-SuiteCommand -Arguments $runArgs
        $compareExit = $null
        if ($runExit -eq 0 -and (Test-Path -LiteralPath $baselinePath)) {
            $compareArgs = @(
                "-NoProfile",
                "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $PSScriptRoot "compare-nri-perf-baseline.ps1"),
                "-SummaryPath", $scenarioSummary,
                "-BaselinePath", $baselinePath
            )
            $compareExit = Invoke-SuiteCommand -Arguments $compareArgs
        }
        elseif (-not (Test-Path -LiteralPath $baselinePath)) {
            $compareExit = 1
        }

        $loopTrace = $null
        $errors = @()
        if (Test-Path -LiteralPath $scenarioSummary) {
            $scenarioRunSummary = Get-Content -LiteralPath $scenarioSummary -Raw | ConvertFrom-Json
            $loopTrace = $scenarioRunSummary.loopTrace
            $errors = @($scenarioRunSummary.errors)
        }

        $perfResults += [pscustomobject]@{
            name = $scenarioName
            scenarioPath = $scenarioPath
            baselinePath = $baselinePath
            outputDirectory = $scenarioOutput
            summaryPath = $scenarioSummary
            runExitCode = $runExit
            compareExitCode = $compareExit
            ok = ($runExit -eq 0 -and $compareExit -eq 0)
            loopTrace = $loopTrace
            errors = $errors
        }
    }
}

$selfTestResults = @()
if (-not $SkipSelfTests) {
    $selfTestScenarioPaths = @(
        "tools/validation/scenarios/stable-opening-d3d12-beauty-e3l6.json",
        "tools/validation/scenarios/stable-opening-d3d12-beauty-e1l5.json",
        "tools/validation/scenarios/stable-opening-d3d12-beauty-e2l4.json"
    )

    foreach ($scenarioPath in $selfTestScenarioPaths) {
        $resolvedScenarioPath = (Resolve-Path -LiteralPath $scenarioPath).Path
        $scenarioName = Get-ScenarioName -Path $resolvedScenarioPath
        $baselinePath = Join-Path $PSScriptRoot "baselines/$scenarioName.json"
        $logPath = Join-Path $OutputDirectory "selftest/$scenarioName.log"
        $summaryPath = Join-Path $OutputDirectory "selftest/$scenarioName.summary.json"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $logPath) | Out-Null

        Write-Host "NRI perf suite self-test: $scenarioName"
        $runArgs = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "run-nri-validation.ps1"),
            "-ScenarioPath", $resolvedScenarioPath,
            "-RazePath", $RazePath,
            "-GameGrp", $GameGrp,
            "-File", $File,
            "-TimeoutSeconds", [string]$SelfTestTimeoutSeconds,
            "-CaptureWhenPassed",
            "-LogPath", $logPath,
            "-SummaryOutput", $summaryPath
        )
        $runExit = Invoke-SuiteCommand -Arguments $runArgs
        $compareExit = $null
        if ($runExit -eq 0) {
            $compareArgs = @(
                "-NoProfile",
                "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $PSScriptRoot "compare-nri-baseline.ps1"),
                "-SummaryPath", $summaryPath,
                "-BaselinePath", $baselinePath
            )
            $compareExit = Invoke-SuiteCommand -Arguments $compareArgs
        }

        $selfTestResults += [pscustomobject]@{
            name = $scenarioName
            scenarioPath = $resolvedScenarioPath
            baselinePath = $baselinePath
            logPath = $logPath
            summaryPath = $summaryPath
            runExitCode = $runExit
            compareExitCode = $compareExit
            ok = ($runExit -eq 0 -and $compareExit -eq 0)
        }
    }
}

$explosionResult = $null
if ($IncludeExplosion) {
    $explosionOutput = Join-Path $OutputDirectory "explosion-repro"
    $explosionSummary = Join-Path $explosionOutput "summary.json"
    Write-Host "NRI perf suite explosion repro"
    $explosionArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run-explosion-repro.ps1"),
        "-RazePath", $RazePath,
        "-GameGrp", $GameGrp,
        "-File", $File,
        "-Runs", [string]$ExplosionRuns,
        "-TraceFrames", [string]$ExplosionTraceFrames,
        "-WaitTics", [string]$ExplosionWaitTics,
        "-TimeoutSeconds", [string]$ExplosionTimeoutSeconds,
        "-OutputDirectory", $explosionOutput,
        "-SummaryOutput", $explosionSummary
    )
    $explosionExit = Invoke-SuiteCommand -Arguments $explosionArgs
    $explosionResult = [pscustomobject]@{
        outputDirectory = $explosionOutput
        summaryPath = $explosionSummary
        exitCode = $explosionExit
        ok = $explosionExit -eq 0
    }
}

$failureHits = @()
if (-not $SkipFailureScan) {
    $failureHits = @(Get-FailureScanHits -Directory $OutputDirectory)
}

$errors = New-Object System.Collections.Generic.List[string]
foreach ($result in $perfResults) {
    if (-not $result.ok) {
        $errors.Add("perf scenario failed: $($result.name)")
    }
}
foreach ($result in $selfTestResults) {
    if (-not $result.ok) {
        $errors.Add("self-test failed: $($result.name)")
    }
}
if ($null -ne $explosionResult -and -not $explosionResult.ok) {
    $errors.Add("explosion repro failed")
}
if ($failureHits.Count -gt 0) {
    $errors.Add("failure scan found $($failureHits.Count) generated-log hits")
}

$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    outputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
    generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
    perf = $perfResults
    selfTests = $selfTestResults
    explosion = $explosionResult
    failureScan = [pscustomobject]@{
        ok = $failureHits.Count -eq 0
        hitCount = $failureHits.Count
        hits = $failureHits
    }
    errors = $errors.ToArray()
}

$summaryDirectory = Split-Path -Parent $SummaryOutput
if ($summaryDirectory) {
    New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null
}
$summary | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

Write-Host "NRI perf suite complete: ok=$($summary.ok) perf=$($perfResults.Count) selftests=$($selfTestResults.Count) explosion=$($null -ne $explosionResult) failure_hits=$($failureHits.Count) summary=$SummaryOutput"
if (-not $summary.ok) {
    foreach ($errorText in $summary.errors) {
        Write-Host "  $errorText"
    }
    exit 1
}
