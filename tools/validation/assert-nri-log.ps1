param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [string]$ScenarioPath,

    [int]$MinSelfTestFrames = 1,

    [string[]]$RequiredPrefix,

    [string[]]$ForbiddenPattern,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "lib/NriValidationLog.psm1") -Force

$scenario = $null
if ($ScenarioPath) {
    $scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
    if ($scenario.PSObject.Properties.Name.Contains("minSelfTestFrames")) {
        $MinSelfTestFrames = [int]$scenario.minSelfTestFrames
    }
    if (-not $RequiredPrefix -and $scenario.PSObject.Properties.Name.Contains("requiredPrefixes")) {
        $RequiredPrefix = @($scenario.requiredPrefixes)
    }
    if (-not $ForbiddenPattern -and $scenario.PSObject.Properties.Name.Contains("forbiddenPatterns")) {
        $ForbiddenPattern = @($scenario.forbiddenPatterns)
    }
}

if (-not $RequiredPrefix) {
    $RequiredPrefix = @("NRI PT selftest:")
}
if (-not $ForbiddenPattern) {
    $ForbiddenPattern = @("Device removed", "validation error", "failed to create", "assertion failed")
}

$summary = Get-NriValidationLogSummary -Path $InputPath -RequiredPrefixes $RequiredPrefix -ForbiddenPatterns $ForbiddenPattern
$result = Test-NriValidationSummary -Summary $summary -MinSelfTestFrames $MinSelfTestFrames
$loadingResult = [pscustomobject]@{
    ok = $true
    errors = @()
}
if ($null -ne $scenario -and $scenario.PSObject.Properties.Name.Contains("loadingAssertions")) {
    $loadingResult = Test-NriLoadingAssertions -Summary $summary -Assertions $scenario.loadingAssertions
}

if ($SummaryOutput) {
    $summaryDirectory = Split-Path -Parent $SummaryOutput
    if ($summaryDirectory) {
        New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null
    }
    $summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
}

if (-not $result.ok -or -not $loadingResult.ok) {
    foreach ($errorText in $result.errors) {
        Write-Error $errorText -ErrorAction Continue
    }
    foreach ($errorText in $loadingResult.errors) {
        Write-Error $errorText -ErrorAction Continue
    }
    exit 1
}

Write-Host "NRI validation log passed: accepted_selftest_frames=$($summary.acceptedSelfTestFrameCount) path=$($summary.path)"
