param(
    [string]$RazePath = "build/terminal-ninja/raze.exe",

    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",

    [string]$File = "M:\Raze\full-voxel-overlay",

    [string]$Map = "e1l1",

    [int]$Runs = 3,

    [int]$TraceFrames = 256,

    [int]$WaitTics = 180,

    [int]$TimeoutSeconds = 180,

    [switch]$Build,

    [switch]$SkipAnalyze,

    [string]$OutputDirectory,

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

function Stop-NriReproProcess {
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
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/explosion-repro/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$runResults = @()
for ($runIndex = 1; $runIndex -le $Runs; $runIndex++) {
    $logPath = Join-Path $OutputDirectory ("run-{0}.log" -f $runIndex)
    $commands = "+wait 45; map $Map; wait 1; closemenu; wait $WaitTics; quit"

    $args = New-Object System.Collections.Generic.List[string]
    if ($File) {
        $args.Add("-file")
        $args.Add($File)
    }
    if ($GameGrp) {
        $args.Add("-gamegrp")
        $args.Add($GameGrp)
    }
    $args.Add("-nosound")
    $args.Add("-nologo")
    $args.Add("+set")
    $args.Add("vid_preferbackend")
    $args.Add("4")
    $args.Add("+set")
    $args.Add("nri_api")
    $args.Add("d3d12")
    $args.Add("+set")
    $args.Add("nri_ptwaitpresent")
    $args.Add("false")
    $args.Add("+set")
    $args.Add("developer")
    $args.Add("1")
    $args.Add("+set")
    $args.Add("nri_pttraceframes")
    $args.Add([string]$TraceFrames)
    $args.Add("+set")
    $args.Add("nri_ptactorspritetrace")
    $args.Add("2")
    $args.Add("+set")
    $args.Add("nri_ptactoroverlaylighttrace")
    $args.Add("1")
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

    Write-Host "Explosion repro run ${runIndex}/${Runs}: log=$logPath"
    $process = [System.Diagnostics.Process]::Start($processStart)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $timedOut = $false
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        Stop-NriReproProcess -Process $process
    }
    $stdoutTask.Wait(5000) | Out-Null
    $stderrTask.Wait(5000) | Out-Null

    $runResults += [pscustomobject]@{
        run = $runIndex
        logPath = $logPath
        exitCode = if ($timedOut) { $null } else { $process.ExitCode }
        timedOut = $timedOut
        stdoutLength = if ($stdoutTask.IsCompleted) { $stdoutTask.Result.Length } else { 0 }
        stderrLength = if ($stderrTask.IsCompleted) { $stderrTask.Result.Length } else { 0 }
    }
}

$manifest = [pscustomobject]@{
    outputDirectory = $OutputDirectory
    map = $Map
    runs = $runResults
}
$manifestPath = Join-Path $OutputDirectory "manifest.json"
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if (-not $SkipAnalyze) {
    $analyzeArgs = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "analyze-explosion-repro.ps1"), "-LogDirectory", $OutputDirectory)
    if ($SummaryOutput) {
        $analyzeArgs += @("-SummaryOutput", $SummaryOutput)
    }
    $analyze = Start-Process -FilePath "powershell" -ArgumentList $analyzeArgs -PassThru -Wait -NoNewWindow
    if ($analyze.ExitCode -ne 0) {
        exit $analyze.ExitCode
    }
}

Write-Host "Explosion repro complete: directory=$OutputDirectory manifest=$manifestPath"
