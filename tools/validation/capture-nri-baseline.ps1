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
$compareFrameCount = if ($scenario.PSObject.Properties.Name.Contains("minSelfTestFrames")) { [int]$scenario.minSelfTestFrames } else { 1 }
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

$frames = @($summaries | ForEach-Object { @($_.acceptedSelfTestFrames) | Select-Object -First $compareFrameCount } | Where-Object { $null -ne $_ })
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

$baselinePrefixes = New-Object System.Collections.Generic.List[string]
if ($scenario.PSObject.Properties.Name.Contains("baselinePrefixes")) {
    foreach ($prefix in @($scenario.baselinePrefixes)) {
        if (-not $baselinePrefixes.Contains([string]$prefix)) {
            $baselinePrefixes.Add([string]$prefix)
        }
    }
}
if ($scenario.PSObject.Properties.Name.Contains("prefixAssertions")) {
    foreach ($assertion in @($scenario.prefixAssertions)) {
        if ($assertion.PSObject.Properties.Name.Contains("prefix") -and -not $baselinePrefixes.Contains([string]$assertion.prefix)) {
            $baselinePrefixes.Add([string]$assertion.prefix)
        }
    }
}

$prefixRanges = [ordered]@{}
$prefixBaselineExcludedFields = @(
    "line",
    "frame",
    "stats_frame",
    "total",
    "retire",
    "instance_upload",
    "create",
    "memory",
    "scratch",
    "descriptor",
    "build",
    "barrier"
)
foreach ($prefix in $baselinePrefixes) {
    $records = @()
    foreach ($summary in $summaries) {
        if ($summary.PSObject.Properties.Name.Contains("prefixRecords")) {
            $property = $summary.prefixRecords.PSObject.Properties[$prefix]
            if ($null -ne $property) {
                $records += @($property.Value)
            }
        }
    }

    if ($records.Count -eq 0) {
        continue
    }

    $fields = New-Object System.Collections.Generic.HashSet[string]
    foreach ($record in $records) {
        foreach ($property in $record.PSObject.Properties) {
            if ($prefixBaselineExcludedFields -contains $property.Name) {
                continue
            }

            $parsed = 0.0
            if ([double]::TryParse(
                [string]$property.Value,
                [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsed)) {
                [void]$fields.Add($property.Name)
            }
        }
    }

    $fieldRanges = [ordered]@{}
    foreach ($field in $fields) {
        $values = @()
        foreach ($record in $records) {
            if (-not $record.PSObject.Properties.Name.Contains($field)) {
                continue
            }
            $parsed = 0.0
            if ([double]::TryParse(
                [string]$record.$field,
                [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsed)) {
                $values += $parsed
            }
        }

        if ($values.Count -gt 0) {
            $fieldRanges[$field] = [pscustomobject]@{
                min = ($values | Measure-Object -Minimum).Minimum
                max = ($values | Measure-Object -Maximum).Maximum
            }
        }
    }

    if ($fieldRanges.Count -gt 0) {
        $prefixRanges[$prefix] = [pscustomobject]$fieldRanges
    }
}

$firstFrame = $frames | Select-Object -First 1
$baseline = [pscustomobject]@{
    name = $name
    scenario = $scenario
    runs = $Runs
    compareFrameCount = $compareFrameCount
    capturedUtc = (Get-Date).ToUniversalTime().ToString("o")
    exact = [pscustomobject]@{
        map = if ($firstFrame) { $firstFrame.map } else { $null }
        api = if ($firstFrame) { $firstFrame.api } else { $null }
        route = if ($firstFrame) { $firstFrame.route } else { $null }
        passes = if ($firstFrame) { $firstFrame.passes } else { $null }
    }
    ranges = $ranges
    prefixRanges = $prefixRanges
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$baseline | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "NRI baseline captured: $OutputPath"
