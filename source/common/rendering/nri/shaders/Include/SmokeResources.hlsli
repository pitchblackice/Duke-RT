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

// Projection work is bounded per particle. Cell residency itself is complete:
// every emitted reference receives a unique node in its selected tier.
#define NRI_SMOKE_MAX_PARTICLES_PER_COMMAND 256u
#define NRI_SMOKE_WIDE_GRID_X 16u
#define NRI_SMOKE_WIDE_GRID_Y 9u
#define NRI_SMOKE_WIDE_CELL_COUNT (NRI_SMOKE_WIDE_GRID_X * NRI_SMOKE_WIDE_GRID_Y)
#define NRI_SMOKE_MAX_TIER_REFERENCES 512u
#define NRI_SMOKE_REFERENCE_END 0xffffffffu

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

struct SmokeCellHeader
{
	uint Head;
	uint Count;
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
	uint ReferenceInvalidLinks;
	uint ReferenceTraversalLimitExits;
	uint GlobalDepthReferences;
	uint FineTierParticles;
	uint WideTierParticles;
	uint GlobalTierParticles;
	uint FineOccupiedCells;
	uint WideOccupiedCells;
	uint GlobalOccupiedSlices;
	uint FineMaximumCellReferences;
	uint WideMaximumCellReferences;
	uint GlobalMaximumCellReferences;
	uint3 ReferenceDiagnosticsPadding0;
	uint3 ReferenceDiagnosticsPadding1;
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
	uint DirectionalFroxelsProcessed;
	uint DirectionalSamples;
	uint DirectionalShadowRays;
	uint DirectionalShadowVisible;
	uint DirectionalShadowOccluded;
	uint DirectionalRadianceClamps;
	uint EmissiveFroxelsProcessed;
	uint EmissiveSamples;
	uint EmissiveCandidateMisses;
	uint EmissiveDistanceRejected;
	uint EmissiveFacingRejected;
	uint EmissiveShadowRays;
	uint EmissiveShadowVisible;
	uint EmissiveShadowOccluded;
	uint EmissiveContributed;
	uint EmissiveRadianceClamps;
	uint IndirectFroxelsProcessed;
	uint IndirectLocalityRays;
	uint IndirectLocalityAgreement;
	uint IndirectLocalityOneSided;
	uint IndirectLocalityMismatch;
	uint IndirectLocalityInvalid;
	uint IndirectReferenceRays;
	uint IndirectReferenceHits;
	uint IndirectReferenceMisses;
	uint IndirectSectorContributions;
	uint IndirectSkyContributions;
	uint IndirectEmissionContributions;
	uint IndirectRadianceClamps;
	uint IndirectNanRejects;
	uint Padding;
};

StructuredBuffer<SmokeStyle> gSmokeStyles : register(t0, space0);
StructuredBuffer<SmokeInjectionCommand> gSmokeCommands : register(t1, space0);

RWStructuredBuffer<SmokeParticle> gSmokeParticles : register(u0, space1);
RWStructuredBuffer<SmokeControl> gSmokeControl : register(u1, space1);
RWStructuredBuffer<SmokeCellHeader> gSmokeFineCells : register(u2, space1);
RWStructuredBuffer<uint> gSmokeReferenceNext : register(u3, space1);
RWStructuredBuffer<float4> gSmokeFroxelMedium : register(u4, space1);
RWStructuredBuffer<float4> gSmokeFroxelIntegrated : register(u5, space1);
RWStructuredBuffer<SmokeCellHeader> gSmokeWideCells : register(u6, space1);
RWStructuredBuffer<SmokeCellHeader> gSmokeGlobalDepthCells : register(u7, space1);
RWStructuredBuffer<float4> gSmokeFroxelPhase : register(u8, space1);
RWStructuredBuffer<float4> gSmokeFroxelSource : register(u9, space1);
RWStructuredBuffer<uint> gSmokeOccupiedFroxelIndices : register(u10, space1);

Texture2D<float4> gSmokeSceneInput : register(t0, space2);
Texture2D<float4> gSmokeViewZInput : register(t1, space2);
RWTexture2D<float4> gSmokeOutput : register(u0, space3);

NRI_ROOT_CONSTANTS(SmokeConstants, gSmokeConstants, 0, NRI_SMOKE_SET_ROOT);

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
