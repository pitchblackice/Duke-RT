#ifndef NRI_SMOKE_RESOURCES_HLSLI
#define NRI_SMOKE_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeConstants.hlsli"

#define NRI_SMOKE_SET_INPUTS 0
#define NRI_SMOKE_SET_BUFFERS 1
#define NRI_SMOKE_SET_TEXTURES 2
#define NRI_SMOKE_SET_OUTPUT 3
#define NRI_SMOKE_SET_ROOT 4

// Hard ceilings keep overlapping or near-camera effects from turning smoke into
// an unbounded per-frame workload.
#define NRI_SMOKE_MAX_BIN_COLUMNS_PER_AXIS 8u
#define NRI_SMOKE_MAX_PARTICLES_PER_COMMAND 256u
#define NRI_SMOKE_MAX_EVALUATED_PARTICLES_PER_COLUMN 32u

struct SmokeParticle
{
	float3 Position;
	float Radius;
	float3 Velocity;
	float Age;
	float Density;
	float Lifetime;
	uint StyleIndex;
	uint Epoch;
	float InitialDensity;
	float InitialRadius;
	uint Serial;
	uint Active;
};

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
	float3 Padding;
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

struct SmokeControl
{
	uint WriteCursor;
	uint ActiveApprox;
	uint LiveEvictions;
	uint ColumnOverflow;
	uint Epoch;
	uint Spawned;
	uint Expired;
	uint Reserved;
};

StructuredBuffer<SmokeStyle> gSmokeStyles : register(t0, space0);
StructuredBuffer<SmokeInjectionCommand> gSmokeCommands : register(t1, space0);

RWStructuredBuffer<SmokeParticle> gSmokeParticles : register(u0, space1);
RWStructuredBuffer<SmokeControl> gSmokeControl : register(u1, space1);
RWStructuredBuffer<uint> gSmokeColumnCounts : register(u2, space1);
RWStructuredBuffer<uint> gSmokeColumnIndices : register(u3, space1);
RWStructuredBuffer<float4> gSmokeFroxelLocal : register(u4, space1);
RWStructuredBuffer<float4> gSmokeFroxelIntegrated : register(u5, space1);

Texture2D<float4> gSmokeSceneInput : register(t0, space2);
Texture2D<float4> gSmokeViewZInput : register(t1, space2);
RWTexture2D<float4> gSmokeOutput : register(u0, space3);

NRI_ROOT_CONSTANTS(SmokeConstants, gSmokeConstants, 0, NRI_SMOKE_SET_ROOT);

uint SmokeFroxelIndex(uint x, uint y, uint z)
{
	return (z * gSmokeConstants.FroxelHeight + y) * gSmokeConstants.FroxelWidth + x;
}

void SmokeLimitColumnRange(inout int minimumColumn, inout int maximumColumn)
{
	const int maximumSpan = (int)NRI_SMOKE_MAX_BIN_COLUMNS_PER_AXIS;
	const int span = maximumColumn - minimumColumn + 1;
	if (span <= maximumSpan)
		return;

	const int originalMinimum = minimumColumn;
	const int originalMaximum = maximumColumn;
	const int center = (originalMinimum + originalMaximum) / 2;
	minimumColumn = clamp(center - maximumSpan / 2, originalMinimum, originalMaximum - maximumSpan + 1);
	maximumColumn = minimumColumn + maximumSpan - 1;
}

float SmokeSliceFarDepth(uint z)
{
	const float normalizedDepth = (float)(z + 1u) / max((float)gSmokeConstants.FroxelDepth, 1.0);
	return gSmokeConstants.FroxelMaxDistance * pow(normalizedDepth, max(gSmokeConstants.DepthExponent, 0.001));
}

uint SmokeDepthSlice(float viewDepth)
{
	const float normalizedDepth = saturate(viewDepth / max(gSmokeConstants.FroxelMaxDistance, 0.001));
	const float slice = pow(normalizedDepth, 1.0 / max(gSmokeConstants.DepthExponent, 0.001));
	return min((uint)(slice * gSmokeConstants.FroxelDepth), gSmokeConstants.FroxelDepth - 1u);
}

uint SmokeHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}

float SmokeRandom01(inout uint state)
{
	state = SmokeHash(state);
	return (float)(state & 0x00ffffffu) / 16777216.0;
}

#endif
