#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint commandIndex = dispatchThreadId.x;
	if (commandIndex >= gSmokeConstants.CommandCount || gSmokeConstants.ParticleCapacity == 0u)
		return;

	const SmokeInjectionCommand command = gSmokeCommands[commandIndex];
	if (command.StyleIndex >= gSmokeConstants.StyleCount || command.Epoch != gSmokeConstants.SimulationEpoch)
		return;

	const SmokeStyle style = gSmokeStyles[command.StyleIndex];
	uint baseCursor = 0u;
	InterlockedAdd(gSmokeControl[0].WriteCursor, command.Count, baseCursor);
	for (uint i = 0u; i < command.Count; ++i)
	{
		const uint particleIndex = (baseCursor + i) % gSmokeConstants.ParticleCapacity;
		const SmokeParticle previous = gSmokeParticles[particleIndex];
		if (previous.Active != 0u)
			InterlockedAdd(gSmokeControl[0].LiveEvictions, 1u);
		else
			InterlockedAdd(gSmokeControl[0].ActiveApprox, 1u);

		uint randomState = SmokeHash(command.Serial ^ (i * 0x9e3779b9u));
		float3 randomDirection = float3(SmokeRandom01(randomState), SmokeRandom01(randomState), SmokeRandom01(randomState)) * 2.0 - 1.0;
		randomDirection = normalize(randomDirection + float3(0.0001, 0.0002, 0.0003));
		const float radialDistance = command.SpawnRadius * pow(SmokeRandom01(randomState), 1.0 / 3.0);

		SmokeParticle particle;
		particle.Position = command.Position + randomDirection * radialDistance;
		particle.Radius = max(style.Radius * command.RadiusScale, 0.001);
		particle.Velocity = command.Velocity * style.VelocityInherit + randomDirection * style.VelocityRandom;
		particle.Age = 0.0;
		particle.Density = max(style.Density * command.DensityScale, 0.0);
		particle.Lifetime = max(style.Lifetime, 0.001);
		particle.StyleIndex = command.StyleIndex;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		particle.InitialDensity = particle.Density;
		particle.InitialRadius = particle.Radius;
		particle.Serial = SmokeHash(command.Serial + i);
		particle.Active = 1u;
		gSmokeParticles[particleIndex] = particle;
		InterlockedAdd(gSmokeControl[0].Spawned, 1u);
	}
}
