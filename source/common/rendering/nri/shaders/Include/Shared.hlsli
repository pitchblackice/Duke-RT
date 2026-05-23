#ifndef RAZE_NRI_PT_SHARED_HLSLI
#define RAZE_NRI_PT_SHARED_HLSLI

#include "NRI.hlsl"
#include "NRD.hlsli"
#include "TraceConstants.hlsli"

#define SET_SAMPLERS 0
#define SET_SCENE_TEXTURES 1
#define SET_SCENE_DATA 2
#define SET_INPUTS 3
#define SET_OUTPUTS 4
#define SET_ROOT 5

#define MAX_SCENE_TEXTURES 512
#define NRI_FLAG_USE_JITTER 0x40u
#define NRI_JITTER_PHASE_SHIFT 16u
#define NRI_FLAG_FAST_EMISSIVE_SHADOW 0x100u
#define NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS 0x200u
#define NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW 0x400u
#define NRI_FLAG_TRACE_SHADER_STATS 0x800u
#define NRI_TAA_JITTER_PHASE_COUNT 8u

#define MATERIAL_FLAG_INDEXED 1
#define MATERIAL_FLAG_FULLBRIGHT 2
#define MATERIAL_FLAG_FLAT 4
#define MATERIAL_FLAG_SPRITE 8
#define MATERIAL_FLAG_MIRROR 16
#define MATERIAL_FLAG_SKY 32
#define MATERIAL_FLAG_PORTAL 64
#define MATERIAL_FLAG_ONE_WAY 128
#define MATERIAL_FLAG_ALPHA_CLIP 256
#define MATERIAL_FLAG_FACING_BILLBOARD 512
#define MATERIAL_FLAG_POINT_SAMPLED 1024
#define PRIMITIVE_FLAG_REFLECTION_ONLY 65536u

#define MATERIAL_LIGHTING_FLAG_NO_SHADOW_RECEIVE 32
#define MATERIAL_LIGHTING_FLAG_NO_SHADOW_CAST 64

struct SceneVertex
{
	float3 position;
	float3 prevPosition;
	float2 uv;
};

struct PrimitiveData
{
	uint3 indices;
	uint materialIndex;
	float2 uv0;
	float2 uv1;
	float2 uv2;
	float3 normal;
	uint flags;
	uint portalIndex;
	uint reserved0;
};

struct MaterialData
{
	uint textureIndex;
	uint paletteIndex;
	uint flags;
	uint materialClass;
	uint lightingFlags;
	uint normalTextureIndex;
	uint metallicTextureIndex;
	uint roughnessTextureIndex;
	uint sectorIndex;
	uint emissiveTextureIndex;
	float lightLevel;
	float alpha;
	float roughnessHint;
	float metalnessHint;
	float3 emissiveColor;
	float emissiveIntensity;
	float emissiveMaskScale;
	uint emissiveMode;
	float emissiveReserved;
};

struct SceneInstanceData
{
	uint primitiveOffset;
	uint dataSource;
	uint reserved0;
	uint reserved1;
	uint visibilityChunk;
	uint3 reserved2;
	float4 currentTransformRow0;
	float4 currentTransformRow1;
	float4 currentTransformRow2;
	float4 previousTransformRow0;
	float4 previousTransformRow1;
	float4 previousTransformRow2;
};

struct PortalData
{
	uint traversalClass;
	uint kind;
	uint targetLocalSpaceIndex;
	uint flags;
	float3 delta;
	uint reserved0;
};

struct RuntimePointLightData
{
	float3 position;
	float radius;
	float3 color;
	float intensity;
	uint flags;
	uint3 reserved;
};

#define RUNTIME_POINT_LIGHT_FLAG_CASTS_SHADOW 0x1u

struct RuntimeLightTileHeaderData
{
	uint indexOffset;
	uint indexCount;
};

struct EmissivePrimitiveHeaderData
{
	uint activeCount;
	uint dominantIndex;
	uint flags;
	float totalPower;
};

struct EmissivePrimitiveData
{
	uint dataSource;
	uint primitiveIndex;
	uint sourceFlags;
	uint textureId;
	float primitiveArea;
	float powerEstimate;
	float selectionWeight;
	float selectionPdf;
	uint stableKeyLo;
	uint stableKeyHi;
};

struct SectorLightHeaderData
{
	uint sectorCount;
	uint activeCount;
	uint pulsingCount;
	uint flags;
};

struct SectorLightData
{
	float3 ambientColor;
	float ambientIntensity;
	float3 hemisphereColor;
	float hemisphereAmount;
	float fogAmount;
	float pulseScale;
	uint sourceFlags;
	int paletteIndex;
	int lotag;
	int hitag;
};

struct ReprojectionData
{
	float4 currentViewToClipMatrix[4];
	float4 previousViewToClipMatrix[4];
	float4 currentWorldToViewMatrix[4];
	float4 previousWorldToViewMatrix[4];
};

NRI_ROOT_CONSTANTS(NRITraceSceneConstants, gTraceConstants, 0, SET_ROOT);

float3 GetDirectionalPlaceholderColor()
{
	const uint packed = gTraceConstants.BounceCounts >> 8u;
	return float3(
		(float)(packed & 0xffu),
		(float)((packed >> 8u) & 0xffu),
		(float)((packed >> 16u) & 0xffu)) * (8.0 / 255.0);
}

float GetDirectionalPlaceholderAngularSize()
{
	const float normalized = (float)((gTraceConstants.ReservedTrace1 >> 16u) & 0xffffu) * (1.0 / 65535.0);
	return max(normalized * 1.2, 1e-4);
}

float GetDirectionalPlaceholderTanAngularSize()
{
	return tan(GetDirectionalPlaceholderAngularSize());
}

float GetTemporalHaltonSample(uint index, uint base)
{
	float inverseBase = 1.0 / float(base);
	float fraction = inverseBase;
	float result = 0.0;

	while (index > 0u)
	{
		result += fraction * float(index % base);
		index /= base;
		fraction *= inverseBase;
	}

	return result;
}

float2 GetTemporalJitterForFrame(uint frameIndex)
{
	if ((gTraceConstants.Flags & NRI_FLAG_USE_JITTER) == 0u)
	{
		return 0.0;
	}

	const uint phaseCount = max((gTraceConstants.Flags >> NRI_JITTER_PHASE_SHIFT) & 0xffu, 1u);
	const uint sampleIndex = (frameIndex % phaseCount) + 1u;
	return float2(
		GetTemporalHaltonSample(sampleIndex, 2u) - 0.5,
		GetTemporalHaltonSample(sampleIndex, 3u) - 0.5);
}

float2 GetCurrentTemporalJitter()
{
	return GetTemporalJitterForFrame(gTraceConstants.FrameIndex);
}

float2 GetPreviousTemporalJitter()
{
	const uint previousFrameIndex = gTraceConstants.FrameIndex > 0u ? gTraceConstants.FrameIndex - 1u : 0u;
	return GetTemporalJitterForFrame(previousFrameIndex);
}

RaytracingAccelerationStructure gWorldTlas : register(t0, space5);
StructuredBuffer<SceneVertex> gStaticVertices : register(t0, space2);
StructuredBuffer<uint> gStaticIndices : register(t1, space2);
StructuredBuffer<PrimitiveData> gStaticPrimitives : register(t2, space2);
StructuredBuffer<MaterialData> gStaticMaterials : register(t3, space2);
StructuredBuffer<SceneVertex> gDynamicVertices : register(t4, space2);
StructuredBuffer<uint> gDynamicIndices : register(t5, space2);
StructuredBuffer<PrimitiveData> gDynamicPrimitives : register(t6, space2);
StructuredBuffer<MaterialData> gDynamicMaterials : register(t7, space2);
StructuredBuffer<SceneInstanceData> gSceneInstances : register(t8, space2);
StructuredBuffer<PortalData> gScenePortals : register(t9, space2);
StructuredBuffer<RuntimePointLightData> gRuntimePointLights : register(t10, space2);
StructuredBuffer<RuntimeLightTileHeaderData> gRuntimeLightTileHeaders : register(t11, space2);
StructuredBuffer<uint> gRuntimeLightTileIndices : register(t12, space2);
StructuredBuffer<EmissivePrimitiveHeaderData> gEmissivePrimitiveHeaders : register(t13, space2);
StructuredBuffer<EmissivePrimitiveData> gEmissivePrimitives : register(t14, space2);
StructuredBuffer<float> gEmissivePrimitiveCdf : register(t15, space2);
StructuredBuffer<SectorLightHeaderData> gSectorLightHeaders : register(t16, space2);
StructuredBuffer<SectorLightData> gSectorLights : register(t17, space2);
StructuredBuffer<ReprojectionData> gReprojectionDataBuffer : register(t18, space2);
StructuredBuffer<uint> gVisibleChunkWords : register(t19, space2);
StructuredBuffer<uint> gVisibleFlatPlaneWords : register(t20, space2);
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
StructuredBuffer<SceneVertex> gPersistentVoxelVertices : register(t21, space2);
StructuredBuffer<uint> gPersistentVoxelIndices : register(t22, space2);
StructuredBuffer<PrimitiveData> gPersistentVoxelPrimitives : register(t23, space2);
StructuredBuffer<MaterialData> gPersistentVoxelMaterials : register(t24, space2);
#endif

SamplerState gLinearWrap : register(s0, space0);
SamplerState gLinearClamp : register(s1, space0);
SamplerState gPointWrap : register(s2, space0);
SamplerState gPointClamp : register(s3, space0);
Texture2D<float4> gPaletteLookup : register(t0, space1);
TextureCube<float4> gSkyTexture : register(t1, space1);
Texture2D<float4> gSceneTextures[MAX_SCENE_TEXTURES] : register(t2, space1);

Texture2D<float4> gHistoryInput : register(t0, space3);
// PT motion contract shared by TraceOpaque, NRD, TAA, and the upscaler:
// - xy: screen-space motion in pixel units, excluding temporal jitter, with oldUv = newUv + motion.xy / renderSize
// - z: 2.5D depth delta, viewZPrev - viewZ
// - w: Raze-local history/validity metadata. Positive values allow local history consumers to reproject;
//   current hit paths store current viewZ here, while bootstrap/miss paths write a negative sentinel.
// - NRD consumes xyz only and is configured in screen-space 2.5D mode through motionVectorScale.
// Target policy during the MV rebuild:
// - opaque hits and sky/miss paths should both eventually follow the same canonical reprojection story
// - sky/background should not remain on unconditional zero motion once the dedicated miss path lands
Texture2D<float4> gMotionInput : register(t1, space3);
Texture2D<float4> gViewZInput : register(t2, space3);
Texture2D<float4> gNormalRoughnessInput : register(t3, space3);
Texture2D<float4> gBaseColorInput : register(t4, space3);
Texture2D<float4> gComposedInput : register(t5, space3);
Texture2D<float4> gUpscaledInput : register(t6, space3);
Texture2D<float4> gValidationInput : register(t7, space3);
Texture2D<float4> gGuideDiffuseInput : register(t8, space3);
Texture2D<float4> gGuideSpecularInput : register(t9, space3);
Texture2D<float4> gGuideSpecHitInput : register(t10, space3);
Texture2D<float4> gShadowInput : register(t11, space3);
Texture2D<float4> gDirectLightingInput : register(t12, space3);
Texture2D<float4> gDirectEmissionInput : register(t13, space3);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gTraceOutput, u, 0, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gComposedOutput, u, 1, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gFinalOutput, u, 2, SET_OUTPUTS);
// See gMotionInput above for the authoritative PT motion-buffer contract.
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gMotionOutput, u, 3, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gViewZOutput, u, 4, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gNormalRoughnessOutput, u, 5, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gBaseColorOutput, u, 6, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 7, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gUpscaledOutput, u, 8, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideDiffuseOutput, u, 9, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideSpecularOutput, u, 10, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideSpecHitOutput, u, 11, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gShadowPenumbraOutput, u, 12, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gDirectLightingOutput, u, 13, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gDirectEmissionOutput, u, 14, SET_OUTPUTS);
RWStructuredBuffer<uint> gTraceShaderStats : register(u15, space4);

#endif
