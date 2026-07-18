#ifndef NRI_SMOKE_RESOURCES_HLSLI
#define NRI_SMOKE_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeConstants.hlsli"
#include "SmokeData.hlsli"
#include "SmokeSourceShaping.hlsli"
#include "SmokeGridData.hlsli"
#include "SmokeGridLightingData.hlsli"

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
#define NRI_SMOKE_DIRECTIONAL_PROBE_AXIS 20u
#define NRI_SMOKE_DIRECTIONAL_PROBE_HALF_AXIS 10
#define NRI_SMOKE_DIRECTIONAL_PROBES_PER_PARTICLE 8000u
#define NRI_SMOKE_DIRECTIONAL_PROBE_CELL_SIZE 2.0
int3 SmokeDirectionalProbeWindowMinCell(float3 particlePosition)
{
	const int3 anchorCell = (int3)floor(particlePosition / NRI_SMOKE_DIRECTIONAL_PROBE_CELL_SIZE);
	return anchorCell - (NRI_SMOKE_DIRECTIONAL_PROBE_HALF_AXIS - 1);
}

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

struct SmokeCellHeader
{
	uint Head;
	uint Count;
};

struct SmokeIndirectCacheRecord
{
	float3 Radiance;
	float SigmaT;
	float3 WorldPosition;
	uint Metadata;
};

struct SmokeDirectCacheRecord
{
	float3 Radiance;
	float SigmaT;
	float3 WorldPosition;
	uint Metadata;
	float MediumTransmittance;
	uint MediumMetadata;
};

struct SmokeEmissiveReservoirRecord
{
	uint CandidateIndex;
	uint SampleSeed;
	uint StableKeyLo;
	uint StableKeyHi;
	float Target;
	float WeightSum;
	uint Metadata;
	uint Generation;
	float3 ReceiverPosition;
	float SigmaT;
};

// The three emissive UAVs retain a fixed 48-byte stride. Particle froxels use
// the legacy reservoir encoding while grid froxels use packed lane pairs and
// tagged RGB moment records in the same storage.
struct SmokeEmissiveStorageRecord
{
	uint4 Data0;
	uint4 Data1;
	uint4 Data2;
};

struct SmokeEmissiveLaneRecord
{
	uint CandidateIndex;
	uint SampleSeed;
	uint StableKeyLo;
	uint StableKeyHi;
	float Target;
	float WeightSum;
};

struct SmokeEmissiveMomentRecord
{
	float3 MeanRadiance;
	float3 SecondMoment;
	float3 ReceiverPosition;
	float SigmaT;
	uint Direction;
	uint Metadata;
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
	uint EmissiveInnerRisSets;
	uint EmissiveInnerPointProposals;
	uint EmissiveInnerZeroProposals;
	uint EmissiveInnerRisRejects;
	uint EmissiveInnerSelections;
	uint EmissiveInnerVisibilityRays;
	uint EmissiveInnerSourceVisibilityRays;
	uint EmissiveInnerVisibilityVisible;
	uint EmissiveInnerBlockerReceiverImmediate;
	uint EmissiveInnerBlockerReceiverCell;
	uint EmissiveInnerBlockerEmitterCell;
	uint EmissiveInnerBlockerInterior;
	uint EmissiveInnerSourceSelections;
	uint EmissiveInnerSourceOverflow;
	uint EmissiveTargetVisibilityRays;
	uint EmissiveTargetVisibilityVisible;
	uint EmissiveTargetBlockerExact;
	uint EmissiveTargetBlockerRange;
	uint EmissiveTargetBlockerOther;
	uint EmissiveTargetWitnessClaim;
	uint EmissiveTargetWitnessCandidate;
	uint EmissiveTargetWitnessRelation;
	uint EmissiveTargetWitnessSamplePrimitive;
	uint EmissiveTargetWitnessSampleMaterial;
	uint EmissiveTargetWitnessBlockerDataSource;
	uint EmissiveTargetWitnessBlockerInstance;
	uint EmissiveTargetWitnessBlockerPrimitive;
	uint EmissiveTargetWitnessBlockerMaterial;
	uint EmissiveTargetWitnessDistanceBits;
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
	uint EmissiveReservoirInitial;
	uint EmissiveReservoirInvalid;
	uint EmissiveTemporalAccepted;
	uint EmissiveTemporalRejected;
	uint EmissiveSpatialAccepted;
	uint EmissiveSpatialRejected;
	uint EmissiveFinalEvaluations;
	uint EmissiveSourceClamps;
	uint EmissiveRemovedEnergy;
	uint EmissiveMaximumAge;
	uint EmissiveReferenceSamples;
	uint EmissiveReferenceRays;
	uint EmissiveIdentityRejects;
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
	uint IndirectTemporalAccepted;
	uint IndirectTemporalRejected;
	uint IndirectSpatialAccepted;
	uint IndirectSpatialRejected;
	uint IndirectCacheMaximumAge;
	uint IndirectCacheClamps;
	uint IndirectCacheResolved;
	uint DirectReceiverSamples;
	uint DirectFractionalVisibility;
	uint DirectVisibilityZero;
	uint DirectVisibilityOne;
	uint DirectTemporalAccepted;
	uint DirectTemporalRejected;
	uint DirectSpatialAccepted;
	uint DirectSpatialRejected;
	uint DirectHistoryMaximumAge;
	uint DirectHistoryResolved;
	uint DirectHistoryClamps;
	uint DirectNanRejects;
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
RWStructuredBuffer<SmokeIndirectCacheRecord> gSmokeIndirectHistory : register(u11, space1);
RWStructuredBuffer<SmokeIndirectCacheRecord> gSmokeIndirectScratch : register(u12, space1);
RWStructuredBuffer<float> gSmokeParticleDirectionalVisibility : register(u13, space1);
RWStructuredBuffer<SmokeEmissiveStorageRecord> gSmokeEmissiveCurrent : register(u14, space1);
RWStructuredBuffer<SmokeEmissiveStorageRecord> gSmokeEmissiveTemporal : register(u15, space1);
RWStructuredBuffer<SmokeEmissiveStorageRecord> gSmokeEmissiveHistory : register(u16, space1);
RWStructuredBuffer<SmokeGridControl> gSmokeRenderGridControl : register(u17, space1);
RWStructuredBuffer<SmokeGridHashEntry> gSmokeRenderGridHash : register(u18, space1);
RWStructuredBuffer<SmokeGridBrick> gSmokeRenderGridBricks : register(u19, space1);
RWStructuredBuffer<float4> gSmokeRenderGridScalarA : register(u20, space1);
RWStructuredBuffer<float4> gSmokeRenderGridScalarB : register(u21, space1);
RWStructuredBuffer<float4> gSmokeRenderGridVelocityA : register(u22, space1);
RWStructuredBuffer<float4> gSmokeRenderGridVelocityB : register(u23, space1);
RWStructuredBuffer<float4> gSmokeRenderGridOpticalA : register(u24, space1);
RWStructuredBuffer<float4> gSmokeRenderGridOpticalB : register(u25, space1);
RWStructuredBuffer<float4> gSmokeRenderGridDynamicsA : register(u26, space1);
RWStructuredBuffer<float4> gSmokeRenderGridDynamicsB : register(u27, space1);
RWStructuredBuffer<SmokeDirectCacheRecord> gSmokeDirectCurrent : register(u28, space1);
RWStructuredBuffer<SmokeDirectCacheRecord> gSmokeDirectHistory : register(u29, space1);
RWStructuredBuffer<SmokeGridLightRecord> gSmokeGridLightCurrent : register(u30, space1);
RWStructuredBuffer<SmokeGridLightRecord> gSmokeGridLightHistory : register(u31, space1);
RWStructuredBuffer<uint> gSmokeGridLightActive : register(u32, space1);
RWStructuredBuffer<SmokeGridLightControl> gSmokeGridLightControl : register(u33, space1);
RWStructuredBuffer<uint4> gSmokeGridLightLinks : register(u34, space1);
RWStructuredBuffer<SmokeGridLightRecord> gSmokeGridLightFiltered : register(u35, space1);
RWStructuredBuffer<SmokeGridLightProposal> gSmokeGridLightProposals : register(u36, space1);
RWStructuredBuffer<float4> gSmokeGridScatterSeed : register(u37, space1);
RWStructuredBuffer<float4> gSmokeGridScatterBounceA : register(u38, space1);
RWStructuredBuffer<float4> gSmokeGridScatterBounceB : register(u39, space1);
RWStructuredBuffer<SmokeGridScatterMetadata> gSmokeGridScatterMetadata : register(u40, space1);
RWStructuredBuffer<uint> gSmokeGridScatterActive : register(u41, space1);
RWStructuredBuffer<SmokeGridLightRecord> gSmokeGridLightSelfShadowCurrent : register(u42, space1);
RWStructuredBuffer<SmokeGridLightRecord> gSmokeGridLightSelfShadowHistory : register(u43, space1);

Texture2D<float4> gSmokeSceneInput : register(t0, space2);
Texture2D<float4> gSmokeViewZInput : register(t1, space2);
Texture2D<float4> gSmokeVolumeHistoryInput : register(t2, space2);
Texture2D<float4> gSmokeVolumeMetaInput : register(t3, space2);
Texture2D<float4> gSmokeVolumeResolvedInput : register(t4, space2);
Texture2D<float4> gSmokeVolumeResolvedMetaInput : register(t5, space2);
Texture2D<float4> gSmokeVolumeCurrentInput : register(t6, space2);
Texture2D<float4> gSmokeVolumeCurrentMetaInput : register(t7, space2);
RWTexture2D<float4> gSmokeOutput : register(u0, space3);
RWTexture2D<float4> gSmokeVolumeCurrentOutput : register(u1, space3);
RWTexture2D<float4> gSmokeVolumeCurrentMetaOutput : register(u2, space3);
RWTexture2D<float4> gSmokeVolumeHistoryOutput : register(u3, space3);
RWTexture2D<float4> gSmokeVolumeMetaOutput : register(u4, space3);

NRI_ROOT_CONSTANTS(SmokeConstants, gSmokeConstants, 0, NRI_SMOKE_SET_ROOT);

#endif
