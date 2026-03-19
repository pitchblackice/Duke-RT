#ifndef RAZE_NRI_PT_SHARED_HLSLI
#define RAZE_NRI_PT_SHARED_HLSLI

#include "NRI.hlsl"

#define SET_SAMPLERS 0
#define SET_SCENE_TEXTURES 1
#define SET_OUTPUTS 2
#define SET_ROOT 4

#define MAX_SCENE_TEXTURES 256

#define MATERIAL_FLAG_INDEXED 1
#define MATERIAL_FLAG_FULLBRIGHT 2

struct NRITraceConstants
{
	float3 CameraPos;
	uint OutputWidth;
	float3 CameraForward;
	uint OutputHeight;
	float3 CameraRight;
	float TanHalfFovX;
	float3 CameraUp;
	float TanHalfFovY;
	float3 LightDirection;
	uint PrimitiveCount;
	float3 SkyColor;
	uint DebugMode;
	float3 GroundColor;
	uint MaterialCount;
};

struct SceneVertex
{
	float3 position;
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

SamplerState gLinearClamp : register(s0, space0);
Texture2D<float4> gPaletteLookup : register(t0, space1);
Texture2D<float4> gSceneTextures[MAX_SCENE_TEXTURES] : register(t1, space1);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gTraceOutput, u, 0, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gComposedOutput, u, 1, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gFinalOutput, u, 2, SET_OUTPUTS);

#endif
