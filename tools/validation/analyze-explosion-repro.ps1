param(
    [Parameter(Mandatory = $true)]
    [string]$LogDirectory,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-KeyValuePairs {
    param([Parameter(Mandatory = $true)][string]$Line)

    $pairs = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\([^\)]*\)|[^\s]+)')) {
        $value = $match.Groups[2].Value
        if ($value.Length -ge 2 -and $value[0] -eq '"' -and $value[$value.Length - 1] -eq '"') {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $pairs[$match.Groups[1].Value] = $value
    }
    return $pairs
}

function Add-Example {
    param(
        [System.Collections.Generic.List[string]]$Examples,
        [string]$Line,
        [int]$Limit = 8
    )

    if ($Examples.Count -lt $Limit) {
        $Examples.Add($Line)
    }
}

$resolvedLogDirectory = Resolve-Path -LiteralPath $LogDirectory -ErrorAction Stop
$logFiles = @(Get-ChildItem -LiteralPath $resolvedLogDirectory.Path -Filter "*.log" -File | Sort-Object Name)
if ($logFiles.Count -eq 0) {
    throw "no .log files found in $($resolvedLogDirectory.Path)"
}

$forbiddenPatterns = @(
    "Device removed",
    "DRED page fault",
    "DRED breadcrumbs",
    "DXGI_ERROR_DEVICE",
    "validation error",
    "failed to create",
    "assertion failed",
    "Fatal error",
    "NRI render crash"
)

$logSummaries = @()
$totalActorRows = 0
$totalActorEmittedRows = 0
$totalActorMissingRows = 0
$totalMaterialRows = 0
$totalMaterialNoShadowFailures = 0
$totalLightRows = 0
$totalCacheDrops = 0
$totalForbiddenMatches = 0
$totalUnstableKeys = 0
$examples = [System.Collections.Generic.List[string]]::new()

foreach ($logFile in $logFiles) {
    $actorRows = 0
    $actorEmittedRows = 0
    $actorMissingRows = 0
    $materialRows = 0
    $materialNoShadowFailures = 0
    $lightRows = 0
    $cacheDrops = 0
    $forbiddenMatches = 0
    $stableKeysByActorRule = @{}
    $lightRowsByActorRule = @{}
    $actorEmittedByActorRule = @{}

    foreach ($line in [System.IO.File]::ReadLines($logFile.FullName)) {
        foreach ($pattern in $forbiddenPatterns) {
            if ($line.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $forbiddenMatches++
                Add-Example -Examples $examples -Line "$($logFile.Name): forbidden: $line"
                break
            }
        }

        if ($line.StartsWith("NRI PT actor-sprite cache:") -and $line.Contains("action=drop")) {
            $cacheDrops++
        }

        if ($line.StartsWith("NRI PT explosion actor:")) {
            $actorRows++
            $pairs = Read-KeyValuePairs -Line $line
            $actor = if ($pairs.ContainsKey("actor")) { $pairs["actor"] } else { "unknown" }
            $rule = if ($pairs.ContainsKey("rule")) { $pairs["rule"] } else { "unknown" }
            $key = "$actor/$rule"
            if ($pairs.ContainsKey("emitted") -and $pairs["emitted"] -eq "yes") {
                $actorEmittedRows++
                if (-not $actorEmittedByActorRule.ContainsKey($key)) {
                    $actorEmittedByActorRule[$key] = 0
                }
                $actorEmittedByActorRule[$key] = [int]$actorEmittedByActorRule[$key] + 1
            }
            elseif ($pairs.ContainsKey("emitted") -and $pairs["emitted"] -eq "no") {
                $actorMissingRows++
                Add-Example -Examples $examples -Line "$($logFile.Name): actor emitted=no: $line"
            }
        }
        elseif ($line.StartsWith("NRI PT explosion material:")) {
            $materialRows++
            $pairs = Read-KeyValuePairs -Line $line
            $receiveOk = $pairs.ContainsKey("no_shadow_receive") -and $pairs["no_shadow_receive"] -eq "yes"
            $castOk = $pairs.ContainsKey("no_shadow_cast") -and $pairs["no_shadow_cast"] -eq "yes"
            if (-not ($receiveOk -and $castOk)) {
                $materialNoShadowFailures++
                Add-Example -Examples $examples -Line "$($logFile.Name): material no-shadow missing: $line"
            }
        }
        elseif ($line.StartsWith("NRI PT explosion light:")) {
            $lightRows++
            $pairs = Read-KeyValuePairs -Line $line
            $actor = if ($pairs.ContainsKey("actor")) { $pairs["actor"] } else { "unknown" }
            $rule = if ($pairs.ContainsKey("rule")) { $pairs["rule"] } else { "unknown" }
            $stable = if ($pairs.ContainsKey("stable")) { $pairs["stable"] } else { "unknown" }
            $key = "$actor/$rule"
            if (-not $stableKeysByActorRule.ContainsKey($key)) {
                $stableKeysByActorRule[$key] = [System.Collections.Generic.HashSet[string]]::new()
            }
            [void]$stableKeysByActorRule[$key].Add($stable)
            if (-not $lightRowsByActorRule.ContainsKey($key)) {
                $lightRowsByActorRule[$key] = 0
            }
            $lightRowsByActorRule[$key] = [int]$lightRowsByActorRule[$key] + 1
        }
    }

    $unstableKeys = 0
    foreach ($key in $stableKeysByActorRule.Keys) {
        if ($stableKeysByActorRule[$key].Count -gt 1) {
            $unstableKeys++
            Add-Example -Examples $examples -Line "$($logFile.Name): unstable light key actor_rule=$key count=$($stableKeysByActorRule[$key].Count)"
        }
    }

    foreach ($key in $actorEmittedByActorRule.Keys) {
        if (-not $lightRowsByActorRule.ContainsKey($key)) {
            Add-Example -Examples $examples -Line "$($logFile.Name): actor emitted rows without light rows actor_rule=$key count=$($actorEmittedByActorRule[$key])"
            $actorMissingRows += [int]$actorEmittedByActorRule[$key]
        }
    }

    $logSummary = [pscustomobject]@{
        path = $logFile.FullName
        actorRows = $actorRows
        actorEmittedRows = $actorEmittedRows
        actorMissingRows = $actorMissingRows
        materialRows = $materialRows
        materialNoShadowFailures = $materialNoShadowFailures
        lightRows = $lightRows
        cacheDrops = $cacheDrops
        forbiddenMatches = $forbiddenMatches
        unstableLightKeys = $unstableKeys
    }
    $logSummaries += $logSummary

    $totalActorRows += $actorRows
    $totalActorEmittedRows += $actorEmittedRows
    $totalActorMissingRows += $actorMissingRows
    $totalMaterialRows += $materialRows
    $totalMaterialNoShadowFailures += $materialNoShadowFailures
    $totalLightRows += $lightRows
    $totalCacheDrops += $cacheDrops
    $totalForbiddenMatches += $forbiddenMatches
    $totalUnstableKeys += $unstableKeys
}

$errors = [System.Collections.Generic.List[string]]::new()
if ($totalActorRows -le 0) {
    $errors.Add("no NRI PT explosion actor rows were found")
}
if ($totalLightRows -le 0) {
    $errors.Add("no NRI PT explosion light rows were found")
}
if ($totalActorMissingRows -gt 0) {
    $errors.Add("explosion actor rows reported missing lights: $totalActorMissingRows")
}
if ($totalMaterialNoShadowFailures -gt 0) {
    $errors.Add("explosion material rows missing no-shadow flags: $totalMaterialNoShadowFailures")
}
if ($totalForbiddenMatches -gt 0) {
    $errors.Add("forbidden device/runtime log patterns found: $totalForbiddenMatches")
}
if ($totalUnstableKeys -gt 0) {
    $errors.Add("unstable explosion actor/rule light keys found: $totalUnstableKeys")
}

$summary = [pscustomobject]@{
    ok = $errors.Count -eq 0
    logDirectory = $resolvedLogDirectory.Path
    logCount = $logFiles.Count
    actorRows = $totalActorRows
    actorEmittedRows = $totalActorEmittedRows
    actorMissingRows = $totalActorMissingRows
    materialRows = $totalMaterialRows
    materialNoShadowFailures = $totalMaterialNoShadowFailures
    lightRows = $totalLightRows
    cacheDrops = $totalCacheDrops
    forbiddenMatches = $totalForbiddenMatches
    unstableLightKeys = $totalUnstableKeys
    errors = @($errors)
    examples = @($examples)
    logs = $logSummaries
}

if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $resolvedLogDirectory.Path "summary.json"
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

if (-not $summary.ok) {
    Write-Host "Explosion repro analysis failed: summary=$SummaryOutput"
    foreach ($errorText in $errors) {
        Write-Error $errorText -ErrorAction Continue
    }
    foreach ($example in $examples) {
        Write-Host $example
    }
    exit 1
}

Write-Host "Explosion repro analysis passed: logs=$($summary.logCount) actors=$($summary.actorRows) lights=$($summary.lightRows) materials=$($summary.materialRows) cache_drops=$($summary.cacheDrops) summary=$SummaryOutput"
