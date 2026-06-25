param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp,

    [string]$File,

    [int]$TimeoutSeconds = 120,

    [switch]$Build,

    [switch]$CaptureWhenPassed,

    [string]$LogPath,

    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

$scenario = Get-Content -LiteralPath $ScenarioPath -Raw | ConvertFrom-Json
$name = if ($scenario.PSObject.Properties.Name.Contains("name")) { [string]$scenario.name } else { "nri-validation" }
$scenarioCaptureWhenPassed = $scenario.PSObject.Properties.Name.Contains("captureWhenPassed") -and [bool]$scenario.captureWhenPassed
$CaptureWhenPassed = $CaptureWhenPassed -or $scenarioCaptureWhenPassed
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDirectory = Join-Path (Get-Location) "tools/logs/validation"
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

if (-not $LogPath) {
    $LogPath = Join-Path $logDirectory "$name-$timestamp.log"
}
if (-not $SummaryOutput) {
    $SummaryOutput = Join-Path $logDirectory "$name-$timestamp.summary.json"
}

if ($Build) {
    $buildCommand = 'call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze'
    $buildProcess = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $buildCommand -PassThru -Wait -NoNewWindow
    if ($buildProcess.ExitCode -ne 0) {
        throw "build failed with exit code $($buildProcess.ExitCode)"
    }
}

$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
$args = New-Object System.Collections.Generic.List[string]

if ($File) {
    $args.Add("-file")
    $args.Add($File)
}
if ($GameGrp) {
    $args.Add("-gamegrp")
    $args.Add($GameGrp)
}
if ($scenario.PSObject.Properties.Name.Contains("save")) {
    $save = $scenario.save
    if ($save.PSObject.Properties.Name.Contains("dir")) {
        $args.Add("-savedir")
        $args.Add([string]$save.dir)
    }
}

$args.Add("-nosound")
$args.Add("-nologo")
$args.Add("+set")
$args.Add("vid_preferbackend")
$args.Add("4")
$args.Add("+set")
$args.Add("nri_ptselftest")
$args.Add("true")
$args.Add("+set")
$args.Add("developer")
$args.Add("1")

if ($scenario.PSObject.Properties.Name.Contains("backend")) {
    $args.Add("+set")
    $args.Add("nri_api")
    $args.Add([string]$scenario.backend)
}
if ($scenario.PSObject.Properties.Name.Contains("debugMode")) {
    $args.Add("+set")
    $args.Add("nri_ptdebug")
    $args.Add([string]$scenario.debugMode)
}
if ($scenario.PSObject.Properties.Name.Contains("traceFrames")) {
    $args.Add("+set")
    $args.Add("nri_pttraceframes")
    $args.Add([string]$scenario.traceFrames)
}

$args.Add("+logfile")
$args.Add($LogPath.Replace('\', '/'))

if ($scenario.PSObject.Properties.Name.Contains("extraArgs")) {
    foreach ($extra in @($scenario.extraArgs)) {
        $args.Add([string]$extra)
    }
}

if ($scenario.PSObject.Properties.Name.Contains("commands")) {
    $args.Add([string]$scenario.commands)
}

function Get-NriScenarioRequiredPrefixes {
    param([object]$Scenario)

    $prefixes = @()
    if ($Scenario.PSObject.Properties.Name.Contains("requiredPrefixes")) {
        $prefixes = @($Scenario.requiredPrefixes)
    }
    else {
        $prefixes = @("NRI PT selftest:")
    }

    if ($Scenario.PSObject.Properties.Name.Contains("prefixAssertions")) {
        $prefixSet = [ordered]@{}
        foreach ($prefix in @($prefixes)) {
            if ($prefix) {
                $prefixSet[[string]$prefix] = $true
            }
        }
        foreach ($assertion in @($Scenario.prefixAssertions)) {
            if ($assertion.PSObject.Properties.Name.Contains("prefix")) {
                $prefixSet[[string]$assertion.prefix] = $true
            }
        }
        $prefixes = @($prefixSet.Keys)
    }

    return $prefixes
}

function Get-NriScenarioForbiddenPatterns {
    param([object]$Scenario)

    if ($Scenario.PSObject.Properties.Name.Contains("forbiddenPatterns")) {
        return @($Scenario.forbiddenPatterns)
    }

    return @("Device removed", "validation error", "failed to create", "assertion failed")
}

function Get-NriScenarioMinSelfTestFrames {
    param([object]$Scenario)

    if ($Scenario.PSObject.Properties.Name.Contains("minSelfTestFrames")) {
        return [int]$Scenario.minSelfTestFrames
    }

    return 1
}

function Stop-NriValidationProcess {
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

$processStart = [System.Diagnostics.ProcessStartInfo]::new()
$processStart.FileName = $resolvedRaze.Path
$processStart.UseShellExecute = $false
$processStart.CreateNoWindow = $true
$processStart.RedirectStandardOutput = $true
$processStart.RedirectStandardError = $true
$processStart.Arguments = Join-NriProcessArguments -Arguments $args.ToArray()

$process = [System.Diagnostics.Process]::Start($processStart)
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()

if ($CaptureWhenPassed) {
    Import-Module (Join-Path $PSScriptRoot "lib/NriValidationLog.psm1") -Force
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $requiredPrefixes = Get-NriScenarioRequiredPrefixes -Scenario $scenario
    $forbiddenPatterns = Get-NriScenarioForbiddenPatterns -Scenario $scenario
    $minSelfTestFrames = Get-NriScenarioMinSelfTestFrames -Scenario $scenario

    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $LogPath) -and ((Get-Item -LiteralPath $LogPath).Length -gt 0)) {
            try {
                $summary = Get-NriValidationLogSummary -Path $LogPath -RequiredPrefixes $requiredPrefixes -ForbiddenPatterns $forbiddenPatterns
                $result = Test-NriValidationSummary -Summary $summary -MinSelfTestFrames $minSelfTestFrames
                $loadingResult = [pscustomobject]@{
                    ok = $true
                    errors = @()
                }
                if ($scenario.PSObject.Properties.Name.Contains("loadingAssertions")) {
                    $loadingResult = Test-NriLoadingAssertions -Summary $summary -Assertions $scenario.loadingAssertions
                }
                $prefixResult = [pscustomobject]@{
                    ok = $true
                    errors = @()
                }
                if ($scenario.PSObject.Properties.Name.Contains("prefixAssertions")) {
                    $prefixResult = Test-NriPrefixAssertions -Summary $summary -Assertions $scenario.prefixAssertions
                }
                if ($result.ok -and $loadingResult.ok -and $prefixResult.ok) {
                    if ($SummaryOutput) {
                        $summaryDirectory = Split-Path -Parent $SummaryOutput
                        if ($summaryDirectory) {
                            New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null
                        }
                        $summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
                    }
                    Stop-NriValidationProcess -Process $process
                    Write-Host "NRI validation capture passed: accepted_selftest_frames=$($summary.acceptedSelfTestFrameCount) log=$LogPath summary=$SummaryOutput"
                    exit 0
                }
            }
            catch {
            }
        }

        if ($process.HasExited) {
            break
        }

        Start-Sleep -Seconds 1
    }

    if (-not $process.HasExited) {
        Stop-NriValidationProcess -Process $process
    }
}

if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-NriValidationProcess -Process $process
    throw "validation run timed out after $TimeoutSeconds seconds"
}

if ($process.ExitCode -ne 0 -and -not $CaptureWhenPassed) {
    throw "validation run exited with code $($process.ExitCode); log=$LogPath"
}

& (Join-Path $PSScriptRoot "assert-nri-log.ps1") -InputPath $LogPath -ScenarioPath $ScenarioPath -SummaryOutput $SummaryOutput
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "NRI validation run passed: log=$LogPath summary=$SummaryOutput"
