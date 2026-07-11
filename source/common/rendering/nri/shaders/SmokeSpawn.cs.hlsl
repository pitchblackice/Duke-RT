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
	const uint spawnCount = min(command.Count, min(particleCapacity, NRI_SMOKE_MAX_PARTICLES_PER_COMMAND));
	InterlockedAdd(gSmokeControl[0].WriteCursor, spawnCount, baseCursor);
	for (uint i = 0u; i < spawnCount; ++i)
	{
		const uint particleIndex = (baseCursor + i) % particleCapacity;
		const SmokeParticle previous = gSmokeParticles[particleIndex];
		if (previous.Active != 0u)
			InterlockedAdd(gSmokeControl[0].LiveEvictions, 1u);
		else
			InterlockedAdd(gSmokeControl[0].ActiveApprox, 1u);

		uint randomState = SmokeHash(command.Serial ^ (i * 0x9e3779b9u));
		const float randomZ = SmokeRandom01(randomState) * 2.0 - 1.0;
		const float randomPhi = SmokeRandom01(randomState) * 6.28318530718;
		const float randomRadius = sqrt(max(0.0, 1.0 - randomZ * randomZ));
		const float3 randomDirection = float3(randomRadius * cos(randomPhi), randomRadius * sin(randomPhi), randomZ);
		const float radialDistance = command.SpawnRadius * pow(SmokeRandom01(randomState), 1.0 / 3.0);
		float3 velocityDirection = randomDirection;
		const float commandVelocityLengthSquared = dot(command.Velocity, command.Velocity);
		if (commandVelocityLengthSquared > 1e-8)
		{
			const float3 coneAxis = command.Velocity * rsqrt(commandVelocityLengthSquared);
			const float coneCosine = cos(radians(clamp(command.VelocityCone, 0.0, 180.0)));
			const float cosTheta = lerp(1.0, coneCosine, SmokeRandom01(randomState));
			const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
			const float phi = SmokeRandom01(randomState) * 6.28318530718;
			const float3 referenceAxis = abs(coneAxis.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
			const float3 tangent = normalize(cross(referenceAxis, coneAxis));
			const float3 bitangent = cross(coneAxis, tangent);
			velocityDirection = coneAxis * cosTheta + (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta;
		}

		SmokeParticle particle;
		particle.Position = command.Position + randomDirection * radialDistance;
		particle.Radius = max(style.Radius * command.RadiusScale, 0.001);
		particle.Velocity = command.Velocity * style.VelocityInherit + velocityDirection * style.VelocityRandom;
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
