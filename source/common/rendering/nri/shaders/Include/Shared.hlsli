#ifndef RAZE_NRI_PT_SHARED_HLSLI
#define RAZE_NRI_PT_SHARED_HLSLI

#include "NRI.hlsl"
#include "NRD.hlsli"

#define SET_SAMPLERS 0
#define SET_SCENE_TEXTURES 1
#define SET_INPUTS 2
#define SET_OUTPUTS 3
#define SET_ROOT 4

#define MAX_SCENE_TEXTURES 256

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
	uint PrimitiveCount;
	float3 SkyColor;
	uint DebugMode;
	float3 GroundColor;
	uint MaterialCount;
	uint FrameIndex;
	uint Flags;
	uint BootstrapMode;
	uint BounceCounts;
	uint InstanceDataCount;
};

struct InstanceData
{
	uint primitiveOffset;
	uint flags;
	uint reserved0;
	uint reserved1;
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
};

struct MaterialData
{
	uint textureIndex;
	uint paletteIndex;
	uint flags;
	uint reserved;
	float lightLevel;
	float alpha;
	float reserved1;
	float reserved2;
};

NRI_ROOT_CONSTANTS(NRITraceConstants, gTraceConstants, 0, SET_ROOT);

RaytracingAccelerationStructure gWorldTlas : register(t0, space4);
StructuredBuffer<SceneVertex> gVertices : register(t1, space4);
StructuredBuffer<uint> gIndices : register(t2, space4);
StructuredBuffer<PrimitiveData> gPrimitives : register(t3, space4);
StructuredBuffer<MaterialData> gMaterials : register(t4, space4);
StructuredBuffer<InstanceData> gInstanceData : register(t5, space4);

SamplerState gLinearWrap : register(s0, space0);
SamplerState gLinearClamp : register(s1, space0);
SamplerState gPointWrap : register(s2, space0);
SamplerState gPointClamp : register(s3, space0);
Texture2D<float4> gPaletteLookup : register(t0, space1);
TextureCube<float4> gSkyTexture : register(t1, space1);
Texture2D<float4> gSceneTextures[MAX_SCENE_TEXTURES] : register(t2, space1);

Texture2D<float4> gHistoryInput : register(t0, space2);
Texture2D<float4> gMotionInput : register(t1, space2);
Texture2D<float4> gViewZInput : register(t2, space2);
Texture2D<float4> gNormalRoughnessInput : register(t3, space2);
Texture2D<float4> gBaseColorInput : register(t4, space2);
Texture2D<float4> gComposedInput : register(t5, space2);
Texture2D<float4> gUpscaledInput : register(t6, space2);
Texture2D<float4> gValidationInput : register(t7, space2);
Texture2D<float4> gGuideDiffuseInput : register(t8, space2);
Texture2D<float4> gGuideSpecularInput : register(t9, space2);
Texture2D<float4> gGuideSpecHitInput : register(t10, space2);

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

#endif
