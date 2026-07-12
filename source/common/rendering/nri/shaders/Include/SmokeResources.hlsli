#ifndef NRI_SMOKE_RESOURCES_HLSLI
#define NRI_SMOKE_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeConstants.hlsli"

#define NRI_SMOKE_SET_INPUTS 0
#define NRI_SMOKE_SET_BUFFERS 1
#define NRI_SMOKE_SET_TEXTURES 2
#define NRI_SMOKE_SET_OUTPUT 3
#define NRI_SMOKE_SET_LIGHTS 4
#define NRI_SMOKE_SET_ROOT 5

// Hard ceilings keep overlapping or near-camera effects from turning smoke into
// an unbounded per-frame workload.
#define NRI_SMOKE_MAX_PARTICLES_PER_COMMAND 256u
#define NRI_SMOKE_FINE_CELL_CAPACITY 8u
#define NRI_SMOKE_WIDE_GRID_X 16u
#define NRI_SMOKE_WIDE_GRID_Y 9u
#define NRI_SMOKE_WIDE_CELL_COUNT (NRI_SMOKE_WIDE_GRID_X * NRI_SMOKE_WIDE_GRID_Y)
#define NRI_SMOKE_WIDE_CELL_CAPACITY 16u
#define NRI_SMOKE_GLOBAL_DEPTH_CAPACITY 16u
#define NRI_SMOKE_MAX_TIER_REFERENCES 512u
#define NRI_SMOKE_MAX_CANDIDATES_PER_FROXEL (NRI_SMOKE_FINE_CELL_CAPACITY + NRI_SMOKE_WIDE_CELL_CAPACITY + NRI_SMOKE_GLOBAL_DEPTH_CAPACITY)

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
	uint WideParticlesProjected;
	uint LightCandidatesTested;
	uint LightDistanceRejected;
	uint LightShadowRays;
	uint LightShadowVisible;
	uint LightShadowOccluded;
	uint LightSoftSamples;
	uint LightRadianceClamps;
	uint WideGlobalDrops;
	uint FilterCandidateHits;
	uint FilterAlphaRejects;
	uint FilterNoShadowRejects;
	uint FilterOneWayRejects;
	uint FilterReflectionRejects;
	uint FilterPortalContinuations;
	uint FilterAcceptedBlockers;
	uint FilterMisses;
	uint FilterSkipLimitExits;
	uint FilterContinuationLimitExits;
	uint FilterResourceDowngrades;
	uint FineColumnReferences;
	uint WideCellReferences;
	uint SelectionCollisions;
	uint SelectionReplacements;
	uint GlobalDepthReferences;
	uint FineTierParticles;
	uint WideTierParticles;
	uint GlobalTierParticles;
	uint FineOccupiedCells;
	uint WideOccupiedCells;
	uint GlobalOccupiedSlices;
	uint FineSelectionCollisions;
	uint WideSelectionCollisions;
	uint GlobalSelectionCollisions;
	uint FineSelectionReplacements;
	uint WideSelectionReplacements;
	uint GlobalSelectionReplacements;
	uint FineSelectionLosses;
	uint WideSelectionLosses;
	uint GlobalSelectionLosses;
	uint MaximumDepthSpan;
	uint DepthSpanOne;
	uint DepthSpanTwoToFour;
	uint DepthSpanFiveToSixteen;
	uint DepthSpanOverSixteen;
	uint MaximumCandidatesPerFroxel;
	uint OccupiedCount;
	uint OccupiedOverflow;
	uint MediumCandidateTests;
	uint PointFroxelsProcessed;
	uint3 Padding;
};

StructuredBuffer<SmokeStyle> gSmokeStyles : register(t0, space0);
StructuredBuffer<SmokeInjectionCommand> gSmokeCommands : register(t1, space0);

RWStructuredBuffer<SmokeParticle> gSmokeParticles : register(u0, space1);
RWStructuredBuffer<SmokeControl> gSmokeControl : register(u1, space1);
RWStructuredBuffer<uint> gSmokeFineCellCounts : register(u2, space1);
RWStructuredBuffer<uint> gSmokeFineCellIndices : register(u3, space1);
RWStructuredBuffer<float4> gSmokeFroxelMedium : register(u4, space1);
RWStructuredBuffer<float4> gSmokeFroxelIntegrated : register(u5, space1);
RWStructuredBuffer<uint> gSmokeWideCellCounts : register(u6, space1);
RWStructuredBuffer<uint> gSmokeWideCellIndices : register(u7, space1);
RWStructuredBuffer<uint> gSmokeGlobalDepthCounts : register(u8, space1);
RWStructuredBuffer<uint> gSmokeGlobalDepthIndices : register(u9, space1);
RWStructuredBuffer<float4> gSmokeFroxelPhase : register(u10, space1);
RWStructuredBuffer<float4> gSmokeFroxelSource : register(u11, space1);
RWStructuredBuffer<uint> gSmokeOccupiedFroxelIndices : register(u12, space1);

Texture2D<float4> gSmokeSceneInput : register(t0, space2);
Texture2D<float4> gSmokeViewZInput : register(t1, space2);
RWTexture2D<float4> gSmokeOutput : register(u0, space3);

NRI_ROOT_CONSTANTS(SmokeConstants, gSmokeConstants, 0, NRI_SMOKE_SET_ROOT);

uint SmokeFroxelIndex(uint x, uint y, uint z)
{
	return (z * gSmokeConstants.FroxelHeight + y) * gSmokeConstants.FroxelWidth + x;
}

uint SmokeFroxelCount()
{
	return gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight * gSmokeConstants.FroxelDepth;
}

uint3 SmokeFroxelCoordinates(uint froxelIndex)
{
	const uint planeSize = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight;
	const uint z = froxelIndex / max(planeSize, 1u);
	const uint planeIndex = froxelIndex - z * planeSize;
	const uint y = planeIndex / max(gSmokeConstants.FroxelWidth, 1u);
	return uint3(planeIndex - y * gSmokeConstants.FroxelWidth, y, z);
}

float3 SmokeFroxelRay(uint2 froxelPosition)
{
	const float2 uv = (float2(froxelPosition) + 0.5) / float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	return gSmokeConstants.CameraForward +
		gSmokeConstants.CameraRight * (ndc.x * gSmokeConstants.TanHalfFovX) +
		gSmokeConstants.CameraUp * (ndc.y * gSmokeConstants.TanHalfFovY);
}

uint SmokeWideCellIndex(uint x, uint y, uint z)
{
	return (z * NRI_SMOKE_WIDE_GRID_Y + y) * NRI_SMOKE_WIDE_GRID_X + x;
}

float SmokeSliceFarDepth(uint z)
{
	const float normalizedDepth = (float)(z + 1u) / max((float)gSmokeConstants.FroxelDepth, 1.0);
	return gSmokeConstants.FroxelMaxDistance * pow(normalizedDepth, max(gSmokeConstants.DepthExponent, 0.001));
}

float SmokeSliceNearDepth(uint z)
{
	return z == 0u ? 0.0 : SmokeSliceFarDepth(z - 1u);
}

float3 SmokeFroxelCenter(uint3 froxelPosition, float3 ray)
{
	const float nearDepth = SmokeSliceNearDepth(froxelPosition.z);
	const float farDepth = SmokeSliceFarDepth(froxelPosition.z);
	return gSmokeConstants.CameraPosition + ray * ((nearDepth + farDepth) * 0.5);
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

uint SmokePackCandidate(SmokeParticle particle, uint particleIndex)
{
	// Positive IEEE-754 values retain their ordering as unsigned integers.
	// Keep the upper 16 bits as a compact optical-importance key and use the
	// particle index as a deterministic tie breaker and recoverable payload.
	const float opticalImportance = max(particle.Density * particle.Radius, 1e-20);
	const uint importance = max(asuint(opticalImportance) >> 16u, 1u);
	return (importance << 16u) | (particleIndex & 0xffffu);
}

uint SmokeUnpackCandidateIndex(uint packedCandidate)
{
	return packedCandidate & 0xffffu;
}

#endif
