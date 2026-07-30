Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$frameBuildPath = Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_frame_build.cpp'
$frameBuildSource = Get-Content -LiteralPath $frameBuildPath -Raw
$residentStaticPath = Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_runtime_mutation_resident_static.cpp'
$residentStaticSource = Get-Content -LiteralPath $residentStaticPath -Raw

$startMarker = 'if (paletteReady && texturesReady && buffersReady && accelerationReady)'
$endMarker = 'else if (mGpuSceneHasDynamicOverlay || residentStaticWorldGeometryChanged)'
$start = $frameBuildSource.IndexOf($startMarker, [StringComparison]::Ordinal)
$end = $frameBuildSource.IndexOf($endMarker, $start + $startMarker.Length, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -lt 0) {
	throw 'could not isolate the resident-static overlay fallback'
}
$fallback = $frameBuildSource.Substring($start, $end - $start)

foreach ($required in @(
	'PT runtime/dynamic overlay update failed; tracing the resident static world only.',
	'EnsurePaletteTexture(mStaticMapScene.materialBridge)',
	'EnsureSceneTextures(',
	'"static_map_scene"',
	'staticTextureStateRestored && RestoreStaticTopLevelScene();',
	'if (!staticSceneRestored)',
	'mGpuSceneHasDynamicOverlay = false;',
	'mUsedDynamicSceneLastFrame = false;',
	'mUsedStaticMapSceneLastFrame = true;',
	'texturesReady = true;',
	'accelerationReady = true;'
)) {
	if (-not $fallback.Contains($required)) {
		throw "overlay failure must restore a complete resident-static frame (missing '$required')"
	}
}

if ($fallback.Contains('if (mGpuSceneHasDynamicOverlay)')) {
	throw 'overlay failure must restore the resident static scene even when no dynamic overlay was previously committed'
}

$textureRestore = $fallback.IndexOf('const bool staticTextureStateRestored =', [StringComparison]::Ordinal)
$sceneRestore = $fallback.IndexOf('const bool staticSceneRestored =', $textureRestore, [StringComparison]::Ordinal)
$failureCheck = $fallback.IndexOf('if (!staticSceneRestored)', $sceneRestore, [StringComparison]::Ordinal)
$forcedReady = $fallback.IndexOf('paletteReady = true;', $failureCheck, [StringComparison]::Ordinal)
if ($textureRestore -lt 0 -or $sceneRestore -le $textureRestore -or
	$failureCheck -le $sceneRestore -or $forcedReady -le $failureCheck) {
	throw 'static textures and scene data must be restored and checked before fallback is marked ready'
}

$restoreStart = $residentStaticSource.IndexOf('bool NRIRenderer::RestoreStaticTopLevelScene()', [StringComparison]::Ordinal)
$restoreEnd = $residentStaticSource.IndexOf('bool NRIRenderer::RefreshResidentStaticSceneDataSet()', $restoreStart, [StringComparison]::Ordinal)
$refreshEnd = $residentStaticSource.IndexOf('bool NRIRenderer::RefreshResidentStaticMaterialSlices(', $restoreEnd, [StringComparison]::Ordinal)
if ($restoreStart -lt 0 -or $restoreEnd -lt 0 -or $refreshEnd -lt 0) {
	throw 'could not isolate resident-static scene-data publication helpers'
}
$staticPublication = $residentStaticSource.Substring($restoreStart, $refreshEnd - $restoreStart)
if ($staticPublication.Contains('GetCurrentDynamic')) {
	throw 'resident-static publication must not depend on a dynamic ring slot that may be uninitialized after a transition'
}
if (([regex]::Matches($staticPublication, [regex]::Escape('mStaticVertexBuffer'))).Count -lt 4 -or
	([regex]::Matches($staticPublication, [regex]::Escape('mStaticMaterialBuffer'))).Count -lt 4) {
	throw 'resident-static publication must supply static buffers for both descriptor domains'
}

Write-Host 'NRI resident-static overlay fallback restore tests passed.'
