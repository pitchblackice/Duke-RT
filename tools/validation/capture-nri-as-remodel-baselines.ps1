param(
    [string[]]$Scenario = @(),

    [int]$Runs = 3,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",

    [string]$File = "M:\Raze\full-voxel-overlay",

    [string]$BaselineDirectory,

    [string]$LogDirectory,

    [switch]$Force
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

if (-not $BaselineDirectory) {
    $BaselineDirectory = Join-Path $PSScriptRoot "baselines/as-remodel"
}
if (-not $LogDirectory) {
    $LogDirectory = Join-Path (Get-Location) "tools/logs/as-remodel-baselines/$(Get-Date -Format 'yyyyMMdd-HHmmss')"
}
New-Item -ItemType Directory -Force -Path $BaselineDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

$results = New-Object System.Collections.Generic.List[object]
$commit = (& git rev-parse --short=10 HEAD 2>$null)
foreach ($scenarioInput in $Scenario) {
    $scenarioPath = Get-AsScenarioPath -NameOrPath $scenarioInput
    $name = Get-ScenarioName -Path $scenarioPath
    $outputPath = Join-Path $BaselineDirectory "$name.json"
    if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
        throw "baseline exists: $outputPath (pass -Force to overwrite)"
    }

    Write-Host "Capturing AS remodel baseline: $name"
    & (Join-Path $PSScriptRoot "capture-nri-baseline.ps1") `
        -ScenarioPath $scenarioPath `
        -OutputPath $outputPath `
        -Runs $Runs `
        -RazePath $RazePath `
        -GameGrp $GameGrp `
        -File $File
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $compareCommand = "powershell -NoProfile -ExecutionPolicy Bypass -File tools\validation\compare-nri-baseline.ps1 -SummaryPath <summary.json> -BaselinePath $outputPath"
    Write-Host "Compare with: $compareCommand"
    $results.Add([pscustomobject]@{
        name = $name
        scenarioPath = $scenarioPath
        baselinePath = $outputPath
        compareCommand = $compareCommand
    })
}

$manifest = [pscustomobject]@{
    capturedUtc = (Get-Date).ToUniversalTime().ToString("o")
    sourceCommit = $commit
    runs = $Runs
    razePath = $RazePath
    gameGrp = $GameGrp
    file = $File
    baselineDirectory = (Resolve-Path -LiteralPath $BaselineDirectory).Path
    logDirectory = (Resolve-Path -LiteralPath $LogDirectory).Path
    baselines = $results.ToArray()
}
$manifestPath = Join-Path $BaselineDirectory "manifest.json"
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "NRI AS remodel baselines captured: $($results.Count) manifest=$manifestPath"
