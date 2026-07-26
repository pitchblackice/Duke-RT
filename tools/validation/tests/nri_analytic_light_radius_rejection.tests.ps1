Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}

function Get-SourceSlice([string]$Source, [string]$StartMarker, [string]$EndMarker) {
	$start = $Source.IndexOf($StartMarker)
	Assert-True ($start -ge 0) "missing source marker '$StartMarker'"
	$end = $Source.IndexOf($EndMarker, $start + $StartMarker.Length)
	Assert-True ($end -gt $start) "missing source marker '$EndMarker' after '$StartMarker'"
	return $Source.Substring($start, $end - $start)
}

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$shader = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\shaders\TraceOpaque.cs.hlsl') -Raw
$cvars = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\renderer\nri_cvars.cpp') -Raw

$mirrorLoop = Get-SourceSlice $shader `
	'float3 EvaluatePlainMirrorSurfaceGlint' `
	'float3 emissiveSampleDiffuse'
$primaryLoop = Get-SourceSlice $shader `
	'const uint runtimeLightCandidateCount = plainMirrorMaterial ? 0u : runtimeLightTile.indexCount;' `
	'float3 emissiveSampleDiffuse'

$mirrorRadiusContract = @'
const float lightRadiusSq = runtimeLight.radius * runtimeLight.radius;
		if (runtimeLight.radius <= 0.0 ||
			lightDistanceSq <= 0.0001 ||
			lightDistanceSq >= lightRadiusSq)
		{
			continue;
		}

		const float lightDistance = sqrt(lightDistanceSq);
'@
$primaryRadiusContract = @'
const float lightRadiusSq = runtimeLight.radius * runtimeLight.radius;
					if (runtimeLight.radius <= 0.0 ||
						centerLightDistanceSq <= 0.0001 ||
						centerLightDistanceSq >= lightRadiusSq)
					{
						continue;
					}

					const float centerLightDistance = sqrt(centerLightDistanceSq);
'@

Assert-True $mirrorLoop.Contains($mirrorRadiusContract.TrimEnd()) `
	'plain-mirror analytic lights must reject nonpositive-radius, coincident, and out-of-radius candidates before sqrt'
Assert-True $primaryLoop.Contains($primaryRadiusContract.TrimEnd()) `
	'primary analytic lights must reject nonpositive-radius, coincident, and out-of-radius candidates before sqrt'
Assert-True (([regex]::Matches($shader, 'const float lightRadiusSq = runtimeLight\.radius \* runtimeLight\.radius;')).Count -eq 2) `
	'TraceOpaque must contain exactly two squared-radius analytic-light rejection sites'
Assert-True ($shader -notmatch '(?:lightDistance|centerLightDistance)\s*>=\s*runtimeLight\.radius') `
	'analytic-light loops must not compute distance before radius rejection'

Assert-True ($mirrorLoop -match 'const float3 lightDir = toLight / lightDistance;[\s\S]*?ComputePointLightShadow\([^;]*lightDistance\)[\s\S]*?EvaluateAnalyticPointLightAttenuation\(lightDistance, runtimeLight\.radius, runtimeLight\.intensity\)') `
	'plain-mirror accepted-light direction, visibility, and attenuation math changed'
Assert-True ($primaryLoop -match 'const float3 centerLightDir = toLightCenter / centerLightDistance;[\s\S]*?runtimeLightStableSeed[\s\S]*?SampleRuntimePointEmitter\(runtimeLight, centerLightDir, runtimeLightRng\)[\s\S]*?const float lightDistance = sqrt\(lightDistanceSq\);[\s\S]*?ComputePointLightShadow\([^;]*lightDistance\)[\s\S]*?EvaluateAnalyticPointLightAttenuation\(centerLightDistance, runtimeLight\.radius, runtimeLight\.intensity\)') `
	'primary accepted-light RNG, direction, visibility, or attenuation math changed'

foreach ($default in @(
	'CVAR(Int, nri_ptlightbounces, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
	'CVAR(Int, nri_ptmirrorbounces, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
	'CVAR(Int, nri_ptportaldepth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
	'CVAR(Int, nri_ptemissivesamples, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)'
)) {
	Assert-True $cvars.Contains($default) "trace default changed: $default"
}

Write-Host 'NRI analytic-light radius rejection contracts passed'
