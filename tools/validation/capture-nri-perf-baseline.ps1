param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [string]$OutputPath,

    [int]$Runs = 3,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File,

    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-ObjectProperty {
    param(
        [object]$Object,
        [string]$Name,
        [object]$Default = $null
    )

    if ($null -ne $Object -and $Object.PSObject.Properties.Name.Contains($Name)) {
        return $Object.$Name
    }
    return $Default
}

$scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
$name = [string](Get-ObjectProperty -Object $scenario -Name "name" -Default "nri-perf")
if (-not $OutputPath) {
    $baselineDirectory = Join-Path $PSScriptRoot "perf-baselines"
    New-Item -ItemType Directory -Force -Path $baselineDirectory | Out-Null
    $OutputPath = Join-Path $baselineDirectory "$name.json"
}
if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/perf-baseline/$name/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$summaryPath = Join-Path $OutputDirectory "summary.json"
$args = @(
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $PSScriptRoot "run-nri-perf.ps1"),
    "-ScenarioPath", $ScenarioPath,
    "-RazePath", $RazePath,
    "-Runs", [string]$Runs,
    "-OutputDirectory", $OutputDirectory,
    "-SummaryOutput", $summaryPath
)
if ($GameGrp) {
    $args += @("-GameGrp", $GameGrp)
}
if ($File) {
    $args += @("-File", $File)
}

$process = Start-Process -FilePath "powershell" -ArgumentList $args -PassThru -Wait -NoNewWindow
if ($process.ExitCode -ne 0) {
    exit $process.ExitCode
}

$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$baseline = [pscustomobject]@{
    name = $name
    scenarioPath = (Resolve-Path -LiteralPath $ScenarioPath).Path
    scenario = $scenario
    runs = $Runs
    capturedUtc = (Get-Date).ToUniversalTime().ToString("o")
    sourceSummaryPath = (Resolve-Path -LiteralPath $summaryPath).Path
    baselineCompare = Get-ObjectProperty -Object $scenario -Name "baselineCompare"
    thresholds = Get-ObjectProperty -Object $scenario -Name "thresholds"
    loopTrace = $summary.loopTrace
    fields = $summary.fields
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$baseline | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "NRI perf baseline captured: $OutputPath"
