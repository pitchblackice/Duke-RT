#include "Include/SmokeResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint commandCount, styleCount, particleCount, controlCount, ignoredStride;
	gSmokeCommands.GetDimensions(commandCount, ignoredStride);
	gSmokeStyles.GetDimensions(styleCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	const uint commandIndex = dispatchThreadId.x;
	const uint particleCapacity = min(gSmokeConstants.ParticleCapacity, particleCount);
	if (commandIndex >= min(gSmokeConstants.CommandCount, commandCount) || particleCapacity == 0u || controlCount == 0u)
		return;

	const SmokeInjectionCommand command = gSmokeCommands[commandIndex];
	if (command.StyleIndex >= min(gSmokeConstants.StyleCount, styleCount) || command.Epoch != gSmokeConstants.SimulationEpoch)
		return;

	const SmokeStyle style = gSmokeStyles[command.StyleIndex];
	uint baseCursor = 0u;
	const uint spawnCount = min(command.RangeCount, min(particleCapacity, NRI_SMOKE_MAX_PARTICLES_PER_COMMAND));
	InterlockedAdd(gSmokeControl[0].WriteCursor, spawnCount, baseCursor);
	for (uint i = 0u; i < spawnCount; ++i)
	{
		const uint particleIndex = (baseCursor + i) % particleCapacity;
		const SmokeParticle previous = gSmokeParticles[particleIndex];
		if (previous.Active != 0u)
			InterlockedAdd(gSmokeControl[0].LiveEvictions, 1u);
		else
			InterlockedAdd(gSmokeControl[0].ActiveApprox, 1u);

		const uint pulseOrdinal = command.RangeBegin + i;
		uint randomState = SmokeHash(command.Serial ^ (pulseOrdinal * 0x9e3779b9u));
		float3 sourcePosition = command.Position;
		float3 halfAxisU, halfAxisV;
		SmokeInjectionRectangleHalfAxes(command, halfAxisU, halfAxisV);
		if (command.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE)
		{
			const float rectangleU = SmokeRandom01(randomState) * 2.0 - 1.0;
			const float rectangleV = SmokeRandom01(randomState) * 2.0 - 1.0;
			sourcePosition += halfAxisU * rectangleU + halfAxisV * rectangleV;
		}
		const float3 randomDirection = SmokeSourceRandomDirection(randomState);
		const float radialDistance = command.SpawnRadius * pow(SmokeRandom01(randomState), 1.0 / 3.0);
		const float3 velocityAxis = SmokeInjectionVelocityAxis(command, halfAxisU, halfAxisV);
		const float3 velocityDirection = SmokeSourceVelocityDirection(velocityAxis,
			command.VelocityCone, randomDirection, randomState);

		SmokeParticle particle;
		particle.Position = sourcePosition + randomDirection * radialDistance;
		particle.Radius = max(style.Radius * command.RadiusScale, 0.001);
		particle.Velocity = command.Velocity * style.VelocityInherit + velocityDirection * style.VelocityRandom;
		particle.Age = 0.0;
		particle.Density = max(style.Density * command.DensityScale, 0.0);
		particle.Lifetime = max(style.Lifetime, 0.001);
		particle.StyleIndex = command.StyleIndex;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		particle.InitialDensity = particle.Density;
		particle.InitialRadius = particle.Radius;
		particle.Serial = SmokeHash(command.Serial + pulseOrdinal);
		particle.Active = 1u;
		gSmokeParticles[particleIndex] = particle;
		InterlockedAdd(gSmokeControl[0].Spawned, 1u);
	}
}
