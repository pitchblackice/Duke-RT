param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [string]$OutputPath,

    [int]$Runs = 3,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
$name = if ($scenario.PSObject.Properties.Name.Contains("name")) { [string]$scenario.name } else { "nri-validation" }
if (-not $OutputPath) {
    $baselineDirectory = Join-Path $PSScriptRoot "baselines"
    New-Item -ItemType Directory -Force -Path $baselineDirectory | Out-Null
    $OutputPath = Join-Path $baselineDirectory "$name.json"
}

$summaries = @()
for ($i = 0; $i -lt $Runs; ++$i) {
    $summaryPath = Join-Path (Get-Location) "tools/logs/validation/$name-baseline-$i.summary.json"
    & (Join-Path $PSScriptRoot "run-nri-validation.ps1") -ScenarioPath $ScenarioPath -RazePath $RazePath -GameGrp $GameGrp -File $File -SummaryOutput $summaryPath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    $summaries += Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
}

$frames = @($summaries | ForEach-Object { $_.acceptedSelfTestFrames } | Where-Object { $null -ne $_ })
$numericFields = @("prims", "mats", "scene_instances", "static_instances", "dynamic_instances", "voxel_instances", "emissive_instances", "render_width", "render_height", "output_width", "output_height")
$ranges = [ordered]@{}
foreach ($field in $numericFields) {
    $values = @($frames | Where-Object { $_.PSObject.Properties.Name.Contains($field) } | ForEach-Object { [int64]$_.$field })
    if ($values.Count -gt 0) {
        $ranges[$field] = [pscustomobject]@{
            min = ($values | Measure-Object -Minimum).Minimum
            max = ($values | Measure-Object -Maximum).Maximum
        }
    }
}

$firstFrame = $frames | Select-Object -First 1
$baseline = [pscustomobject]@{
    name = $name
    scenario = $scenario
    runs = $Runs
    capturedUtc = (Get-Date).ToUniversalTime().ToString("o")
    exact = [pscustomobject]@{
        map = if ($firstFrame) { $firstFrame.map } else { $null }
        api = if ($firstFrame) { $firstFrame.api } else { $null }
        route = if ($firstFrame) { $firstFrame.route } else { $null }
        passes = if ($firstFrame) { $firstFrame.passes } else { $null }
    }
    ranges = $ranges
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$baseline | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "NRI baseline captured: $OutputPath"
