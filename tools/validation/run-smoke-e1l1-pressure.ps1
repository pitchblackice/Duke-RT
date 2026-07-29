param(
    [string]$RazePath = "build/terminal-ninja/raze.exe",
    [string]$GameGrp = "C:\Program Files (x86)\Steam\steamapps\common\Duke Nukem 3D Twentieth Anniversary World Tour\DUKE3D.GRP",
    [string]$File = "M:\Raze\full-voxel-overlay",
    [ValidateSet("forward", "reverse")]
    [string[]]$Direction = @("forward", "reverse"),
    [int]$Runs = 1,
    [int]$SettleTics = 300,
    [int]$VisitTics = 300,
    [int]$TimeoutSeconds = 300,
    [string[]]$AdditionalArgs = @(),
    [switch]$Build,
    [switch]$SkipAnalyze,
    [string]$OutputDirectory,
    [string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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
    return (($Arguments | ForEach-Object { ConvertTo-ProcessArgument -Argument $_ }) -join ' ')
}

function Stop-ReproProcess {
    param([System.Diagnostics.Process]$Process)
    if ($null -eq $Process -or $Process.HasExited) { return }
    try {
        $Process.Kill()
        $Process.WaitForExit(5000) | Out-Null
    }
    catch { }
}

function Get-VisitCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Warp,
        [Parameter(Mandatory = $true)][string]$RunDirection,
        [Parameter(Mandatory = $true)][int]$WaitTics
    )

    return "echo NRI_SMOKE_PRESSURE checkpoint=$Name direction=$RunDirection; $Warp; wait 2; +Move_Forward; wait 2; -Move_Forward; wait $WaitTics; nri_ptsmokestatus"
}

if ($Build) {
    $buildCommand = 'call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze'
    $buildProcess = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $buildCommand -PassThru -Wait -NoNewWindow
    if ($buildProcess.ExitCode -ne 0) { throw "build failed with exit code $($buildProcess.ExitCode)" }
}

$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path (Get-Location) "tools/logs/smoke-e1l1-pressure/$timestamp"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

# These cameras cover the two adjacent window sources, both rooftop sources,
# and DukeFire actor 405 in the open courtyard. A two-tic movement relinks the
# player after warptocoords, whose implementation changes position only.
$windowWarp = "warptocoords 560 3200 -180 180 0"
$roofWarp = "warptocoords -1616 890 -580 107 7"
$fireWarp = "warptocoords 1040 3432 -6 0 0"

$runResults = @()
foreach ($runDirection in $Direction) {
    for ($runIndex = 1; $runIndex -le $Runs; $runIndex++) {
        $logPath = Join-Path $OutputDirectory ("{0}-run-{1}.log" -f $runDirection, $runIndex)
        $visits = if ($runDirection -eq "forward") {
            @(
                Get-VisitCommand -Name "windows" -Warp $windowWarp -RunDirection $runDirection -WaitTics $VisitTics
                Get-VisitCommand -Name "roof" -Warp $roofWarp -RunDirection $runDirection -WaitTics $VisitTics
                Get-VisitCommand -Name "fire" -Warp $fireWarp -RunDirection $runDirection -WaitTics $VisitTics
            )
        }
        else {
            @(
                Get-VisitCommand -Name "fire-first" -Warp $fireWarp -RunDirection $runDirection -WaitTics $VisitTics
                Get-VisitCommand -Name "roof" -Warp $roofWarp -RunDirection $runDirection -WaitTics $VisitTics
                Get-VisitCommand -Name "windows" -Warp $windowWarp -RunDirection $runDirection -WaitTics $VisitTics
                Get-VisitCommand -Name "fire-return" -Warp $fireWarp -RunDirection $runDirection -WaitTics $VisitTics
            )
        }
        $commands = "+wait 45; map e1l1; wait 1; closemenu; wait $SettleTics; echo NRI_SMOKE_PRESSURE checkpoint=settled direction=$runDirection; nri_ptsmokestatus; " +
            ($visits -join '; ') + "; echo NRI_SMOKE_PRESSURE checkpoint=complete direction=$runDirection; nri_ptsmokestatus; quit"

        $args = [System.Collections.Generic.List[string]]::new()
        if ($File) { $args.Add("-file"); $args.Add($File) }
        if ($GameGrp) { $args.Add("-gamegrp"); $args.Add($GameGrp) }
        foreach ($argument in @(
            "-nosound", "-nologo",
            "+set", "vid_preferbackend", "4",
            "+set", "nri_api", "d3d12",
            "+set", "nri_ptwaitpresent", "false",
            "+set", "developer", "1",
            "+set", "nri_ptsmoke", "true",
            "+set", "nri_ptsmokerepresentation", "1",
            "+set", "nri_ptsmokeworkprofile", "0",
            "+set", "nri_ptsmokegridbricks", "512",
            "+set", "nri_ptsmokegridcellsize", "8",
            "+set", "nri_ptsmoketrace", "2",
            "+set", "nri_ptsmokereadback", "true",
            "+set", "nri_pttraceframes", "0"
        )) { $args.Add($argument) }
        foreach ($argument in $AdditionalArgs) { $args.Add([string]$argument) }
        $args.Add("+logfile")
        $args.Add($logPath.Replace('\', '/'))
        $args.Add($commands)

        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $resolvedRaze.Path
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Arguments = Join-ProcessArguments -Arguments $args.ToArray()

        Write-Host "E1L1 smoke pressure $runDirection run ${runIndex}/${Runs}: log=$logPath"
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) { Stop-ReproProcess -Process $process }
        $stdoutTask.Wait(5000) | Out-Null
        $stderrTask.Wait(5000) | Out-Null

        $runResults += [pscustomobject]@{
            direction = $runDirection
            run = $runIndex
            logPath = $logPath
            exitCode = if ($timedOut) { $null } else { $process.ExitCode }
            timedOut = $timedOut
        }
    }
}

$manifest = [pscustomobject]@{
    fixture = "e1l1-default-512-forward-reverse"
    gridBrickCapacity = 512
    gridCellSize = 8
    settleTics = $SettleTics
    visitTics = $VisitTics
    syntheticTraversal = $true
    limitations = @(
        "warptocoords changes player coordinates without explicitly relinking the sector; the two-tic movement is a best-effort relink, not equivalent to walking the user's route",
        "the open-courtyard authority target is E1L1 actor 405 at (1200,3432,-5.75); if actor-authority never reports appearance_seen=1, repeat with manual traversal before changing residency policy",
        "the four map-source rows prove source activity but synthetic visibility order is not a substitute for the user's portal/door traversal"
    )
    waypoints = [ordered]@{ windows = $windowWarp; roof = $roofWarp; fire = $fireWarp }
    runs = $runResults
}
$manifestPath = Join-Path $OutputDirectory "manifest.json"
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if (-not $SkipAnalyze) {
    $analyzeArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "analyze-smoke-e1l1-pressure.ps1"), "-LogDirectory", $OutputDirectory)
    if ($SummaryOutput) { $analyzeArgs += @("-SummaryOutput", $SummaryOutput) }
    & powershell.exe @analyzeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "E1L1 smoke pressure fixture complete: directory=$OutputDirectory manifest=$manifestPath"
