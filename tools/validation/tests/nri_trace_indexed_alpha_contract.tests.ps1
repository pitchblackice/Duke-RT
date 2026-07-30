$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$shaderPath = Join-Path $repoRoot `
	'source\common\rendering\nri\shaders\Include\RaytracingShared.hlsli'
$shader = Get-Content -LiteralPath $shaderPath -Raw

$functionStart = $shader.IndexOf(
	'bool IsTransparentSurfaceSample(uint materialIndex, uint dataSource, float2 uv)')
$functionEnd = $shader.IndexOf('uint GetPortalTraversalDepth()', $functionStart)
if ($functionStart -lt 0 -or $functionEnd -le $functionStart) {
	throw 'Could not isolate IsTransparentSurfaceSample.'
}
$body = $shader.Substring($functionStart, $functionEnd - $functionStart)

$indexedDeclaration = $body.IndexOf(
	'const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;')
$opaqueIndexedGuard = $body.IndexOf(
	'if (indexed && (material.flags & MATERIAL_FLAG_ALPHA_CLIP) == 0)')
$rawSample = $body.IndexOf(
	'const float4 rawSample = SampleMaterialBaseColorRaw(materialIndex, dataSource, uv);')
$indexedClipTest = $body.IndexOf('const uint paletteIndex =')
$trueColorAlphaTest = $body.IndexOf('return rawSample.a < 0.5;')

if ($indexedDeclaration -lt 0 -or $opaqueIndexedGuard -lt 0 -or
	$rawSample -lt 0 -or $indexedClipTest -lt 0 -or
	$trueColorAlphaTest -lt 0) {
	throw 'Indexed/true-color alpha-filter contracts are incomplete.'
}
if ($opaqueIndexedGuard -le $indexedDeclaration -or
	$rawSample -le $opaqueIndexedGuard) {
	throw 'Opaque indexed materials must return before the raw base-color sample.'
}
if ($indexedClipTest -le $rawSample -or
	$trueColorAlphaTest -le $indexedClipTest) {
	throw 'Alpha-clipped indexed and true-color tests must retain their sampled order.'
}

function Is-TransparentCpuMirror(
	[bool]$Indexed,
	[bool]$AlphaClip,
	[int]$PaletteIndex,
	[double]$Alpha)
{
	if ($Indexed -and -not $AlphaClip) { return $false }
	if ($Indexed) { return $PaletteIndex -eq 0 }
	return $Alpha -lt 0.5
}

if (Is-TransparentCpuMirror $true $false 0 0.0) {
	throw 'Ordinary indexed material must remain opaque even at palette index zero.'
}
if (-not (Is-TransparentCpuMirror $true $true 0 1.0)) {
	throw 'Alpha-clipped indexed material must keep palette index zero transparent.'
}
if (Is-TransparentCpuMirror $true $true 1 0.0) {
	throw 'Alpha-clipped indexed material must keep nonzero palette indices opaque.'
}
if (-not (Is-TransparentCpuMirror $false $false 255 0.49)) {
	throw 'True-color material must keep alpha below 0.5 transparent.'
}
if (Is-TransparentCpuMirror $false $false 0 0.5) {
	throw 'True-color material must keep alpha 0.5 opaque.'
}

Write-Output 'NRI indexed alpha-sampling contracts passed.'
