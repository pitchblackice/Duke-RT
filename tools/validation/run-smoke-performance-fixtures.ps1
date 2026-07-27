param(
    [ValidateSet("offscreen", "visible", "reentry", "occluded", "saturation", "rpg-overload")]
    [string[]]$Case = @("offscreen", "visible", "reentry", "occluded", "saturation", "rpg-overload"),

    [string]$ManifestPath = "tools/validation/smoke-performance-fixtures.json",

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [int]$Runs = 0,

    [int]$TimeoutSeconds = 0,

    [string[]]$AdditionalArgs = @(),

    [string]$OutputDirectory = "tools/logs/perf/smoke-fixtures",

    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RequiredProperty {
    param([object]$Object, [string]$Name, [string]$Context)

    if ($null -eq $Object -or -not $Object.PSObject.Properties.Name.Contains($Name)) {
        throw "$Context is missing '$Name'"
    }
    return $Object.$Name
}

function Get-SaveInfo {
    param([string]$Path)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry("info.json")
        if ($null -eq $entry) {
            throw "save fixture has no info.json: $Path"
        }
        $stream = $entry.Open()
        try {
            $reader = [System.IO.StreamReader]::new($stream)
            try {
                return $reader.ReadToEnd() | ConvertFrom-Json
            }
            finally {
                $reader.Dispose()
            }
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Get-SummaryField {
    param([object]$Summary, [string]$Key)

    return @($Summary.fields | Where-Object { [string]$_.key -eq $Key }) | Select-Object -First 1
}

function ConvertFrom-StatusLine {
    param([string]$Line)

    $values = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $values[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $values
}

function Get-SmokeGridStatusRows {
    param([string]$LogPath)

    return @(
        Select-String -LiteralPath $LogPath -Pattern '^NRI PT smoke grid status: ' |
            ForEach-Object { ConvertFrom-StatusLine -Line $_.Line }
    )
}

function Assert-SmokeTimingWorkJoin {
    param([string]$LogPath, [string]$CaseId, [int]$ExpectedSamples)

    $timingFrames = @(
        Select-String -LiteralPath $LogPath -Pattern '^PERF pt smoke gpu timing NRI: ' |
            ForEach-Object { ConvertFrom-StatusLine -Line $_.Line } |
            ForEach-Object { [uint64]$_.renderer_frame }
    )
    $workRows = @(
        Select-String -LiteralPath $LogPath -Pattern '^PERF pt smoke work NRI: ' |
            ForEach-Object { ConvertFrom-StatusLine -Line $_.Line }
    )
    if ($timingFrames.Count -lt $ExpectedSamples -or $workRows.Count -lt $timingFrames.Count) {
        throw "smoke fixture '$CaseId' has incomplete timing/work rows: timing=$($timingFrames.Count) work=$($workRows.Count)"
    }

    $workFrames = @{}
    foreach ($row in $workRows) {
        if ([string]$row.joined -cne '1' -or
            [string]$row.grid_renderer_frame -cne [string]$row.world_renderer_frame -or
            [string]$row.grid_renderer_frame -cne [string]$row.view_renderer_frame) {
            throw "smoke fixture '$CaseId' has an internally unjoined workload row: $LogPath"
        }
        $frame = [uint64]$row.grid_renderer_frame
        if ($workFrames.ContainsKey($frame)) {
            throw "smoke fixture '$CaseId' has duplicate workload identity renderer_frame=$frame"
        }
        $workFrames[$frame] = $true
    }
    $missing = @($timingFrames | Where-Object { -not $workFrames.ContainsKey($_) })
    if ($missing.Count -ne 0) {
        throw "smoke fixture '$CaseId' cannot join $($missing.Count) timing rows to workload identity; first=$($missing[0])"
    }
}

function Get-RequiredStatusUInt64 {
    param([hashtable]$Status, [string]$Name, [string]$Context)

    if (-not $Status.ContainsKey($Name)) {
        throw "$Context is missing smoke grid status field '$Name'"
    }
    $value = [uint64]0
    if (-not [uint64]::TryParse([string]$Status[$Name], [ref]$value)) {
        throw "$Context has nonnumeric smoke grid status field '$Name=$($Status[$Name])'"
    }
    return $value
}

function Assert-SmokeGridStatus {
    param(
        [string]$LogPath,
        [string]$CaseId,
        [uint64]$ExpectedBricks,
        [bool]$PressureExpected
    )

    $rows = @(Get-SmokeGridStatusRows -LogPath $LogPath)
    if ($rows.Count -eq 0) {
        throw "smoke fixture '$CaseId' has no grid-status snapshot: $LogPath"
    }
    $status = $rows[-1]
    foreach ($expected in @(
        @("requested", "yes"),
        @("representation", "1"),
        @("initialized", "yes"),
        @("resources", "ready"),
        @("gpu_stats", "valid")
    )) {
        if (-not $status.ContainsKey($expected[0]) -or [string]$status[$expected[0]] -cne $expected[1]) {
            throw "smoke fixture '$CaseId' has an invalid grid status '$($expected[0])': $LogPath"
        }
    }

    $bricks = Get-RequiredStatusUInt64 $status "bricks" "smoke fixture '$CaseId'"
    $resident = Get-RequiredStatusUInt64 $status "resident" "smoke fixture '$CaseId'"
    $allocated = Get-RequiredStatusUInt64 $status "allocated" "smoke fixture '$CaseId'"
    $commands = Get-RequiredStatusUInt64 $status "commands" "smoke fixture '$CaseId'"
    $allocationFailures = Get-RequiredStatusUInt64 $status "allocation_failures" "smoke fixture '$CaseId'"
    $probeFailures = Get-RequiredStatusUInt64 $status "probe_failures" "smoke fixture '$CaseId'"
    $rejectedMass = Get-RequiredStatusUInt64 $status "rejected_mass_q" "smoke fixture '$CaseId'"
    $saturatedDeposits = Get-RequiredStatusUInt64 $status "saturated" "smoke fixture '$CaseId'"
    $nanRejects = Get-RequiredStatusUInt64 $status "nan" "smoke fixture '$CaseId'"

    if ($bricks -ne $ExpectedBricks -or $resident -gt $ExpectedBricks) {
        throw "smoke fixture '$CaseId' violated its grid capacity: bricks=$bricks resident=$resident expected=$ExpectedBricks"
    }
    if ($allocated -eq [uint64]0 -or $commands -eq [uint64]0) {
        throw "smoke fixture '$CaseId' did not exercise grid allocation and commands: $LogPath"
    }
    if ($probeFailures -ne [uint64]0 -or $nanRejects -ne [uint64]0) {
        throw "smoke fixture '$CaseId' reported probe/NAN failure: probe=$probeFailures nan=$nanRejects"
    }

    if ($PressureExpected) {
        if ($allocationFailures -eq [uint64]0 -and $rejectedMass -eq [uint64]0 -and $saturatedDeposits -eq [uint64]0) {
            throw "smoke fixture '$CaseId' did not produce deterministic grid pressure: $LogPath"
        }
    }
    elseif ($allocationFailures -ne [uint64]0 -or $rejectedMass -ne [uint64]0 -or $saturatedDeposits -ne [uint64]0) {
        throw "nominal smoke fixture '$CaseId' reported allocation/deposition loss: allocation=$allocationFailures rejected_mass=$rejectedMass saturated=$saturatedDeposits"
    }
}

function Assert-FourMapEmittersObserved {
    param([string]$LogPath)

    $rules = @(
        Select-String -LiteralPath $LogPath -Pattern 'NRI PT smoke emitter: event=map-frame-summary .*rule=([^\s]+) .*emitted=[1-9][0-9]* ' -AllMatches |
            ForEach-Object { $_.Matches } |
            ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique
    )
    if ($rules.Count -ne 4) {
        throw "saturation fixture expected four emitting map rules, found $($rules.Count): $LogPath"
    }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$resolvedManifest = Resolve-Path -LiteralPath (Join-Path $repo $ManifestPath) -ErrorAction Stop
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
if ([int](Get-RequiredProperty $manifest "version" "fixture manifest") -ne 1) {
    throw "unsupported smoke fixture manifest version"
}

$saveDirectoryText = [string](Get-RequiredProperty $manifest "saveDirectory" "fixture manifest")
$saveDirectory = [System.IO.Path]::GetFullPath($saveDirectoryText)
$savesById = @{}
foreach ($save in @(Get-RequiredProperty $manifest "saves" "fixture manifest")) {
    $saveId = [string](Get-RequiredProperty $save "id" "save fixture")
    $saveName = [string](Get-RequiredProperty $save "name" "save fixture '$saveId'")
    $savePath = Join-Path $saveDirectory ($saveName + ".dsave")
    if (-not (Test-Path -LiteralPath $savePath -PathType Leaf)) {
        throw "required smoke save fixture is missing: $savePath"
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $savePath).Hash.ToLowerInvariant()
    $expectedHash = [string](Get-RequiredProperty $save "sha256" "save fixture '$saveId'")
    if ($actualHash -cne $expectedHash.ToLowerInvariant()) {
        throw "save fixture '$saveId' SHA-256 mismatch: expected=$expectedHash actual=$actualHash"
    }
    $info = Get-SaveInfo -Path $savePath
    $expectedTitle = [string](Get-RequiredProperty $save "title" "save fixture '$saveId'")
    $expectedMap = [string](Get-RequiredProperty $save "mapLabel" "save fixture '$saveId'")
    if ([string]$info.Title -cne $expectedTitle -or [string]$info.'Map Label' -cne $expectedMap) {
        throw "save fixture '$saveId' metadata mismatch: title=$($info.Title) map=$($info.'Map Label')"
    }
    $savesById[$saveId] = [pscustomobject]@{ manifest = $save; path = $savePath; hash = $actualHash }
}

$casesById = @{}
foreach ($entry in @(Get-RequiredProperty $manifest "cases" "fixture manifest")) {
    $caseId = [string](Get-RequiredProperty $entry "id" "fixture case")
    $casesById[$caseId] = $entry
}

$selected = @(foreach ($caseId in $Case) {
    if (-not $casesById.ContainsKey($caseId)) {
        throw "fixture manifest does not define case '$caseId'"
    }
    $entry = $casesById[$caseId]
    $saveId = [string](Get-RequiredProperty $entry "saveId" "fixture case '$caseId'")
    if (-not $savesById.ContainsKey($saveId)) {
        throw "fixture case '$caseId' references unknown save '$saveId'"
    }
    $scenarioPath = [System.IO.Path]::GetFullPath((Join-Path $repo ([string](Get-RequiredProperty $entry "scenario" "fixture case '$caseId'"))))
    if (-not (Test-Path -LiteralPath $scenarioPath -PathType Leaf)) {
        throw "fixture case '$caseId' scenario is missing: $scenarioPath"
    }
    $scenario = Get-Content -LiteralPath $scenarioPath -Raw | ConvertFrom-Json
    $scenarioSave = Get-RequiredProperty $scenario "save" "scenario '$caseId'"
    $expectedSave = $savesById[$saveId].manifest
    if ([string]$scenarioSave.name -cne [string]$expectedSave.name) {
        throw "fixture case '$caseId' loads '$($scenarioSave.name)', expected '$($expectedSave.name)'"
    }
    if ([System.IO.Path]::GetFullPath([string]$scenarioSave.dir) -cne $saveDirectory) {
        throw "fixture case '$caseId' save.dir does not match the fixture manifest"
    }
    if ([string]$scenario.commands -notmatch "(?i)(?:^|;\s*)load\s+$([regex]::Escape([string]$expectedSave.name))(?=\s*;|$)") {
        throw "fixture case '$caseId' does not load its stable save basename"
    }
    [pscustomobject]@{ id = $caseId; scenarioPath = $scenarioPath; scenario = $scenario }
})

Write-Host "Smoke performance fixtures valid: saves=$($savesById.Count) cases=$($selected.Count)"
if ($ValidateOnly) {
    return
}

$perfRunner = Join-Path $PSScriptRoot "run-nri-perf.ps1"
foreach ($item in $selected) {
    $caseOutput = Join-Path $OutputDirectory $item.id
    $arguments = @{
        ScenarioPath = $item.scenarioPath
        RazePath = $RazePath
        OutputDirectory = $caseOutput
    }
    if ($Runs -gt 0) { $arguments.Runs = $Runs }
    if ($TimeoutSeconds -gt 0) { $arguments.TimeoutSeconds = $TimeoutSeconds }
    if ($AdditionalArgs.Count -gt 0) { $arguments.AdditionalArgs = $AdditionalArgs }
    & $perfRunner @arguments

    $summaryPath = Join-Path $caseOutput "summary.json"
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    if (-not $summary.ok) {
        throw "smoke fixture case '$($item.id)' failed: $summaryPath"
    }

    foreach ($fieldKey in @(
        "PERF pt gpu timing NRI/invalid",
        "PERF pt gpu timing NRI/dropped",
        "PERF pt smoke gpu timing NRI/invalid",
        "PERF pt smoke gpu timing NRI/dropped"
    )) {
        $field = Get-SummaryField $summary $fieldKey
        if ($null -eq $field -or [double]$field.max -ne 0.0) {
            throw "smoke fixture case '$($item.id)' has invalid or dropped GPU timing scopes: $fieldKey"
        }
    }

    $smokeTotal = Get-SummaryField $summary "PERF pt gpu timing NRI/smoke_total"
    $smokeDetail = Get-SummaryField $summary "PERF pt smoke gpu timing NRI/detail_total"
    if ($null -eq $smokeTotal -or $null -eq $smokeDetail) {
        throw "smoke fixture case '$($item.id)' did not publish joined coarse and detailed smoke timing"
    }
    Write-Host ("Smoke GPU characterization: case={0} total_p50={1} total_p95={2} total_p99={3} total_max={4} detail_p95={5}" -f `
        $item.id, $smokeTotal.p50, $smokeTotal.p95, $smokeTotal.p99, $smokeTotal.max, $smokeDetail.p95)

    $runLogs = @(
        foreach ($runResult in @($summary.runs)) {
            $runLogPath = [string](Get-RequiredProperty $runResult "logPath" "smoke fixture run result")
            Get-Item -LiteralPath $runLogPath -ErrorAction Stop
        }
    )
    if ($runLogs.Count -eq 0) {
        throw "smoke fixture case '$($item.id)' produced no run logs"
    }
    $pressureExpected = $item.id -in @("saturation", "rpg-overload")
    $expectedBricks = if ($pressureExpected) { [uint64]64 } else { [uint64]512 }
    $expectedSamples = [int](Get-RequiredProperty $item.scenario.capture "loopTraceFrames" "fixture case '$($item.id)' capture")
    foreach ($runLog in $runLogs) {
        Assert-SmokeTimingWorkJoin -LogPath $runLog.FullName -CaseId $item.id -ExpectedSamples $expectedSamples
        Assert-SmokeGridStatus -LogPath $runLog.FullName -CaseId $item.id `
            -ExpectedBricks $expectedBricks -PressureExpected $pressureExpected
        if ($item.id -eq "saturation") {
            Assert-FourMapEmittersObserved -LogPath $runLog.FullName
        }
        if ($item.id -eq "rpg-overload") {
            $emittedTrail = Select-String -LiteralPath $runLog.FullName -Quiet `
                -Pattern 'NRI PT smoke emitter: event=frame-summary rule=duke_rpg_trail_continuous .* emitted=[1-9][0-9]* '
            if (-not $emittedTrail) {
                throw "smoke fixture case 'rpg-overload' did not emit a continuous RPG trail: $($runLog.FullName)"
            }
        }
    }
}

Write-Host "Smoke performance fixture runs passed: cases=$($selected.Count) output=$OutputDirectory"
