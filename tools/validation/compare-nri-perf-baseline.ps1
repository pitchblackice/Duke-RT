param(
    [Parameter(Mandatory = $true)]
    [string]$SummaryPath,

    [Parameter(Mandatory = $true)]
    [string]$BaselinePath,

    [string]$ScenarioPath
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

function ConvertTo-FieldMap {
    param([object[]]$Fields)

    $map = @{}
    foreach ($field in @($Fields)) {
        $map[[string]$field.key] = $field
    }
    return $map
}

function Add-ThresholdFailure {
    param(
        [System.Collections.Generic.List[string]]$Errors,
        [string]$Name,
        [double]$Actual,
        [double]$Limit
    )

    if ($Actual -gt $Limit) {
        $Errors.Add("threshold '$Name' actual $([Math]::Round($Actual, 3)) > limit $([Math]::Round($Limit, 3))")
    }
}

$summary = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json
$baseline = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
$scenario = if ($ScenarioPath) {
    Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
}
else {
    Get-ObjectProperty -Object $summary -Name "scenario" -Default (Get-ObjectProperty -Object $baseline -Name "scenario")
}

$errors = New-Object System.Collections.Generic.List[string]
if (-not [bool]$summary.ok) {
    $errors.Add("summary ok=false")
    foreach ($errorText in @($summary.errors)) {
        $errors.Add([string]$errorText)
    }
}

$thresholds = Get-ObjectProperty -Object $scenario -Name "thresholds" -Default (Get-ObjectProperty -Object $baseline -Name "thresholds")
$currentFields = ConvertTo-FieldMap -Fields @($summary.fields)
$baselineFields = ConvertTo-FieldMap -Fields @($baseline.fields)

if ($null -ne $thresholds) {
    if ($thresholds.PSObject.Properties.Name.Contains("frameMsP95Max")) {
        Add-ThresholdFailure -Errors $errors -Name "frameMsP95Max" -Actual ([double]$summary.loopTrace.p95) -Limit ([double]$thresholds.frameMsP95Max)
    }
    if ($thresholds.PSObject.Properties.Name.Contains("frameMsMaxMax")) {
        Add-ThresholdFailure -Errors $errors -Name "frameMsMaxMax" -Actual ([double]$summary.loopTrace.max) -Limit ([double]$thresholds.frameMsMaxMax)
    }
    if ($thresholds.PSObject.Properties.Name.Contains("framesOver100Max")) {
        Add-ThresholdFailure -Errors $errors -Name "framesOver100Max" -Actual ([double]$summary.loopTrace.framesOver100) -Limit ([double]$thresholds.framesOver100Max)
    }
    $thresholdFieldMap = @{
        renderTotalP95Max = "PERF render trace NRI/total"
        shellSelectP95Max = "PERF pt shell trace NRI/select"
        shellLightsP95Max = "PERF pt shell trace NRI/lights"
    }
    foreach ($property in $thresholdFieldMap.Keys) {
        if ($thresholds.PSObject.Properties.Name.Contains($property)) {
            $key = $thresholdFieldMap[$property]
            if ($currentFields.ContainsKey($key)) {
                Add-ThresholdFailure -Errors $errors -Name $property -Actual ([double]$currentFields[$key].p95) -Limit ([double]$thresholds.$property)
            }
        }
    }
}

$baselineCompare = Get-ObjectProperty -Object $scenario -Name "baselineCompare" -Default (Get-ObjectProperty -Object $baseline -Name "baselineCompare")
$allowRelativeRegressionPercent = [double](Get-ObjectProperty -Object $baselineCompare -Name "allowRelativeRegressionPercent" -Default 10.0)
$fieldList = @(Get-ObjectProperty -Object $baselineCompare -Name "fields")
if ($fieldList.Count -eq 0) {
    $fieldList = @($baseline.fields | ForEach-Object { [string]$_.key })
}

$multiplier = 1.0 + ($allowRelativeRegressionPercent / 100.0)
foreach ($key in $fieldList) {
    $fieldKey = [string]$key
    if (-not $currentFields.ContainsKey($fieldKey)) {
        $errors.Add("current summary missing field '$fieldKey'")
        continue
    }
    if (-not $baselineFields.ContainsKey($fieldKey)) {
        $errors.Add("baseline missing field '$fieldKey'")
        continue
    }

    $current = $currentFields[$fieldKey]
    $base = $baselineFields[$fieldKey]
    foreach ($metric in @("avg", "p95")) {
        $baseValue = [double]$base.$metric
        $currentValue = [double]$current.$metric
        if ($baseValue -le 0.0) {
            continue
        }
        $limit = $baseValue * $multiplier
        if ($currentValue -gt $limit) {
            $errors.Add("field '$fieldKey' $metric regressed: current=$([Math]::Round($currentValue, 3)) baseline=$([Math]::Round($baseValue, 3)) limit=$([Math]::Round($limit, 3))")
        }
    }
}

if ($errors.Count -gt 0) {
    foreach ($errorText in $errors) {
        Write-Error $errorText -ErrorAction Continue
    }
    exit 1
}

Write-Host "NRI perf baseline compare passed: summary=$SummaryPath baseline=$BaselinePath fields=$($fieldList.Count) allowed_regression_percent=$allowRelativeRegressionPercent"
