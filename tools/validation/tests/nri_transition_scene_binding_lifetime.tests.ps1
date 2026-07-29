Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$sceneUpload = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_upload.cpp') -Raw
$upscaler = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_upscaler.cpp') -Raw
$frameBuild = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_frame_build.cpp') -Raw
$dispatch = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_pass_dispatch.cpp') -Raw

$snapshotStart = $sceneUpload.IndexOf('const auto acquireSceneDataDescriptorSnapshot', [StringComparison]::Ordinal)
$snapshotEnd = $sceneUpload.IndexOf('if (sceneInstances.empty())', $snapshotStart, [StringComparison]::Ordinal)
if ($snapshotStart -lt 0 -or $snapshotEnd -lt 0) {
	throw 'could not isolate scene-data snapshot acquisition'
}
$snapshot = $sceneUpload.Substring($snapshotStart, $snapshotEnd - $snapshotStart)
foreach ($required in @(
	'GetRecordingCommandFenceValue()',
	'if (recordingFenceValue == 0)',
	'IsCommandFenceValueComplete(snapshot.retireFenceValue)',
	'WaitForCommandsTracked("scene_data_snapshot_reuse")',
	'snapshot.retireFenceValue = recordingFenceValue;'
)) {
	if (-not $snapshot.Contains($required)) {
		throw "scene-data snapshots must use command-fence lifetime (missing '$required')"
	}
}
if ($snapshot.Contains('renderer.mFrameIndex + 1u') -or $snapshot.Contains('IsFrameFenceValueComplete')) {
	throw 'scene-data snapshot lifetime must not mix renderer frame identity with GPU fence identity'
}

$ensureStart = $upscaler.IndexOf('bool NRIUpscalerContext::EnsureUpscaler', [StringComparison]::Ordinal)
$ensureEnd = $upscaler.IndexOf('bool NRIUpscalerContext::EnsureMainUpscaler', $ensureStart, [StringComparison]::Ordinal)
if ($ensureStart -lt 0 -or $ensureEnd -lt 0) {
	throw 'could not isolate upscaler replacement'
}
$ensure = $upscaler.Substring($ensureStart, $ensureEnd - $ensureStart)
$waitIndex = $ensure.IndexOf('frameBuffer.WaitForCommands(true);', [StringComparison]::Ordinal)
$destroyIndex = $ensure.IndexOf('DestroyUpscaler(frameBuffer, slot.instance);', [StringComparison]::Ordinal)
if ($waitIndex -lt 0 -or $destroyIndex -le $waitIndex) {
	throw 'upscaler replacement must drain submitted consumers before destroying the provider instance'
}

foreach ($required in @(
	'PT current queued-frame scene bindings were incomplete; republishing the resident static world.',
	'"static_binding_repair"',
	'RestoreStaticTopLevelScene();',
	'if (!staticBindingsRestored || !currentTraceBindingsReady())',
	'PT current queued-frame scene binding repair failed; skipping TraceOpaque.'
)) {
	if (-not $frameBuild.Contains($required)) {
		throw "queued-frame scene binding repair contract is incomplete (missing '$required')"
	}
}

$traceStart = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchTraceOpaque', [StringComparison]::Ordinal)
$traceEnd = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchDenoiser', $traceStart, [StringComparison]::Ordinal)
if ($traceStart -lt 0 -or $traceEnd -lt 0) {
	throw 'could not isolate TraceOpaque dispatch'
}
$trace = $dispatch.Substring($traceStart, $traceEnd - $traceStart)
if (-not $trace.Contains('if (!context.mSceneBinding.BindSceneRootDescriptors())')) {
	throw 'TraceOpaque must not dispatch without a valid queued-frame TLAS root binding'
}

Write-Host 'NRI transition scene-binding lifetime tests passed.'
