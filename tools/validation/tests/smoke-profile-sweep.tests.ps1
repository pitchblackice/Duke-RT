Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$runnerPath = Join-Path $repo "tools/validation/run-smoke-profile-sweep.ps1"
$runner = Get-Content -LiteralPath $runnerPath -Raw
$testRoot = Join-Path $env:TEMP ("smoke-profile-sweep-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $scenarioPath = Join-Path $testRoot "scenario.json"
    $profilePath = Join-Path $testRoot "profiles.json"
    [pscustomobject]@{
        name = "test"
        commands = "+wait 1; perf_compactframes 256"
        capture = [pscustomobject]@{ loopTraceFrames = 256 }
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $scenarioPath -Encoding UTF8
    [pscustomobject]@{
        version = 1
        profiles = @(
            [pscustomobject]@{ id = "a"; additionalArgs = @("+set", "test_profile", "a") },
            [pscustomobject]@{ id = "b"; additionalArgs = @("+set", "test_profile", "b") },
            [pscustomobject]@{ id = "c"; additionalArgs = @("+set", "test_profile", "c") }
        )
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $profilePath -Encoding UTF8

    $validation = (& $runnerPath -ScenarioPath $scenarioPath -ProfileSpecPath $profilePath -ValidateOnly 6>&1) -join "`n"
    foreach ($expected in @(
        "repeat=1 slot=1 profile=a", "repeat=1 slot=2 profile=b", "repeat=1 slot=3 profile=c",
        "repeat=2 slot=1 profile=b", "repeat=2 slot=2 profile=c", "repeat=2 slot=3 profile=a",
        "repeat=3 slot=1 profile=c", "repeat=3 slot=2 profile=a", "repeat=3 slot=3 profile=b"
    )) {
        if ($validation -notmatch [regex]::Escape($expected)) { throw "missing cyclic schedule row '$expected'" }
    }

    foreach ($contract in @(
        'for \(\$repeat = 0; \$repeat -lt 3;',
        'status=complete .*requested=256 .*eligible=256 .*observed=256 .*dropped=0',
        '\$body -notmatch .*compact=1',
        'acceptedFrames = \$logs.Count \* 256',
        'AdditionalArgs = @\(\$AdditionalArgs\) \+ @\(\$profile.additionalArgs'
    )) {
        if ($runner -notmatch $contract) { throw "runner is missing contract '$contract'" }
    }

    $harness = Join-Path $testRoot "harness"
    New-Item -ItemType Directory -Force -Path $harness | Out-Null
    Copy-Item -LiteralPath $runnerPath -Destination (Join-Path $harness "run-smoke-profile-sweep.ps1")
    @'
param([string]$ScenarioPath, [string]$RazePath, [int]$Runs, [string]$OutputDirectory,
    [string[]]$AdditionalArgs, [string]$GameGrp, [string]$File, [int]$TimeoutSeconds)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$profile = $AdditionalArgs[-1]
$log = Join-Path $OutputDirectory "run-1.log"
$lines = [Collections.Generic.List[string]]::new()
for ($frame = 0; $frame -lt 256; ++$frame) {
    $value = switch ($profile) { "a" { 1 } "b" { 2 } default { 3 } }
    $lines.Add("PERF loop trace: frame=$frame frame_ms=$value compact=1 sample=$frame")
    $lines.Add("PERF pt smoke gpu timing NRI: frame=$frame total=$value compact=1 sample=$frame")
}
$lines.Add("PERF compact capture complete: epoch=1 status=complete requested=256 eligible=256 observed=256 pending_gpu=0 dropped=0 reason=none")
$lines | Set-Content -LiteralPath $log -Encoding UTF8
[pscustomobject]@{ ok = $true; loopTrace = [pscustomobject]@{ samples = 256 } } |
    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $OutputDirectory "summary.json") -Encoding UTF8
'@ | Set-Content -LiteralPath (Join-Path $harness "run-nri-perf.ps1") -Encoding UTF8

    $summaryPath = Join-Path $testRoot "summary.json"
    & (Join-Path $harness "run-smoke-profile-sweep.ps1") -ScenarioPath $scenarioPath `
        -ProfileSpecPath $profilePath -OutputDirectory (Join-Path $testRoot "runs") -SummaryOutput $summaryPath
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if (-not $summary.ok -or @($summary.runs).Count -ne 9) { throw "fake sweep did not complete nine interleaved runs" }
    foreach ($profileSummary in @($summary.profiles)) {
        if ([int]$profileSummary.acceptedFrames -ne 768) { throw "profile '$($profileSummary.id)' did not pool 768 frames" }
        $timing = @($profileSummary.pooledFields | Where-Object { $_.key -eq "PERF pt smoke gpu timing NRI/total" }) | Select-Object -First 1
        if ($null -eq $timing -or [int]$timing.samples -ne 768) { throw "profile '$($profileSummary.id)' did not pool 768 compact timing rows" }
    }
}
finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Host "Smoke profile sweep tests passed."
