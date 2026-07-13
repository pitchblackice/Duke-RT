#ifndef NRI_SMOKE_DATA_HLSLI
#define NRI_SMOKE_DATA_HLSLI

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
	uint2 Padding;
};

#endif
