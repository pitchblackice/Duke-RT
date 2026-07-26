Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}

function Write-Utf8([string]$Path, [string]$Text) {
	$Text | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-AnalyzerForExitCode([string]$PowerShellPath, [string]$AnalyzerPath, [string]$ManifestPath, [string]$SummaryPath) {
	$priorPreference = $ErrorActionPreference
	try {
		$ErrorActionPreference = 'Continue'
		& $PowerShellPath -NoProfile -ExecutionPolicy Bypass -File $AnalyzerPath -ManifestPath $ManifestPath -SummaryOutput $SummaryPath *> $null
		return $LASTEXITCODE
	}
	finally {
		$ErrorActionPreference = $priorPreference
	}
}

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$runnerPath = Join-Path $repo 'tools\validation\run-nri-indirect-radiance-cache-matrix.ps1'
$analyzerPath = Join-Path $repo 'tools\validation\analyze-nri-indirect-radiance-cache-matrix.ps1'
$runner = Get-Content -LiteralPath $runnerPath -Raw
$analyzer = Get-Content -LiteralPath $analyzerPath -Raw

Assert-True ($runner -match '\[Parameter\(Mandatory = \$true\)\][\s\S]*?\[ValidateSet\(0, 1, 3\)\][\s\S]*?\[int\]\$BlasPolicy') 'runner must require an explicit supported voxel BLAS policy'
Assert-True ($runner -match "\[ValidateSet\('exact', 'forced-miss', 'age-one'\)\][\s\S]*?\[string\[\]\]\`$Modes = @\('exact', 'forced-miss', 'age-one'\)") 'runner must expose the three supported modes with the 3-mode default'
Assert-True ($runner -match '\[ValidateRange\(2, 3\)\][\s\S]*?\[int\]\$Cycles = 3') 'runner must expose a validated three-cycle default'
Assert-True ($runner -match '\[int\]\$Samples = 256[\s\S]*?\[int\]\$WarmupSamples = 16') 'runner must default to 16 warmup and 256 measured samples'
foreach ($contract in @(
	"cl_interpolate = 'false'",
	"nri_ptlightbounces = '2'",
	"nri_ptmirrorbounces = '2'",
	"nri_ptportaldepth = '3'",
	"nri_ptemissivesamples = '1'",
	"nri_ptvoxelexcludeindex = '390'",
	'perf_fixedsimulationframes \$\(\$Samples \+ \$WarmupSamples\)',
	'perf_compactwarmupframes \$WarmupSamples; perf_compactframes \$Samples',
	"stopWhenPrefix -NotePropertyValue 'PERF compact capture complete:'",
	'-RequireStrictFirstFrameRelease -RequireRecovery -AllowInitialHistoryReset'
)) {
	Assert-True ($runner -match $contract) "runner is missing fixed matrix contract: $contract"
}
Assert-True ($runner -match 'name = ''exact''; cache = \$false; accept = \$false[\s\S]*?name = ''forced-miss''; cache = \$true; accept = \$false[\s\S]*?name = ''age-one''; cache = \$true; accept = \$true') 'runner must define exact, forced-miss, and age-one legs'
Assert-True ($runner -match 'if \(\$Mode\.accept\)[\s\S]*?nri_ptindirectradiancecacheaccept') 'age-one must be the only leg that requests the future accept CVar'
Assert-True ($runner -match '\$Cycles -ne \$selectedModeNames\.Count[\s\S]*?for \(\$cycle = 1; \$cycle -le \$Cycles; \+\+\$cycle\)[\s\S]*?\$start = \(1 - \$cycle \+ \$legs\.Count\) % \$legs\.Count') 'runner must validate and use a balanced selected-mode rotation'
Assert-True ($runner -match 'modes = \$selectedModeNames[\s\S]*?modeCount = \$legs\.Count[\s\S]*?cycles = \$Cycles') 'runner manifest must declare its selected modes and dimensions'
Assert-True ($analyzer -match 'baselineFrame = \$FirstNriFrame - 1[\s\S]*?endKey = \[string\]\$LastNriFrame') 'analyzer must join cumulative telemetry to exact compact boundaries'
Assert-True ($analyzer -match "mode -eq 'forced-miss'[\s\S]*?delta\.accepted[\s\S]*?delta\.exactFallback[\s\S]*?delta\.lookups") 'analyzer must reject forced-miss acceptance/fallback violations'
Assert-True ($analyzer -match '\$cycles -ne \$modeCount[\s\S]*?\$expectedRunCount = \$cycles \* \$modeCount') 'analyzer must derive balanced matrix dimensions from the manifest'

function New-SyntheticMatrix(
	[string]$Root,
	[string]$Name,
	[string[]]$Modes,
	[int]$Cycles
) {
	$matrixRoot = Join-Path $Root $Name
	New-Item -ItemType Directory -Force -Path $matrixRoot | Out-Null
	$entries = [Collections.Generic.List[object]]::new()
	$sequence = 0
	for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
		$cycleIndex = $cycle - 1
		$start = (1 - $cycle + $Modes.Count) % $Modes.Count
		for ($ordinalIndex = 0; $ordinalIndex -lt $Modes.Count; ++$ordinalIndex) {
			++$sequence
			$mode = $Modes[($start + $ordinalIndex) % $Modes.Count]
			$directory = Join-Path $matrixRoot ("run-$sequence")
			New-Item -ItemType Directory -Force -Path $directory | Out-Null
			$fixedPath = Join-Path $directory 'fixed.json'
			$logPath = Join-Path $directory 'run.log'
			[pscustomobject]@{
				ok = $true
				samples = 2
				strictFirstFrameRelease = [pscustomobject]@{
					manifestHash = '0xbeef'
					selectedBindings = 4
					activeInstances = 8
					batchReadyActors = 8
				}
			} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fixedPath -Encoding UTF8

			$flags = switch ($mode) { 'exact' { 5 }; 'forced-miss' { 0x2005 }; 'age-one' { 0x6005 } }
			$settingsKey = switch ($mode) { 'exact' { 100 }; 'forced-miss' { 200 }; 'age-one' { 300 } }
			$segment = switch ($mode) { 'exact' { 10.0 + $cycleIndex }; 'forced-miss' { 9.0 + $cycleIndex }; 'age-one' { 8.0 + $cycleIndex } }
			$lines = [Collections.Generic.List[string]]::new()
			$lines.Add('PERF pt voxel blas policy NRI: policy=1 compact=0 strict=1 flags=0')
			foreach ($sample in 0..1) {
				$nriFrame = 100 + $sample
				$lines.Add("PERF pt trace workload NRI: frame=$nriFrame nri_frame=$nriFrame renderer_frame=$nriFrame schema=3 settings_key=$settingsKey workload_key=$($settingsKey + 1) render_w=1920 render_h=1080 output_w=1920 output_h=1080 dispatch_x=240 dispatch_y=135 dispatch_z=1 light_bounces=2 mirror_bounces=2 portal_depth=3 emissive_samples=1 emissive_requested=1 emissive_budget=2 indirect_requested=1 indirect_effective=1 indirect_active=1 hit_recon=1 runtime_lights=2 light_tiles_x=30 light_tiles_y=17 light_tile_size=64 light_tile_indices=40 light_tile_max=2 emissive_prims=3 emissive_power=4.000 voxel_occurrences=8 voxel_instance_prims=1000 voxel_occurrence_control=0 flags=$flags debug=0 bootstrap=0 upscaler=0 upscaler_mode=0 denoiser=1 direct_scene=0 directional=1 directional_shadow=1 split_shadow=0 fast_emissive_shadow=1 visible_chunk_gate=1 compact=1 epoch=1 sample=$sample")
				$lines.Add("PERF pt gpu timing NRI: frame=$nriFrame nri_frame=$nriFrame segment=$($segment + $sample) scene=7.000 trace=6.000 trace_dispatch=$($segment - 1 + $sample) denoise=1.000 compose=1.000 upscale=0.000 final=0.000 segments=1 invalid=0 dropped=0 resolved=1 expected=1 compact=1 epoch=1 sample=$sample")
			}

			if ($mode -eq 'exact') {
				$lines.Add('PERF pt indirect radiance cache NRI: frame=98 requested=0 mode=exact-miss valid=0 telemetry_frame=0 lookups=0 accepted=0 forced_miss=0 collision=0 stale=0 unsupported=0 exact_fallback=0 occupancy=0 updates=0 clears=0 table_bytes=0 total_bytes=0 invalidation=0x0 pending_readbacks=0')
			}
			else {
				$loggedMode = if ($mode -eq 'forced-miss') { 'exact-miss' } else { 'age-one' }
				if ($mode -eq 'forced-miss') {
					$baseline = 'lookups=100 accepted=0 forced_miss=100 collision=2 stale=3 unsupported=4 exact_fallback=100 occupancy=10 updates=50 clears=1'
					$endpoint = 'lookups=120 accepted=0 forced_miss=120 collision=3 stale=4 unsupported=5 exact_fallback=120 occupancy=12 updates=60 clears=1'
				}
				else {
					$baseline = 'lookups=100 accepted=10 forced_miss=0 collision=2 stale=3 unsupported=4 exact_fallback=90 occupancy=10 updates=50 clears=1'
					$endpoint = 'lookups=120 accepted=15 forced_miss=0 collision=3 stale=4 unsupported=5 exact_fallback=105 occupancy=12 updates=60 clears=1'
				}
				$lines.Add("PERF pt indirect radiance cache NRI: frame=102 requested=1 mode=$loggedMode valid=1 telemetry_frame=99 $baseline table_bytes=25165824 total_bytes=37748992 invalidation=0x1 pending_readbacks=2")
				$lines.Add("PERF pt indirect radiance cache NRI: frame=103 requested=1 mode=$loggedMode valid=1 telemetry_frame=99 $baseline table_bytes=25165824 total_bytes=37748992 invalidation=0x1 pending_readbacks=2")
				$lines.Add("PERF pt indirect radiance cache NRI: frame=104 requested=1 mode=$loggedMode valid=1 telemetry_frame=101 $endpoint table_bytes=25165824 total_bytes=37748992 invalidation=0x1 pending_readbacks=1")
			}
			Write-Utf8 -Path $logPath -Text ($lines -join [Environment]::NewLine)
			$entries.Add([pscustomobject]@{
				sequence = $sequence
				cycle = $cycle
				ordinal = $ordinalIndex + 1
				mode = $mode
				cacheRequested = $mode -ne 'exact'
				acceptRequested = $mode -eq 'age-one'
				blasPolicy = 1
				logPath = $logPath
				fixedSummaryPath = $fixedPath
			})
		}
	}

	$manifestPath = Join-Path $matrixRoot 'manifest.json'
	$summaryPath = Join-Path $matrixRoot 'summary.json'
	[pscustomobject]@{
		schema = 1
		modes = $Modes
		modeCount = $Modes.Count
		cycles = $Cycles
		samples = 2
		warmupSamples = 1
		blasPolicy = 1
		entries = $entries.ToArray()
	} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
	return [pscustomobject]@{
		manifestPath = $manifestPath
		summaryPath = $summaryPath
		entries = $entries.ToArray()
	}
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('raze-cache-matrix-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
try {
	$powershell = (Get-Process -Id $PID).Path
	$matrix2 = New-SyntheticMatrix -Root $tempRoot -Name 'matrix-2x2' -Modes @('exact', 'forced-miss') -Cycles 2
	$matrix3 = New-SyntheticMatrix -Root $tempRoot -Name 'matrix-3x3' -Modes @('exact', 'forced-miss', 'age-one') -Cycles 3
	foreach ($matrix in @($matrix2, $matrix3)) {
		& $powershell -NoProfile -ExecutionPolicy Bypass -File $analyzerPath -ManifestPath $matrix.manifestPath -SummaryOutput $matrix.summaryPath | Out-Null
		Assert-True ($LASTEXITCODE -eq 0) "valid synthetic cache matrix '$($matrix.manifestPath)' was rejected"
	}

	$summary2 = Get-Content -LiteralPath $matrix2.summaryPath -Raw | ConvertFrom-Json
	Assert-True ([bool]$summary2.ok -and [int]$summary2.cycles -eq 2 -and [int]$summary2.modeCount -eq 2 -and @($summary2.runs).Count -eq 4 -and @($summary2.legs).Count -eq 2) 'valid 2x2 cache summary shape is wrong'
	Assert-True ((@($summary2.modes) -join ',') -eq 'exact,forced-miss') '2x2 cache summary lost manifest mode order'
	$forced2 = @($summary2.legs | Where-Object mode -eq 'forced-miss')[0]
	Assert-True ([uint64]$forced2.cache.totals.lookups -eq 40 -and [uint64]$forced2.cache.totals.exactFallback -eq 40 -and [uint64]$forced2.cache.totals.accepted -eq 0) '2x2 forced-miss cumulative deltas are wrong'

	$summary3 = Get-Content -LiteralPath $matrix3.summaryPath -Raw | ConvertFrom-Json
	Assert-True ([bool]$summary3.ok -and [int]$summary3.cycles -eq 3 -and [int]$summary3.modeCount -eq 3 -and @($summary3.runs).Count -eq 9 -and @($summary3.legs).Count -eq 3) 'valid 3x3 cache summary shape is wrong'
	Assert-True ((@($summary3.modes) -join ',') -eq 'exact,forced-miss,age-one') '3x3 cache summary lost manifest mode order'
	$forced = @($summary3.legs | Where-Object mode -eq 'forced-miss')[0]
	$ageOne = @($summary3.legs | Where-Object mode -eq 'age-one')[0]
	Assert-True ([uint64]$forced.cache.totals.lookups -eq 60 -and [uint64]$forced.cache.totals.exactFallback -eq 60 -and [uint64]$forced.cache.totals.accepted -eq 0) 'forced-miss cumulative deltas are wrong'
	Assert-True ([uint64]$ageOne.cache.totals.accepted -eq 15 -and [double]$ageOne.cache.acceptRate -eq 25.0) 'age-one cache acceptance metrics are wrong'
	Assert-True ([double]$forced.completeGpu.p99 -eq 12.0 -and [double]$ageOne.traceDispatch.max -eq 10.0) 'GPU timing aggregation is wrong'

	$mismatchedManifestPath = Join-Path $tempRoot 'mismatched-cycles.json'
	$mismatchedManifest = Get-Content -LiteralPath $matrix2.manifestPath -Raw | ConvertFrom-Json
	$mismatchedManifest.cycles = 3
	$mismatchedManifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $mismatchedManifestPath -Encoding UTF8
	Assert-True ((Invoke-AnalyzerForExitCode $powershell $analyzerPath $mismatchedManifestPath $matrix2.summaryPath) -ne 0) 'unbalanced mode/cycle dimensions were not rejected'

	$forcedLog = [string](@($matrix2.entries | Where-Object mode -eq 'forced-miss')[0].logPath)
	$original = Get-Content -LiteralPath $forcedLog -Raw
	Write-Utf8 -Path $forcedLog -Text ($original.Replace('lookups=120 accepted=0 forced_miss=120', 'lookups=120 accepted=1 forced_miss=120'))
	Assert-True ((Invoke-AnalyzerForExitCode $powershell $analyzerPath $matrix2.manifestPath $matrix2.summaryPath) -ne 0) 'forced-miss accepted-hit violation was not rejected'
	Write-Utf8 -Path $forcedLog -Text $original

	Write-Utf8 -Path $forcedLog -Text ($original.Replace('telemetry_frame=101', 'telemetry_frame=102'))
	Assert-True ((Invoke-AnalyzerForExitCode $powershell $analyzerPath $matrix2.manifestPath $matrix2.summaryPath) -ne 0) 'missing exact cache endpoint was not rejected'
	Write-Utf8 -Path $forcedLog -Text $original

	$ageLog = [string](@($matrix3.entries | Where-Object mode -eq 'age-one')[0].logPath)
	$originalAge = Get-Content -LiteralPath $ageLog -Raw
	Write-Utf8 -Path $ageLog -Text ($originalAge.Replace('runtime_lights=2 light_tiles_x=', 'runtime_lights=3 light_tiles_x='))
	Assert-True ((Invoke-AnalyzerForExitCode $powershell $analyzerPath $matrix3.manifestPath $matrix3.summaryPath) -ne 0) 'cross-leg content identity drift was not rejected'
	Write-Utf8 -Path $ageLog -Text $originalAge

	Write-Utf8 -Path $forcedLog -Text ($original.Replace('invalid=0 dropped=0', 'invalid=1 dropped=0'))
	Assert-True ((Invoke-AnalyzerForExitCode $powershell $analyzerPath $matrix2.manifestPath $matrix2.summaryPath) -ne 0) 'invalid GPU sample was not rejected'
}
finally {
	$resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
	$systemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
	if ($resolvedTemp.StartsWith($systemTemp, [StringComparison]::OrdinalIgnoreCase)) {
		Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
	}
}

Write-Host 'NRI indirect-radiance cache matrix tests passed'
