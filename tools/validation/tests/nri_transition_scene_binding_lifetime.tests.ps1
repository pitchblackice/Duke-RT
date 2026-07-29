Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$sceneUpload = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_upload.cpp') -Raw
$upscaler = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_upscaler.cpp') -Raw
$frameBuild = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_frame_build.cpp') -Raw
$dispatch = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_pass_dispatch.cpp') -Raw
$descriptorSets = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_descriptor_sets.cpp') -Raw
$renderer = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_renderer.cpp') -Raw
$persistentVoxels = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_persistent_voxels.cpp') -Raw

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
	'PT current queued-frame scene bindings became incomplete after scene construction; skipping TraceOpaque.',
	'if (!currentTraceBindingsReady())'
)) {
	if (-not $frameBuild.Contains($required)) {
		throw "queued-frame scene binding validation contract is incomplete (missing '$required')"
	}
}
$selectionStart = $frameBuild.IndexOf('const bool staticMapSceneReady', [StringComparison]::Ordinal)
$selectionEnd = $frameBuild.IndexOf('bool residentStaticWorldGeometryChanged', $selectionStart, [StringComparison]::Ordinal)
if ($selectionStart -lt 0 -or $selectionEnd -lt 0) {
	throw 'could not isolate static scene path selection'
}
$earlySelection = $frameBuild.Substring($selectionStart, $selectionEnd - $selectionStart)
if ($earlySelection.Contains('RestoreStaticTopLevelScene()') -or $earlySelection.Contains('UpdateSceneDataSet(')) {
	throw 'scene-data publication must remain at the normal post-assembly point where all dependent payloads are ready'
}

foreach ($required in @(
	'mActiveSceneDataSnapshot->descriptorsInitialized',
	'mActiveSceneDataSnapshot->publishedMapEpoch == currentMapEpoch',
	'mSceneDataDescriptorMapEpochs[queuedFrameIndex] == currentMapEpoch',
	'mSceneDataDescriptorBuildEpochs[queuedFrameIndex] == currentBuildEpoch'
)) {
	if (-not $descriptorSets.Contains($required)) {
		throw "scene-data publication must be owner- and epoch-aware (missing '$required')"
	}
}

foreach ($required in @(
	'DestroyWorldTlasFrameSlots();',
	'mActiveSceneDataSnapshot = nullptr;',
	'mSceneDataDescriptorMapEpochs.begin()',
	'mSceneDataDescriptorBuildEpochs.begin()'
)) {
	if (-not $renderer.Contains($required)) {
		throw "level unload must invalidate old scene/TLAS publication (missing '$required')"
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
$guardCount = ([regex]::Matches($dispatch, [regex]::Escape('if (!context.mSceneBinding.BindSceneRootDescriptors())'))).Count
if ($guardCount -lt 5) {
	throw "all scene-root consumers must fail closed (found $guardCount guarded dispatches)"
}

if (-not $frameBuild.Contains('mPersistentVoxels.IsIndirectOnlyActorTlasAppendEligible(')) {
	throw 'local-player reflection handoff must use exact resident-actor TLAS eligibility'
}
if (-not $frameBuild.Contains('mPersistentVoxels.HasTlasAppendEligibleActor(')) {
	throw 'persistent voxel render admission must require an actor that can append to the current TLAS'
}
if ($frameBuild.Contains('persistentVoxelRenderable = hasPersistentVoxelBatch ? mPersistentVoxels.HasRenderableOverlay()')) {
	throw 'persistent voxel render admission must not rely on CPU batch metadata alone'
}
$eligibilityStart = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::IsActorTlasAppendEligible(', [StringComparison]::Ordinal)
$eligibilityEnd = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::HasPreloadPending()', $eligibilityStart, [StringComparison]::Ordinal)
if ($eligibilityStart -lt 0 -or $eligibilityEnd -lt 0) {
	throw 'could not isolate persistent voxel TLAS append eligibility'
}
$eligibility = $persistentVoxels.Substring($eligibilityStart, $eligibilityEnd - $eligibilityStart)
foreach ($required in @(
	'PersistentVoxelMaterialRangeMatches(actor, material)',
	'(mesh.tlasPublished || mesh.tlasReadyFrame <= frameIndex)',
	'services.GetAccelerationStructureHandle(mesh.accelerationStructure) != 0',
	'vertexBuffer.shaderView == nullptr',
	'primitiveRangeValid',
	'meshRangeMatches'
)) {
	if (-not $eligibility.Contains($required)) {
		throw "persistent voxel render admission is missing a TLAS append gate ('$required')"
	}
}

Write-Host 'NRI transition scene-binding lifetime tests passed.'
