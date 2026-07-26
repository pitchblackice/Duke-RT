param(
	[string]$ScenarioPath = 'tools/validation/perf-scenarios/current-direct-trace-large-voxel-d3d12.json',
	[string]$RazePath = 'build/terminal-ninja/raze.exe',
	[Parameter(Mandatory = $true)]
	[ValidateSet(0, 1, 3)]
	[int]$BlasPolicy,
	[ValidateSet('exact', 'forced-miss', 'age-one')]
	[string[]]$Modes = @('exact', 'forced-miss', 'age-one'),
	[ValidateRange(2, 3)]
	[int]$Cycles = 3,
	[int]$Samples = 256,
	[int]$WarmupSamples = 16,
	[int]$TimeoutSeconds = 900,
	[string]$OutputDirectory,
	[string]$SummaryOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Set-ScenarioCvar([object]$Scenario, [string]$Name, [string]$Value) {
	$args = @($Scenario.launch.extraArgs)
	for ($index = 0; $index + 2 -lt $args.Count; ++$index) {
		if ([string]$args[$index] -eq '+set' -and [string]$args[$index + 1] -eq $Name) {
			$args[$index + 2] = $Value
			$Scenario.launch.extraArgs = $args
			return
		}
	}
	$Scenario.launch.extraArgs = @($args) + @('+set', $Name, $Value)
}

function New-CacheScenario([object]$Base, [object]$Mode, [string]$Directory) {
	$scenario = $Base | ConvertTo-Json -Depth 24 | ConvertFrom-Json
	$loopFrames = $Samples + $WarmupSamples + 3
	$settings = [ordered]@{
		cl_interpolate = 'false'
		nri_ptlightbounces = '2'
		nri_ptmirrorbounces = '2'
		nri_ptportaldepth = '3'
		nri_ptemissivesamples = '1'
		nri_ptemissiveprimarybudget = '2'
		nri_ptvoxelomitoccurrences = 'false'
		nri_ptvoxelexcludeindex = '390'
		nri_ptvoxelblaspolicy = [string]$BlasPolicy
		nri_ptvoxelblascompact = 'false'
		nri_ptvoxelarenapresize = 'true'
		nri_ptindirectradiancecache = $Mode.cache.ToString().ToLowerInvariant()
		nri_ptvoxelcomputepreloadterminalcommand = 'set nri_ptloadingtrace 1'
		nri_ptvoxelcomputepreloadreleasecommand = "set nri_ptloadingtrace 0; perf_looptraceframes $loopFrames; perf_fixedsimulationframes $($Samples + $WarmupSamples); perf_compactwarmupframes $WarmupSamples; perf_compactframes $Samples"
	}
	foreach ($setting in $settings.GetEnumerator()) {
		Set-ScenarioCvar -Scenario $scenario -Name $setting.Key -Value ([string]$setting.Value)
	}
	# This CVar deliberately appears only in the age-one leg. Exact and
	# forced-miss remain runnable against the Slice 4.1 binary; age-one fails
	# closed until Slice 4.2 supplies the accept control and matching log mode.
	if ($Mode.accept) {
		Set-ScenarioCvar -Scenario $scenario -Name 'nri_ptindirectradiancecacheaccept' -Value 'true'
	}

	$scenario.name = "gpu-time-slice4-cache-$($Mode.name)-blas-$BlasPolicy-d3d12"
	$scenario.description = "Slice 4.1/4.2 fixed indirect-radiance cache matrix leg $($Mode.name), voxel BLAS policy $BlasPolicy."
	$scenario.capture.loopTraceFrames = $loopFrames
	$scenario.capture.runs = 1
	$scenario.capture.timeoutSeconds = $TimeoutSeconds
	$scenario.capture.stopWhenLoopTraceFramesCaptured = $false
	if (-not $scenario.capture.PSObject.Properties.Name.Contains('stopWhenPrefix')) {
		$scenario.capture | Add-Member -NotePropertyName stopWhenPrefix -NotePropertyValue 'PERF compact capture complete:'
		$scenario.capture | Add-Member -NotePropertyName stopWhenPrefixCount -NotePropertyValue 1
	}
	else {
		$scenario.capture.stopWhenPrefix = 'PERF compact capture complete:'
		$scenario.capture.stopWhenPrefixCount = 1
	}
	$scenario.requiredPrefixes = @($scenario.requiredPrefixes) + @('PERF pt indirect radiance cache NRI:')
	$scenario | Add-Member -Force -NotePropertyName indirectRadianceCacheMatrix -NotePropertyValue ([pscustomobject]@{
		mode = $Mode.name
		cacheRequested = [bool]$Mode.cache
		acceptRequested = [bool]$Mode.accept
		blasPolicy = $BlasPolicy
		samples = $Samples
		warmupSamples = $WarmupSamples
	})
	$path = Join-Path $Directory "cache-$($Mode.name).json"
	$scenario | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
	return [pscustomobject]@{ mode = $Mode.name; cache = [bool]$Mode.cache; accept = [bool]$Mode.accept; path = $path }
}

if ($Samples -lt 8 -or $Samples -gt 2048) { throw 'Samples must be in 8..2048.' }
if ($WarmupSamples -lt 1 -or $WarmupSamples -gt 2048 -or $Samples + $WarmupSamples -gt 4096) {
	throw 'WarmupSamples must be positive and fit the fixed-capture bounds.'
}

$selectedModeNames = @($Modes | ForEach-Object { ([string]$_).ToLowerInvariant() })
$uniqueModeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($modeName in $selectedModeNames) {
	if (-not $uniqueModeNames.Add($modeName)) { throw "Modes contains duplicate mode '$modeName'." }
}
foreach ($requiredMode in @('exact', 'forced-miss')) {
	if (-not $uniqueModeNames.Contains($requiredMode)) { throw "Modes must include '$requiredMode'." }
}
if ($Cycles -ne $selectedModeNames.Count) {
	throw "Cycles must equal the selected mode count ($($selectedModeNames.Count)) so every mode occupies every ordinal exactly once."
}

$base = Get-Content -LiteralPath (Resolve-Path -LiteralPath $ScenarioPath) -Raw | ConvertFrom-Json
if (-not $OutputDirectory) {
	$OutputDirectory = Join-Path (Get-Location) ('tools/logs/perf/gpu-time-slice4-cache-matrix/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not $SummaryOutput) { $SummaryOutput = Join-Path $resolvedOutput 'summary.json' }
$generatedDirectory = Join-Path $resolvedOutput 'generated-scenarios'
New-Item -ItemType Directory -Force -Path $generatedDirectory | Out-Null

$modeDefinitions = @(
	[pscustomobject]@{ name = 'exact'; cache = $false; accept = $false },
	[pscustomobject]@{ name = 'forced-miss'; cache = $true; accept = $false },
	[pscustomobject]@{ name = 'age-one'; cache = $true; accept = $true }
)
$selectedModes = @($selectedModeNames | ForEach-Object {
	$modeName = $_
	@($modeDefinitions | Where-Object name -eq $modeName)[0]
})
$legs = @($selectedModes | ForEach-Object { New-CacheScenario -Base $base -Mode $_ -Directory $generatedDirectory })
$runner = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'run-nri-perf.ps1')).Path
$checker = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'check-nri-fixed-simulation-log.ps1')).Path
$analyzer = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'analyze-nri-indirect-radiance-cache-matrix.ps1')).Path
$powershell = (Get-Process -Id $PID).Path
$entries = [Collections.Generic.List[object]]::new()
$sequence = 0

# One rotation per mode puts every selected mode once in each ordinal position.
for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
	$start = (1 - $cycle + $legs.Count) % $legs.Count
	for ($offset = 0; $offset -lt $legs.Count; ++$offset) {
		$leg = $legs[($start + $offset) % $legs.Count]
		++$sequence
		$legOutput = Join-Path $resolvedOutput ('{0:D2}-cache-{1}-cycle-{2}' -f $sequence, $leg.mode, $cycle)
		$baseSummary = Join-Path $legOutput 'base-summary.json'
		& $powershell -NoProfile -ExecutionPolicy Bypass -File $runner -ScenarioPath $leg.path -RazePath $RazePath -Runs 1 -TimeoutSeconds $TimeoutSeconds -OutputDirectory $legOutput -SummaryOutput $baseSummary
		if ($LASTEXITCODE -ne 0) { throw "Perf runner failed for sequence $sequence." }

		$logPath = Join-Path $legOutput 'run-1.log'
		$fixedSummary = Join-Path $legOutput 'fixed-summary.json'
		& $powershell -NoProfile -ExecutionPolicy Bypass -File $checker -LogPath $logPath -ExpectedSamples $Samples -RouteMode 0 -ExpectedVisibleChunkGate 1 -SkipSpatialPublicationCheck -RequireStrictFirstFrameRelease -RequireRecovery -AllowInitialHistoryReset -SummaryOutput $fixedSummary
		if ($LASTEXITCODE -ne 0) { throw "Fixed-capture validation failed for sequence $sequence." }

		$entries.Add([pscustomobject]@{
			sequence = $sequence
			cycle = $cycle
			ordinal = $offset + 1
			mode = $leg.mode
			cacheRequested = $leg.cache
			acceptRequested = $leg.accept
			blasPolicy = $BlasPolicy
			scenarioPath = $leg.path
			outputDirectory = $legOutput
			logPath = $logPath
			baseSummaryPath = $baseSummary
			fixedSummaryPath = $fixedSummary
		})
		[pscustomobject]@{
			schema = 1
			modes = $selectedModeNames
			modeCount = $legs.Count
			cycles = $Cycles
			samples = $Samples
			warmupSamples = $WarmupSamples
			blasPolicy = $BlasPolicy
			entries = $entries.ToArray()
		} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'matrix-manifest.json') -Encoding UTF8
	}
}

$manifestPath = Join-Path $resolvedOutput 'matrix-manifest.json'
& $powershell -NoProfile -ExecutionPolicy Bypass -File $analyzer -ManifestPath $manifestPath -SummaryOutput $SummaryOutput
exit $LASTEXITCODE
