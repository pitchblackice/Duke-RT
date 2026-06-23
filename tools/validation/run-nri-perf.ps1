param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File,

    [int]$Runs = 0,

    [int]$TimeoutSeconds = 0,

    [switch]$Build,

    [string]$OutputDirectory,

    [string]$SummaryOutput
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

function ConvertTo-NriProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Argument)

    if ($Argument.Length -eq 0) {
        return '""'
    }
    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    $escaped = $Argument -replace '(\\+)$', '$1$1'
    $escaped = $escaped.Replace('"', '\"')
    return '"' + $escaped + '"'
}

function Join-NriProcessArguments {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $quoted = foreach ($argument in $Arguments) {
        ConvertTo-NriProcessArgument -Argument $argument
    }
    return ($quoted -join ' ')
}

function Stop-NriPerfProcess {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process -or $Process.HasExited) {
        return
    }

    try {
        $Process.Kill()
        $Process.WaitForExit(5000) | Out-Null
    }
    catch {
    }
}

function Count-LoopTraceSamples {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return 0
    }

    $count = 0
    $stream = [System.IO.File]::Open((Resolve-Path -LiteralPath $Path).Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $reader = [System.IO.StreamReader]::new($stream)
        try {
            while ($true) {
                $line = $reader.ReadLine()
                if ($null -eq $line) {
                    break
                }
                if ($line.StartsWith("PERF loop trace:", [System.StringComparison]::Ordinal)) {
                    $count++
                }
            }
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
    return $count
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if ($Values.Count -eq 0) {
        return 0.0
    }
    $sorted = @($Values | Sort-Object)
    $rank = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, [int]$rank))
    return [double]$sorted[$index]
}

function Get-Median {
    param([double[]]$Values)

    return Get-Percentile -Values $Values -Percentile 50
}

function Build-PerfCommandString {
    param([object]$Scenario)

    $capture = Get-ObjectProperty -Object $Scenario -Name "capture"
    $save = Get-ObjectProperty -Object $Scenario -Name "save"
    $loopTraceFrames = [int](Get-ObjectProperty -Object $capture -Name "loopTraceFrames" -Default 256)

    $explicitCommands = Get-ObjectProperty -Object $Scenario -Name "commands"
    if ($null -ne $explicitCommands -and [string]$explicitCommands -ne "") {
        return [string]$explicitCommands
    }

    if ($null -eq $save) {
        throw "scenario must define either 'commands' or 'save'"
    }

    $saveName = [string](Get-ObjectProperty -Object $save -Name "name")
    if ([string]::IsNullOrWhiteSpace($saveName)) {
        throw "scenario save block must define 'name'"
    }

    $startupWaitTics = [int](Get-ObjectProperty -Object $save -Name "startupWaitTics" -Default 45)
    $settleTics = [int](Get-ObjectProperty -Object $save -Name "settleTics" -Default 35)
    $warmupTics = [int](Get-ObjectProperty -Object $save -Name "warmupTics" -Default 10)
    return "+wait $startupWaitTics; load $saveName; wait $settleTics; closemenu; wait $warmupTics; perf_looptraceframes $loopTraceFrames"
}

function Convert-RunSummariesToAggregate {
    param(
        [object]$Scenario,
        [object[]]$RunResults,
        [object[]]$RunSummaries,
        [string]$OutputDirectory,
        [string]$ManifestPath
    )

    $fieldKeys = New-Object System.Collections.Generic.HashSet[string]
    foreach ($summary in $RunSummaries) {
        foreach ($field in @($summary.fields)) {
            [void]$fieldKeys.Add([string]$field.key)
        }
    }

    $aggregateFields = foreach ($key in @($fieldKeys | Sort-Object)) {
        $rows = @($RunSummaries | ForEach-Object { $summary = $_; @($summary.fields) | Where-Object { [string]$_.key -eq $key } })
        if ($rows.Count -eq 0) {
            continue
        }
        $first = $rows | Select-Object -First 1
        [pscustomobject]@{
            key = $key
            prefix = $first.prefix
            field = $first.field
            runs = $rows.Count
            samples = (@($rows | ForEach-Object { [int]$_.samples }) | Measure-Object -Sum).Sum
            avg = [Math]::Round((Get-Median -Values ([double[]]@($rows | ForEach-Object { [double]$_.avg }))), 3)
            p50 = [Math]::Round((Get-Median -Values ([double[]]@($rows | ForEach-Object { [double]$_.p50 }))), 3)
            p90 = [Math]::Round((Get-Median -Values ([double[]]@($rows | ForEach-Object { [double]$_.p90 }))), 3)
            p95 = [Math]::Round((Get-Median -Values ([double[]]@($rows | ForEach-Object { [double]$_.p95 }))), 3)
            p99 = [Math]::Round((Get-Median -Values ([double[]]@($rows | ForEach-Object { [double]$_.p99 }))), 3)
            max = [Math]::Round((@($rows | ForEach-Object { [double]$_.max }) | Measure-Object -Maximum).Maximum, 3)
            maxFrame = (@($rows | Sort-Object @{ Expression = "max"; Descending = $true }) | Select-Object -First 1).maxFrame
        }
    }

    $loopRows = @($RunSummaries | ForEach-Object { $_.loopTrace })
    $allErrors = @($RunResults | ForEach-Object { @($_.errors) }) + @($RunSummaries | ForEach-Object { @($_.errors) })
    $allForbidden = @($RunSummaries | ForEach-Object { @($_.forbiddenHits) })
    $ok = (@($RunResults | Where-Object { -not $_.ok }).Count -eq 0) -and (@($RunSummaries | Where-Object { -not $_.ok }).Count -eq 0)

    return [pscustomobject]@{
        ok = $ok
        scenario = $Scenario
        outputDirectory = $OutputDirectory
        manifestPath = $ManifestPath
        runCount = $RunResults.Count
        completedRunCount = @($RunResults | Where-Object { $_.ok }).Count
        errors = @($allErrors | Where-Object { $null -ne $_ -and [string]$_ -ne "" })
        forbiddenHits = $allForbidden
        loopTrace = [pscustomobject]@{
            samples = (@($loopRows | ForEach-Object { [int]$_.samples }) | Measure-Object -Sum).Sum
            p50 = [Math]::Round((Get-Median -Values ([double[]]@($loopRows | ForEach-Object { [double]$_.p50 }))), 3)
            p90 = [Math]::Round((Get-Median -Values ([double[]]@($loopRows | ForEach-Object { [double]$_.p90 }))), 3)
            p95 = [Math]::Round((Get-Median -Values ([double[]]@($loopRows | ForEach-Object { [double]$_.p95 }))), 3)
            p99 = [Math]::Round((Get-Median -Values ([double[]]@($loopRows | ForEach-Object { [double]$_.p99 }))), 3)
            max = [Math]::Round((@($loopRows | ForEach-Object { [double]$_.max }) | Measure-Object -Maximum).Maximum, 3)
            framesOver50 = (@($loopRows | ForEach-Object { [int]$_.framesOver50 }) | Measure-Object -Sum).Sum
            framesOver100 = (@($loopRows | ForEach-Object { [int]$_.framesOver100 }) | Measure-Object -Sum).Sum
            framesOver200 = (@($loopRows | ForEach-Object { [int]$_.framesOver200 }) | Measure-Object -Sum).Sum
        }
        fields = @($aggregateFields | Sort-Object @{ Expression = "p95"; Descending = $true }, @{ Expression = "avg"; Descending = $true }, key)
        runs = $RunResults
        runSummaries = $RunSummaries
    }
}

$scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
$name = [string](Get-ObjectProperty -Object $scenario -Name "name" -Default "nri-perf")
$capture = Get-ObjectProperty -Object $scenario -Name "capture"
$launch = Get-ObjectProperty -Object $scenario -Name "launch"
$save = Get-ObjectProperty -Object $scenario -Name "save"

if ($Runs -le 0) {
    $Runs = [int](Get-ObjectProperty -Object $capture -Name "runs" -Default 1)
}
if ($TimeoutSeconds -le 0) {
    $TimeoutSeconds = [int](Get-ObjectProperty -Object $capture -Name "timeoutSeconds" -Default 180)
}
$loopTraceFrames = [int](Get-ObjectProperty -Object $capture -Name "loopTraceFrames" -Default 256)
$stopWhenCaptured = [bool](Get-ObjectProperty -Object $capture -Name "stopWhenLoopTraceFramesCaptured" -Default $true)

$scenarioFile = [string](Get-ObjectProperty -Object $launch -Name "file")
$scenarioGameGrp = [string](Get-ObjectProperty -Object $launch -Name "gameGrp")
if (-not $File -and $scenarioFile) {
    $File = $scenarioFile
}
if (-not $GameGrp -and $scenarioGameGrp) {
    $GameGrp = $scenarioGameGrp
}

if ($Build) {
    $buildCommand = 'call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze'
    $buildProcess = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $buildCommand -PassThru -Wait -NoNewWindow
    if ($buildProcess.ExitCode -ne 0) {
        throw "build failed with exit code $($buildProcess.ExitCode)"
    }
}

$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/perf/$name/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $OutputDirectory "summary.json"
}

$runResults = @()
$runSummaries = @()
for ($runIndex = 1; $runIndex -le $Runs; ++$runIndex) {
    $logPath = Join-Path $OutputDirectory ("run-{0}.log" -f $runIndex)
    $summaryPath = Join-Path $OutputDirectory ("run-{0}.summary.json" -f $runIndex)
    $csvPath = Join-Path $OutputDirectory ("run-{0}.timing.csv" -f $runIndex)
    $commands = Build-PerfCommandString -Scenario $scenario

    $args = New-Object System.Collections.Generic.List[string]
    if ($File) {
        $args.Add("-file")
        $args.Add($File)
    }
    if ($GameGrp) {
        $args.Add("-gamegrp")
        $args.Add($GameGrp)
    }
    $saveDir = [string](Get-ObjectProperty -Object $save -Name "dir")
    if ($saveDir) {
        $args.Add("-savedir")
        $args.Add($saveDir)
    }
    $args.Add("-nosound")
    $args.Add("-nologo")
    $args.Add("+set")
    $args.Add("vid_preferbackend")
    $args.Add("4")
    $backend = [string](Get-ObjectProperty -Object $scenario -Name "backend" -Default "d3d12")
    if ($backend) {
        $args.Add("+set")
        $args.Add("nri_api")
        $args.Add($backend)
    }
    $args.Add("+set")
    $args.Add("developer")
    $args.Add("1")

    $topLevelExtraArgs = @(Get-ObjectProperty -Object $scenario -Name "extraArgs" -Default @())
    foreach ($extra in $topLevelExtraArgs) {
        $args.Add([string]$extra)
    }
    $launchExtraArgs = @(Get-ObjectProperty -Object $launch -Name "extraArgs" -Default @())
    foreach ($extra in $launchExtraArgs) {
        $args.Add([string]$extra)
    }

    $args.Add("+logfile")
    $args.Add($logPath.Replace('\', '/'))
    $args.Add($commands)

    $processStart = [System.Diagnostics.ProcessStartInfo]::new()
    $processStart.FileName = $resolvedRaze.Path
    $processStart.UseShellExecute = $false
    $processStart.CreateNoWindow = $true
    $processStart.RedirectStandardOutput = $true
    $processStart.RedirectStandardError = $true
    $processStart.Arguments = Join-NriProcessArguments -Arguments $args.ToArray()

    Write-Host "NRI perf run ${runIndex}/${Runs}: scenario=$name log=$logPath"
    $process = [System.Diagnostics.Process]::Start($processStart)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $timedOut = $false
    $stoppedAfterCapture = $false
    while ((Get-Date) -lt $deadline) {
        if ($stopWhenCaptured) {
            $samples = Count-LoopTraceSamples -Path $logPath
            if ($samples -ge $loopTraceFrames) {
                $stoppedAfterCapture = $true
                Stop-NriPerfProcess -Process $process
                break
            }
        }

        if ($process.HasExited) {
            break
        }
        Start-Sleep -Seconds 1
    }

    if (-not $process.HasExited -and -not $stoppedAfterCapture) {
        $timedOut = $true
        Stop-NriPerfProcess -Process $process
    }
    $stdoutTask.Wait(5000) | Out-Null
    $stderrTask.Wait(5000) | Out-Null

    & (Join-Path $PSScriptRoot "analyze-nri-perf-log.ps1") -LogPath $logPath -ScenarioPath $ScenarioPath -SummaryOutput $summaryPath -CsvPath $csvPath
    $runSummary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    $runErrors = New-Object System.Collections.Generic.List[string]
    if ($timedOut) {
        $runErrors.Add("run timed out after $TimeoutSeconds seconds")
    }
    if (-not $stoppedAfterCapture -and -not $timedOut -and $process.ExitCode -ne 0) {
        $runErrors.Add("run exited with code $($process.ExitCode)")
    }
    if (-not $runSummary.ok) {
        foreach ($errorText in @($runSummary.errors)) {
            $runErrors.Add([string]$errorText)
        }
    }

    $runResult = [pscustomobject]@{
        run = $runIndex
        ok = $runErrors.Count -eq 0
        logPath = $logPath
        summaryPath = $summaryPath
        csvPath = $csvPath
        exitCode = if ($stoppedAfterCapture -or $timedOut) { $null } else { $process.ExitCode }
        timedOut = $timedOut
        stoppedAfterCapture = $stoppedAfterCapture
        stdoutLength = if ($stdoutTask.IsCompleted) { $stdoutTask.Result.Length } else { 0 }
        stderrLength = if ($stderrTask.IsCompleted) { $stderrTask.Result.Length } else { 0 }
        errors = $runErrors.ToArray()
    }
    $runResults += $runResult
    $runSummaries += $runSummary
}

$manifest = [pscustomobject]@{
    scenarioPath = (Resolve-Path -LiteralPath $ScenarioPath).Path
    scenario = $scenario
    razePath = $resolvedRaze.Path
    outputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
    runs = $runResults
}
$manifestPath = Join-Path $OutputDirectory "manifest.json"
$manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

$aggregate = Convert-RunSummariesToAggregate -Scenario $scenario -RunResults $runResults -RunSummaries $runSummaries -OutputDirectory (Resolve-Path -LiteralPath $OutputDirectory).Path -ManifestPath $manifestPath
$aggregate | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8

Write-Host "NRI perf suite complete: ok=$($aggregate.ok) runs=$($aggregate.runCount) summary=$SummaryOutput"
if (-not $aggregate.ok) {
    foreach ($errorText in @($aggregate.errors)) {
        Write-Host "  $errorText"
    }
    exit 1
}
