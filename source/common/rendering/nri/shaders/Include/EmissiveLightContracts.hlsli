#ifndef RAZE_NRI_EMISSIVE_LIGHT_CONTRACTS_HLSLI
#define RAZE_NRI_EMISSIVE_LIGHT_CONTRACTS_HLSLI

struct SceneVertex
{
	float3 position;
	float3 prevPosition;
	float2 uv;
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
	float emissionScale;
	uint stableKeyLo;
	uint stableKeyHi;
};

struct EmissiveMaterialResponseData
{
	uint dataSource;
	uint primitiveIndex;
	float materialScale;
	uint flags;
};

#endif
