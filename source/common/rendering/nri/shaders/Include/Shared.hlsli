#ifndef RAZE_NRI_PT_SHARED_HLSLI
#define RAZE_NRI_PT_SHARED_HLSLI

#include "NRI.hlsl"
#include "NRD.hlsli"

#define SET_SAMPLERS 0
#define SET_SCENE_TEXTURES 1
#define SET_SCENE_DATA 2
#define SET_INPUTS 3
#define SET_OUTPUTS 4
#define SET_ROOT 5

#define MAX_SCENE_TEXTURES 256
#define NRI_FLAG_USE_JITTER 0x40u
#define NRI_TAA_JITTER_PHASE_COUNT 8u

#define MATERIAL_FLAG_INDEXED 1
#define MATERIAL_FLAG_FULLBRIGHT 2
#define MATERIAL_FLAG_FLAT 4
#define MATERIAL_FLAG_SPRITE 8
#define MATERIAL_FLAG_MIRROR 16
#define MATERIAL_FLAG_SKY 32
#define MATERIAL_FLAG_PORTAL 64
#define MATERIAL_FLAG_ONE_WAY 128

struct NRITraceConstants
{
	float3 CameraPos;
	uint RenderWidth;
	float3 CameraForward;
	uint RenderHeight;
	float3 CameraRight;
	float TanHalfFovX;
	float3 CameraUp;
	float TanHalfFovY;
	float3 PrevCameraPos;
	uint DisplayWidth;
	float3 PrevCameraForward;
	uint DisplayHeight;
	float3 PrevCameraRight;
	float PrevTanHalfFovX;
	float3 PrevCameraUp;
	float PrevTanHalfFovY;
	float3 LightDirection;
	uint SceneInstanceCount;
	float3 SkyColor;
	uint DebugMode;
	float3 GroundColor;
	uint StaticPrimitiveCount;
	uint FrameIndex;
	uint DynamicPrimitiveCount;
	uint Flags;
	uint StaticMaterialCount;
	uint BootstrapMode;
	uint DynamicMaterialCount;
	uint BounceCounts;
	uint PortalCount;
	uint RuntimeLightCount;
	uint PortalDepth;
	uint ReservedTrace0;
	uint ReservedTrace1;
};

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
};

struct MaterialData
{
	uint textureIndex;
	uint paletteIndex;
	uint flags;
	uint materialClass;
	float lightLevel;
	float alpha;
	float roughnessHint;
	float metalnessHint;
	float3 emissiveColor;
	float emissiveIntensity;
	float emissiveMaskScale;
	uint emissiveMode;
	uint sectorIndex;
	uint emissiveTextureIndex;
	float emissiveReserved;
};

struct SceneInstanceData
{
	uint primitiveOffset;
	uint dataSource;
	uint reserved0;
	uint reserved1;
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
};

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

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, SET_ROOT);

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

	const uint sampleIndex = (frameIndex % NRI_TAA_JITTER_PHASE_COUNT) + 1u;
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

SamplerState gLinearWrap : register(s0, space0);
SamplerState gLinearClamp : register(s1, space0);
SamplerState gPointWrap : register(s2, space0);
SamplerState gPointClamp : register(s3, space0);
Texture2D<float4> gPaletteLookup : register(t0, space1);
TextureCube<float4> gSkyTexture : register(t1, space1);
Texture2D<float4> gSceneTextures[MAX_SCENE_TEXTURES] : register(t2, space1);

Texture2D<float4> gHistoryInput : register(t0, space3);
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

#endif
