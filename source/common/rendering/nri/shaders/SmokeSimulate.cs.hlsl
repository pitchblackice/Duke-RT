#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint particleCount, controlCount, styleCount, ignoredStride;
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= min(gSmokeConstants.ParticleCapacity, particleCount))
		return;

	SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u)
		return;

	if (particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount))
	{
		particle.Active = 0u;
		gSmokeParticles[particleIndex] = particle;
		if (controlCount != 0u)
		{
			InterlockedAdd(gSmokeControl[0].ActiveApprox, 0xffffffffu);
			InterlockedAdd(gSmokeControl[0].Expired, 1u);
		}
		return;
	}

	const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
	const float dt = max(gSmokeConstants.DeltaTime * gSmokeConstants.TimeScale, 0.0);
	particle.Age += dt;
	if (particle.Age >= particle.Lifetime)
	{
		particle.Active = 0u;
		gSmokeParticles[particleIndex] = particle;
		if (controlCount != 0u)
		{
			InterlockedAdd(gSmokeControl[0].ActiveApprox, 0xffffffffu);
			InterlockedAdd(gSmokeControl[0].Expired, 1u);
		}
		return;
	}

	uint noiseState = SmokeHash(particle.Serial ^ gSmokeConstants.FrameIndex);
	const float3 noise = float3(SmokeRandom01(noiseState), SmokeRandom01(noiseState), SmokeRandom01(noiseState)) * 2.0 - 1.0;
	const float drag = exp(-max(style.Drag, 0.0) * dt);
	particle.Velocity *= drag;
	particle.Velocity += (gSmokeConstants.Wind + gSmokeConstants.CameraUp * (style.RiseVelocity + style.Buoyancy) + noise * style.Turbulence) * dt;
	particle.Position += particle.Velocity * dt;
	particle.Radius = max(particle.InitialRadius + style.ExpansionVelocity * particle.Age, 0.001);
	const float expansionRatio = saturate(particle.InitialRadius / particle.Radius);
	const float expansionDilution = expansionRatio * expansionRatio * expansionRatio;
	particle.Density = particle.InitialDensity * expansionDilution * exp2(-particle.Age / max(style.DensityHalfLife, 0.001));
	gSmokeParticles[particleIndex] = particle;
}
