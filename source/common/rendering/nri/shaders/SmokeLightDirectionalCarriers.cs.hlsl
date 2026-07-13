#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint particleCount, visibilityCount, ignoredStride;
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeParticleDirectionalVisibility.GetDimensions(visibilityCount, ignoredStride);
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, min(particleCount, visibilityCount)))
		return;

	gSmokeParticleDirectionalVisibility[particleIndex] = 0.0;
	const SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u || particle.Epoch != gSmokeConstants.SimulationEpoch ||
		gSmokeConstants.LightMode == 0u ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) == 0u)
		return;

	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	if (gSmokeConstants.LightMode < 2u || !castsShadow)
	{
		gSmokeParticleDirectionalVisibility[particleIndex] = 1.0;
		return;
	}
	if (!SmokeShadowTracingReady())
		return;

	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const float3 centerDirection = SmokeDirectionalDirection();
	const uint sampleCount = gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u;
	float visibleSamples = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeDirectionalCarrierRandomSeed(
			particle.Serial,
			sampleIndex,
			gSmokeConstants.DirectionalColorPacked ^
			asuint(gSmokeConstants.DirectionalAngularSize) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionX)) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionY)) ^
			SmokeHash(asuint(gSmokeConstants.DirectionalDirectionZ)));
		const float3 lightDirection = gSmokeConstants.LightMode >= 3u
			? SmokeSampleDirectionalCone(centerDirection, gSmokeConstants.DirectionalAngularSize, randomState)
			: centerDirection;
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].DirectionalSamples, 1u);
			InterlockedAdd(gSmokeControl[0].DirectionalShadowRays, 1u);
		}
		const bool visible = SmokeFilteredVisibilityEffective()
			? SmokePointLightVisibleFiltered(particle.Position, lightDirection, 100000.0, diagnostics)
			: SmokePointLightVisible(particle.Position, lightDirection, 100000.0, diagnostics);
		if (visible)
		{
			visibleSamples += 1.0;
			if (diagnostics) InterlockedAdd(gSmokeControl[0].DirectionalShadowVisible, 1u);
		}
		else if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].DirectionalShadowOccluded, 1u);
		}
	}
	gSmokeParticleDirectionalVisibility[particleIndex] = visibleSamples / (float)sampleCount;
}
