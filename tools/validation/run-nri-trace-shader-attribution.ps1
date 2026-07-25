param(
    [string]$ContractPath = "tools/validation/perf-scenarios/current-direct-trace-shader-attribution-d3d12.json",
    [string]$RazePath = "build/terminal-ninja/raze.exe",
    [string]$GameGrp,
    [string]$File,
    [int]$Runs = 1,
    [int]$TimeoutSeconds = 0,
    [switch]$Build,
    [switch]$PrepareOnly,
    [string]$OutputDirectory,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Set-ScenarioCvar {
    param([object]$Scenario, [string]$Name, [string]$Value)

    $args = @($Scenario.launch.extraArgs)
    for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
        if ([string]$args[$index] -eq "+set" -and [string]$args[$index + 1] -eq $Name) {
            $args[$index + 2] = $Value
            $Scenario.launch.extraArgs = $args
            return
        }
    }
    $Scenario.launch.extraArgs = @($args) + @("+set", $Name, $Value)
}

function New-AttributionScenario {
    param([object]$Contract, [object]$Base, [object]$Leg, [string]$Directory)

    $scenario = $Base | ConvertTo-Json -Depth 24 | ConvertFrom-Json
    foreach ($setting in $Leg.cvars.PSObject.Properties) {
        Set-ScenarioCvar -Scenario $scenario -Name $setting.Name -Value ([string]$setting.Value)
    }
    Set-ScenarioCvar -Scenario $scenario -Name "nri_ptshaderstats" -Value "true"
    Set-ScenarioCvar -Scenario $scenario -Name "nri_ptgputiming" -Value "false"

    $collectionFrames = [int]$Contract.attributionFrames + [int]$Contract.drainFrames
    $compactFrames = [int]$Contract.compactIdentityFrames
    $terminalCommand = "set nri_ptloadingtrace 0; wait 1; closemenu; wait 512; perf_looptraceframes $collectionFrames; perf_compactframes $compactFrames"
    Set-ScenarioCvar -Scenario $scenario -Name "nri_ptvoxelcomputepreloadterminalcommand" -Value $terminalCommand

    $scenario.name = "current-direct-trace-large-voxel-shader-$($Leg.name)"
    $scenario.description = "Asynchronous TraceOpaque shader attribution derived from '$($Base.name)'; leg=$($Leg.name)."
    $scenario.capture.loopTraceFrames = $collectionFrames + $compactFrames
    $scenario.capture.runs = 1
    $scenario.capture.stopWhenLoopTraceFramesCaptured = $true
    $scenario.requiredPrefixes = @($Contract.requiredPrefixes)
    $scenario | Add-Member -NotePropertyName traceShaderAttribution -NotePropertyValue ([pscustomobject]@{
        schema = 1
        leg = [string]$Leg.name
        attributionFrames = [int]$Contract.attributionFrames
        drainFrames = [int]$Contract.drainFrames
        compactIdentityFrames = $compactFrames
        minimumPublishedSnapshots = [int]$Contract.minimumPublishedSnapshots
        terminalCommand = $terminalCommand
        expectedTrace = $Leg.expectedTrace
    })

    $path = Join-Path $Directory ("large-voxel-{0}.json" -f $Leg.name)
    $scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

if ($Runs -lt 1) { throw "Runs must be at least 1." }
$resolvedContract = (Resolve-Path -LiteralPath $ContractPath -ErrorAction Stop).Path
$contract = Get-Content -LiteralPath $resolvedContract -Raw | ConvertFrom-Json
if ([int]$contract.attributionFrames -lt 1 -or [int]$contract.drainFrames -lt 1 -or
    [int]$contract.compactIdentityFrames -lt 1 -or [int]$contract.minimumPublishedSnapshots -lt 1) {
    throw "Attribution, drain, compact identity, and minimum snapshot counts must be positive."
}
if (@($contract.legs).Count -lt 2) { throw "The attribution contract must contain at least two legs." }

$resolvedBase = (Resolve-Path -LiteralPath ([string]$contract.baseScenario) -ErrorAction Stop).Path
$baseScenario = Get-Content -LiteralPath $resolvedBase -Raw | ConvertFrom-Json
if ([string]$baseScenario.directTraceProfile -ne "large-voxel") {
    throw "Attribution must derive from the current direct large-voxel profile."
}

if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/perf/current-direct-trace-shader-attribution/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not $SummaryOutput) { $SummaryOutput = Join-Path $resolvedOutput "summary.json" }
$generatedDirectory = Join-Path $resolvedOutput "generated-scenarios"
New-Item -ItemType Directory -Force -Path $generatedDirectory | Out-Null

$generated = @{}
foreach ($leg in @($contract.legs)) {
    if ($generated.ContainsKey([string]$leg.name)) { throw "Attribution leg names must be unique." }
    $generated[[string]$leg.name] = New-AttributionScenario -Contract $contract -Base $baseScenario -Leg $leg -Directory $generatedDirectory
}

$runCommand = "powershell -NoProfile -ExecutionPolicy Bypass -File tools/validation/run-nri-trace-shader-attribution.ps1 -Runs $Runs"
if ($PrepareOnly) {
    [pscustomobject]@{
        schema = 1
        preparedOnly = $true
        contractPath = $resolvedContract
        baseScenarioPath = $resolvedBase
        runCommand = $runCommand
        generatedScenarios = @($generated.GetEnumerator() | Sort-Object Name | ForEach-Object {
            [pscustomobject]@{ leg = $_.Name; path = $_.Value }
        })
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
    Write-Host "Trace shader attribution prepared without launch: summary=$SummaryOutput"
    exit 0
}

$runnerPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "run-nri-perf.ps1")).Path
$analyzerPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "analyze-nri-trace-shader-attribution.ps1")).Path
$powershellPath = (Get-Process -Id $PID).Path
$entries = New-Object System.Collections.Generic.List[object]
$sequence = 0
$buildPending = [bool]$Build
for ($cycle = 1; $cycle -le $Runs; ++$cycle) {
    $legs = if (($cycle % 2) -eq 1) { @($contract.legs) } else { @($contract.legs)[(@($contract.legs).Count - 1)..0] }
    foreach ($leg in $legs) {
        $sequence++
        $legName = [string]$leg.name
        $legOutput = Join-Path $resolvedOutput ("{0:D2}-{1}-run-{2:D2}" -f $sequence, $legName, $cycle)
        $baseSummary = Join-Path $legOutput "summary.json"
        $runnerArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runnerPath,
            "-ScenarioPath", $generated[$legName], "-RazePath", $RazePath, "-Runs", "1",
            "-OutputDirectory", $legOutput, "-SummaryOutput", $baseSummary)
        if ($TimeoutSeconds -gt 0) { $runnerArgs += @("-TimeoutSeconds", [string]$TimeoutSeconds) }
        if ($GameGrp) { $runnerArgs += @("-GameGrp", $GameGrp) }
        if ($File) { $runnerArgs += @("-File", $File) }
        if ($buildPending) { $runnerArgs += "-Build"; $buildPending = $false }

        Write-Host "Trace shader attribution: sequence=$sequence cycle=$cycle leg=$legName"
        & $powershellPath @runnerArgs
        $exitCode = $LASTEXITCODE
        $entries.Add([pscustomobject]@{
            sequence = $sequence
            cycle = $cycle
            leg = $legName
            scenarioPath = $generated[$legName]
            outputDirectory = $legOutput
            logPath = Join-Path $legOutput "run-1.log"
            baseSummaryPath = $baseSummary
            exitCode = $exitCode
        })
        if ($exitCode -ne 0) { throw "Perf runner failed for sequence $sequence with exit code $exitCode." }
    }
}

$manifestPath = Join-Path $resolvedOutput "manifest.json"
[pscustomobject]@{
    schema = 1
    generatedUtc = (Get-Date).ToUniversalTime().ToString("o")
    contractPath = $resolvedContract
    baseScenarioPath = $resolvedBase
    runCommand = $runCommand
    runsPerLeg = $Runs
    entries = $entries.ToArray()
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

& $powershellPath -NoProfile -ExecutionPolicy Bypass -File $analyzerPath -ManifestPath $manifestPath -SummaryOutput $SummaryOutput
exit $LASTEXITCODE
