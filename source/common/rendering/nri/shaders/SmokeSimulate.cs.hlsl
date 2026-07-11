#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint particleIndex = dispatchThreadId.x;
	if (particleIndex >= gSmokeConstants.ParticleCapacity)
		return;

	SmokeParticle particle = gSmokeParticles[particleIndex];
	if (particle.Active == 0u)
		return;

	if (particle.Epoch != gSmokeConstants.SimulationEpoch || particle.StyleIndex >= gSmokeConstants.StyleCount)
	{
		particle.Active = 0u;
		gSmokeParticles[particleIndex] = particle;
		InterlockedAdd(gSmokeControl[0].ActiveApprox, 0xffffffffu);
		InterlockedAdd(gSmokeControl[0].Expired, 1u);
		return;
	}

	const SmokeStyle style = gSmokeStyles[particle.StyleIndex];
	const float dt = max(gSmokeConstants.DeltaTime * gSmokeConstants.TimeScale, 0.0);
	particle.Age += dt;
	if (particle.Age >= particle.Lifetime)
	{
		particle.Active = 0u;
		gSmokeParticles[particleIndex] = particle;
		InterlockedAdd(gSmokeControl[0].ActiveApprox, 0xffffffffu);
		InterlockedAdd(gSmokeControl[0].Expired, 1u);
		return;
	}

	uint noiseState = SmokeHash(particle.Serial ^ gSmokeConstants.FrameIndex);
	const float3 noise = float3(SmokeRandom01(noiseState), SmokeRandom01(noiseState), SmokeRandom01(noiseState)) * 2.0 - 1.0;
	const float drag = exp(-max(style.Drag, 0.0) * dt);
	particle.Velocity *= drag;
	particle.Velocity += (gSmokeConstants.Wind + gSmokeConstants.CameraUp * (style.RiseVelocity + style.Buoyancy) + noise * style.Turbulence) * dt;
	particle.Position += particle.Velocity * dt;
	particle.Radius = max(particle.InitialRadius + style.ExpansionVelocity * particle.Age, 0.001);
	particle.Density = particle.InitialDensity * exp2(-particle.Age / max(style.DensityHalfLife, 0.001));
	gSmokeParticles[particleIndex] = particle;
}
