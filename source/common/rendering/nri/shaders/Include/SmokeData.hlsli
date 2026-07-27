#ifndef NRI_SMOKE_DATA_HLSLI
#define NRI_SMOKE_DATA_HLSLI

#define NRI_SMOKE_INJECTION_SHAPE_SPHERE 0u
#define NRI_SMOKE_INJECTION_SHAPE_RECTANGLE 1u
#define NRI_SMOKE_SOURCE_CLASS_AMBIENT 0u
#define NRI_SMOKE_SOURCE_CLASS_INTERACTIVE_ACTOR 1u
#define NRI_SMOKE_SOURCE_CLASS_INTERACTIVE_EVENT 2u
#define NRI_SMOKE_SOURCE_CLASS_DIAGNOSTIC 3u

struct SmokeStyle
{
	float3 Albedo;
	float Extinction;
	float Anisotropy;
	float Radius;
	float ExpansionVelocity;
	float Lifetime;
	float Density;
	float DensityHalfLife;
	float RiseVelocity;
	float VelocityRandom;
	float VelocityInherit;
	float Buoyancy;
	float Drag;
	float Turbulence;
	float TurbulenceScale;
	float Temperature;
	float MomentumScale;
	float CoolingHalfLife;
};

struct SmokeInjectionCommand
{
	float3 Position;
	float SpawnRadius;
	float3 Velocity;
	uint StyleIndex;
	uint Count;
	uint Serial;
	float DensityScale;
	float RadiusScale;
	float VelocityCone;
	uint Epoch;
	float3 HalfAxisU;
	uint Shape;
	float3 HalfAxisV;
	uint SourceId;
	uint SourceSlot;
	uint SourceMetadata;
	uint RangeBegin;
	uint RangeCount;
	uint PulseIdLow;
	uint PulseIdHigh;
};

uint SmokeInjectionSourceClass(SmokeInjectionCommand command)
{
	return command.SourceMetadata & 0xffu;
}

uint SmokeInjectionSourcePriority(SmokeInjectionCommand command)
{
	return (command.SourceMetadata >> 8u) & 0xffu;
}

void SmokeInjectionRectangleHalfAxes(SmokeInjectionCommand command,
	out float3 halfAxisU, out float3 halfAxisV)
{
	halfAxisU = 0.0;
	halfAxisV = 0.0;
	if (command.Shape != NRI_SMOKE_INJECTION_SHAPE_RECTANGLE)
		return;

	halfAxisU = all(isfinite(command.HalfAxisU)) ? command.HalfAxisU : 0.0;
	halfAxisV = all(isfinite(command.HalfAxisV)) ? command.HalfAxisV : 0.0;
	const float uLength = length(halfAxisU);
	const float vLength = length(halfAxisV);
	if (!isfinite(uLength) || uLength <= 1e-6)
		halfAxisU = 0.0;
	if (!isfinite(vLength) || vLength <= 1e-6)
		halfAxisV = 0.0;
}

float3 SmokeInjectionClosestRectanglePoint(float3 position, float3 center,
	float3 halfAxisU, float3 halfAxisV)
{
	const float3 offset = position - center;
	float3 closest = center;
	const float uLengthSquared = dot(halfAxisU, halfAxisU);
	const float vLengthSquared = dot(halfAxisV, halfAxisV);
	if (uLengthSquared > 1e-8)
		closest += halfAxisU * clamp(dot(offset, halfAxisU) / uLengthSquared, -1.0, 1.0);
	if (vLengthSquared > 1e-8)
		closest += halfAxisV * clamp(dot(offset, halfAxisV) / vLengthSquared, -1.0, 1.0);
	return closest;
}

float3 SmokeInjectionVelocityAxis(SmokeInjectionCommand command,
	float3 halfAxisU, float3 halfAxisV)
{
	const float3 velocity = all(isfinite(command.Velocity)) ? command.Velocity : 0.0;
	if (dot(velocity, velocity) > 1e-8)
		return velocity;
	if (command.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE)
	{
		// The Build-to-render transform reflects handedness, so V x U points
		// along the transformed authored normal.
		const float3 normal = cross(halfAxisV, halfAxisU);
		const float normalLengthSquared = dot(normal, normal);
		if (isfinite(normalLengthSquared) && normalLengthSquared > 1e-8)
			return normal * rsqrt(normalLengthSquared);
	}
	return 0.0;
}

bool SmokeInjectionTraversalFits(uint3 extent, uint maximumElements)
{
	if (any(extent == 0u) || extent.x > maximumElements)
		return false;
	uint product = extent.x;
	if (extent.y > maximumElements / product)
		return false;
	product *= extent.y;
	return extent.z <= maximumElements / product;
}

#endif
