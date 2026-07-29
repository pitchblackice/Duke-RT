param(
    [string]$RazePath = "build/terminal-ninja/raze.exe",
    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",
    [string]$File,
    [string]$Map = "e1l5",
    [ValidateRange(1, 20)][int]$Runs = 3,
    [ValidateRange(1, 3600)][int]$PostMapWarmupWaitTics = 600,
    [ValidateRange(1, 3600)][int]$InjectionSettleWaitTics = 120,
    [ValidateRange(1, 7200)][int]$LifecycleDrainWaitTics = 600,
    [ValidateRange(1, 3600)][int]$ReclaimGraceFrames = 120,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 240,
    [switch]$Build,
    [switch]$SkipAnalyze,
    [string]$OutputDirectory,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $File) {
    $File = Join-Path $repoRoot 'tools/validation/fixtures/smoke-lifecycle-no-continuous'
}
if (-not (Test-Path -LiteralPath $File -PathType Container)) {
    throw "smoke lifecycle overlay directory not found: $File"
}

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Argument)
    if ($Argument.Length -eq 0) { return '""' }
    if ($Argument -notmatch '[\s"]') { return $Argument }
    $escaped = $Argument -replace '(\\+)$', '$1$1'
    $escaped = $escaped.Replace('"', '\"')
    return '"' + $escaped + '"'
}

function Join-ProcessArguments {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    return (($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' ')
}

if ($Build) {
    $buildCommand = 'call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze'
    $build = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $buildCommand -PassThru -Wait -NoNewWindow
    if ($build.ExitCode -ne 0) { throw "build failed with exit code $($build.ExitCode)" }
}

$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('tools/logs/smoke-grid-lifecycle/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$commands = "+wait 45; map $Map; wait 1; closemenu; wait $PostMapWarmupWaitTics; " +
    "nri_ptsmokestatus; nri_ptsmoke_test; wait $InjectionSettleWaitTics; nri_ptsmokestatus; " +
    "wait $LifecycleDrainWaitTics; nri_ptsmokestatus; " +
    "nri_ptsmoke_test; wait $InjectionSettleWaitTics; nri_ptsmokestatus; " +
    "wait $LifecycleDrainWaitTics; nri_ptsmokestatus; quit"

$logPaths = [System.Collections.Generic.List[string]]::new()
for ($run = 1; $run -le $Runs; ++$run) {
    $logPath = Join-Path $OutputDirectory "run-$run.log"
    $logPaths.Add($logPath)
    $args = [System.Collections.Generic.List[string]]::new()
    foreach ($pair in @(@('-file', $File), @('-gamegrp', $GameGrp))) {
        if ($pair[1]) { $args.Add($pair[0]); $args.Add($pair[1]) }
    }
    foreach ($argument in @(
        '-nosound', '-nologo',
        '+set', 'vid_preferbackend', '4',
        '+set', 'nri_api', 'd3d12',
        '+set', 'nri_ptwaitpresent', 'false',
        '+set', 'developer', '1',
        '+set', 'nri_validation', 'true',
        '+set', 'nri_dred', 'true',
        '+set', 'nri_ptsmoke', 'true',
        '+set', 'nri_ptsmokerepresentation', '1',
        '+set', 'nri_ptsmokeemissivebackend', '0',
        '+set', 'nri_ptsmokemultiplescatter', 'false',
        '+set', 'nri_ptsmokeselfshadow', 'false',
        '+set', 'nri_ptsmokereadback', 'true',
        '+set', 'nri_ptsmokegridreclaimgrace', [string]$ReclaimGraceFrames,
        '+logfile', $logPath.Replace('\', '/'),
        $commands
    )) { $args.Add([string]$argument) }

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $resolvedRaze.Path
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = Join-ProcessArguments $args.ToArray()
    $process = [System.Diagnostics.Process]::Start($start)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        try { $process.Kill(); $process.WaitForExit(5000) | Out-Null } catch {}
    }
    $stdoutTask.Wait(5000) | Out-Null
    $stderrTask.Wait(5000) | Out-Null
    $manifest = [ordered]@{
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        run = $run
        logPath = $logPath
        exitCode = if ($timedOut) { $null } else { $process.ExitCode }
        timedOut = $timedOut
        map = $Map
        overlay = $File
        postMapWarmupWaitTics = $PostMapWarmupWaitTics
        injectionSettleWaitTics = $InjectionSettleWaitTics
        lifecycleDrainWaitTics = $LifecycleDrainWaitTics
        reclaimGraceFrames = $ReclaimGraceFrames
        stdout = $stdoutTask.Result
        stderr = $stderrTask.Result
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory "run-$run.manifest.json") -Encoding UTF8
    if ($timedOut -or $process.ExitCode -ne 0) {
        throw "smoke lifecycle run $run failed: timeout=$timedOut exit=$($process.ExitCode)"
    }
}

if (-not $SkipAnalyze) {
    if (-not $SummaryOutput) { $SummaryOutput = Join-Path $OutputDirectory 'summary.json' }
    & (Join-Path $PSScriptRoot 'analyze-smoke-grid-lifecycle-repro.ps1') -LogPath $logPaths.ToArray() -SummaryOutput $SummaryOutput
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Smoke grid lifecycle repro complete: $OutputDirectory"
