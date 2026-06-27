param(
    [Parameter(Mandatory = $true)]
    [string]$SummaryPath,

    [Parameter(Mandatory = $true)]
    [string]$BaselinePath
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

function Get-TransitionSummary {
    param([object]$Summary)

    $transitionRuns = @(Get-ObjectProperty -Object $Summary -Name "transitionRuns" -Default @())
    if ($transitionRuns.Count -gt 0) {
        return $transitionRuns[0]
    }
    return $Summary
}

function Test-ThresholdObject {
    param(
        [string]$Path,
        [double]$Actual,
        [object]$Threshold,
        [System.Collections.Generic.List[string]]$Errors
    )

    $minValue = Get-ObjectProperty -Object $Threshold -Name "min"
    if ($null -ne $minValue -and $Actual -lt [double]$minValue) {
        $Errors.Add("$Path actual=$Actual below min=$minValue")
    }

    $maxValue = Get-ObjectProperty -Object $Threshold -Name "max"
    if ($null -ne $maxValue -and $Actual -gt [double]$maxValue) {
        $Errors.Add("$Path actual=$Actual above max=$maxValue")
    }
}

$summary = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json
$baseline = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
$transition = Get-TransitionSummary -Summary $summary

$errors = [System.Collections.Generic.List[string]]::new()

$minWindowCount = [int](Get-ObjectProperty -Object $baseline -Name "minWindowCount" -Default 0)
if ([int]$transition.windowCount -lt $minWindowCount) {
    $errors.Add("windowCount actual=$($transition.windowCount) below min=$minWindowCount")
}

$recordCountThresholds = Get-ObjectProperty -Object $baseline -Name "recordCounts"
if ($null -ne $recordCountThresholds) {
    foreach ($property in $recordCountThresholds.PSObject.Properties) {
        $actual = 0.0
        if ($null -ne $transition.recordCounts -and $transition.recordCounts.PSObject.Properties.Name.Contains($property.Name)) {
            $actual = [double]$transition.recordCounts.($property.Name)
        }
        Test-ThresholdObject -Path "recordCounts/$($property.Name)" -Actual $actual -Threshold $property.Value -Errors $errors
    }
}

foreach ($windowThreshold in @(Get-ObjectProperty -Object $baseline -Name "windows" -Default @())) {
    $index = [int](Get-ObjectProperty -Object $windowThreshold -Name "index")
    $window = @($transition.windows | Where-Object { [int]$_.index -eq $index } | Select-Object -First 1)
    if ($window.Count -eq 0) {
        $errors.Add("window[$index] missing")
        continue
    }

    $frameThresholds = Get-ObjectProperty -Object $windowThreshold -Name "frame"
    if ($null -ne $frameThresholds) {
        foreach ($property in $frameThresholds.PSObject.Properties) {
            if ($null -eq $window[0].frame -or -not $window[0].frame.PSObject.Properties.Name.Contains($property.Name)) {
                $errors.Add("window[$index].frame.$($property.Name) missing")
                continue
            }

            $actual = [double]$window[0].frame.($property.Name)
            Test-ThresholdObject -Path "window[$index].frame.$($property.Name)" -Actual $actual -Threshold $property.Value -Errors $errors
        }
    }
}

if ($errors.Count -gt 0) {
    Write-Host "Level transition baseline compare failed: $SummaryPath"
    foreach ($errorText in $errors) {
        Write-Host "  $errorText"
    }
    exit 1
}

Write-Host "Level transition baseline compare passed: $SummaryPath"
