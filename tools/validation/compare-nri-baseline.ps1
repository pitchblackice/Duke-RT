param(
    [Parameter(Mandatory = $true)]
    [string]$SummaryPath,

    [Parameter(Mandatory = $true)]
    [string]$BaselinePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$summary = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json
$baseline = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
$errors = New-Object System.Collections.Generic.List[string]
$frames = @($summary.acceptedSelfTestFrames)
if ($frames.Count -eq 0) {
    $errors.Add("summary has no accepted selftest frames")
}

$compareFrameCount = 0
if ($baseline.PSObject.Properties.Name.Contains("compareFrameCount")) {
    $compareFrameCount = [int]$baseline.compareFrameCount
}
elseif ($baseline.PSObject.Properties.Name.Contains("scenario") -and
    $baseline.scenario.PSObject.Properties.Name.Contains("minSelfTestFrames")) {
    $compareFrameCount = [int]$baseline.scenario.minSelfTestFrames
}

if ($compareFrameCount -gt 0) {
    $frames = @($frames | Select-Object -First $compareFrameCount)
}

$firstFrame = $frames | Select-Object -First 1
if ($firstFrame -ne $null -and $baseline.PSObject.Properties.Name.Contains("exact")) {
    foreach ($field in @("map", "api", "route", "passes")) {
        if ($baseline.exact.PSObject.Properties.Name.Contains($field) -and $null -ne $baseline.exact.$field) {
            if ([string]$firstFrame.$field -ne [string]$baseline.exact.$field) {
                $errors.Add("exact field '$field' expected '$($baseline.exact.$field)' actual '$($firstFrame.$field)'")
            }
        }
    }
}

if ($baseline.PSObject.Properties.Name.Contains("ranges")) {
    foreach ($rangeProperty in $baseline.ranges.PSObject.Properties) {
        $field = $rangeProperty.Name
        $range = $rangeProperty.Value
        foreach ($frame in $frames) {
            if (-not $frame.PSObject.Properties.Name.Contains($field)) {
                $errors.Add("frame missing ranged field '$field'")
                continue
            }
            $value = [int64]$frame.$field
            if ($value -lt [int64]$range.min -or $value -gt [int64]$range.max) {
                $errors.Add("field '$field' value $value outside baseline range $($range.min)..$($range.max)")
            }
        }
    }
}

if ($errors.Count -gt 0) {
    foreach ($errorText in $errors) {
        Write-Error $errorText -ErrorAction Continue
    }
    exit 1
}

Write-Host "NRI baseline compare passed: summary=$SummaryPath baseline=$BaselinePath"
