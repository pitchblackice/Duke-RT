param(
	[string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$rendererDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_trace_stats_readback_policy.tests.cpp'
$outputDir = if ($OutputDirectory) { $OutputDirectory } else {
	Join-Path $repoRoot 'build\planner-tests\trace-stats-readback'
}
$testObject = Join-Path $outputDir 'nri_trace_stats_readback_policy.tests.obj'
$testExe = Join-Path $outputDir 'nri_trace_stats_readback_policy.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /c /std:c++17 /EHsc /W4 /WX /permissive- /MT /I"{1}" /Fo"{3}" "{2}"' -f `
	$vsDevCmd, $rendererDir, $testSource, $testObject
cmd /c $compile
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy compilation failed with exit code $LASTEXITCODE."
}

$link = 'call "{0}" -arch=x64 -host_arch=x64 >nul && link /nologo /out:"{1}" "{2}"' -f `
	$vsDevCmd, $testExe, $testObject
cmd /c $link
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy link failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy tests failed with exit code $LASTEXITCODE."
}

$traceStats = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_trace_stats.cpp') -Raw
$traceStatsHeader = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_trace_stats.h') -Raw
$passDispatch = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_pass_dispatch.cpp') -Raw
$renderDevice = Get-Content -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\system\nri_renderdevice.cpp') -Raw

if ($traceStats -match '\bWaitForCommands\s*\(') {
	throw 'Trace shader stats must not introduce a host wait.'
}
if ($traceStatsHeader -notmatch 'NRI_TRACE_SHADER_READBACK_SLOT_COUNT' -or
	$traceStats -notmatch 'IsCommandFenceValueComplete' -or
	$traceStats -notmatch 'IsCommandFenceValueAbandoned') {
	throw 'Trace shader stats must use fence-retired readback slots with explicit abandoned-work handling.'
}
if ($traceStats -notmatch 'SelectNRITraceShaderReadback[\s\S]*MapBuffer' -or
	$traceStats -notmatch 'readbacksSuperseded' -or
	$traceStats -notmatch 'copiesDroppedBusy') {
	throw 'Trace shader stats must publish the newest ready slot and expose observer backpressure.'
}
if ($traceStats -notmatch 'NRITraceShaderInstanceAttribution' -or
	$traceStats -notmatch 'attributionBytesCopied') {
	throw 'Trace shader stats must preserve frame-correct compact attribution and expose its CPU copy volume.'
}
if ($passDispatch -notmatch 'traceOpaqueReadbackMs[\s\S]*?\.Readback\(' -or
	$passDispatch -notmatch 'traceOpaqueStatsCopyMs[\s\S]*?\.CopyForReadback\(') {
	throw 'Trace-stat observer work must remain separately timed at its readback and copy call sites.'
}
if ($passDispatch -notmatch 'GetRecordingCommandFenceValue' -or
	$passDispatch -notmatch 'IsCommandFenceValueComplete' -or
	$passDispatch -notmatch 'IsCommandFenceValueAbandoned') {
	throw 'The pass boundary must provide explicit command-fence ownership services.'
}
if ($traceStats -notmatch 'consumerFence\s*!=\s*mReadbackConsumerFence[\s\S]*?outStats\.valid\s*=\s*false' -or
	$renderDevice -notmatch 'PERF pt shader stats observer NRI:[^\n]*copies=%llu[^\n]*recorded=%llu[^\n]*busy=%llu[^\n]*no_fence=%llu[^\n]*published=%llu[^\n]*superseded=%llu[^\n]*abandoned=%llu[^\n]*map_fail=%llu[^\n]*pending=%u[^\n]*attribution_rows=%llu[^\n]*attribution_bytes=%llu') {
	throw 'Published snapshots must be one-consumer-frame events and observer cost/drop counters must be logged.'
}

Write-Output 'NRI trace-stat nonblocking readback contract tests passed.'
