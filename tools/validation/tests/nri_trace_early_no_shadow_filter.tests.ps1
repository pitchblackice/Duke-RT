$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$shaderPath = Join-Path $repoRoot `
	'source\common\rendering\nri\shaders\Include\RaytracingShared.hlsli'
$shader = Get-Content -LiteralPath $shaderPath -Raw

$coreStart = $shader.IndexOf(
	'bool TraceClosestSurface(float3 startOrigin, float3 direction, float maxDistance, uint traceMask')
$coreEnd = $shader.IndexOf(
	'bool TraceScenePath(float3 startOrigin, float3 startDirection, float maxDistance, uint traceMask',
	$coreStart)
if ($coreStart -lt 0 -or $coreEnd -le $coreStart) {
	throw 'Could not isolate the shared closest-hit filter core.'
}
$core = $shader.Substring($coreStart, $coreEnd - $coreStart)

$oneWay = $core.IndexOf('if (ShouldIgnoreOneWayHit(')
$noShadow = $core.IndexOf(
	'if (ignoreNoShadowCast && !MaterialCastsShadow(GetMaterialData(materialIndex, instanceData.dataSource)))')
$barycentrics = $core.IndexOf(
	'const float2 bary = rayQuery.CommittedTriangleBarycentrics();')
$alpha = $core.IndexOf(
	'if (IsTransparentSurfaceSample(materialIndex, instanceData.dataSource, uv))')
if ($oneWay -lt 0 -or $noShadow -le $oneWay -or
	$barycentrics -le $noShadow -or $alpha -le $barycentrics) {
	throw 'No-shadow rejection must follow one-way filtering and precede barycentric/UV/alpha work.'
}
if ([regex]::Matches($core, 'TRACE_STAT_REJECT_NO_SHADOW').Count -ne 1) {
	throw 'No-shadow rejection and attribution must remain singular.'
}

function Resolve-FilterResult(
	[bool]$IgnoreNoShadowCast,
	[bool]$CastsShadow,
	[bool]$Transparent)
{
	if ($IgnoreNoShadowCast -and -not $CastsShadow) { return 'no-shadow' }
	if ($Transparent) { return 'transparent' }
	return 'accepted'
}

if ((Resolve-FilterResult $true $false $true) -ne 'no-shadow') {
	throw 'An overlapping transparent/no-shadow shadow hit must continue instead of blocking.'
}
if ((Resolve-FilterResult $true $true $true) -ne 'transparent') {
	throw 'A shadow-casting transparent hit must retain alpha rejection.'
}
if ((Resolve-FilterResult $true $false $false) -ne 'no-shadow') {
	throw 'An opaque no-shadow hit must continue instead of blocking.'
}
if ((Resolve-FilterResult $false $false $true) -ne 'transparent') {
	throw 'Non-shadow rays must ignore no-shadow-cast and retain alpha rejection.'
}
if ((Resolve-FilterResult $false $false $false) -ne 'accepted') {
	throw 'Non-shadow opaque hits must remain accepted.'
}

Write-Output 'NRI early no-shadow filter contracts passed.'
