#pragma once

#include "nri_scene_bridge.h"

#include <vector>

namespace nri_scene
{
enum MaterialLightingFlags : uint32_t
{
	MaterialLightingFlag_None = 0,
	MaterialLightingFlag_MaterialFullbright = 1u << 0,
	MaterialLightingFlag_TextureFullbright = 1u << 1,
	MaterialLightingFlag_TextureGlowing = 1u << 2,
	MaterialLightingFlag_TextureAutoGlowing = 1u << 3,
	MaterialLightingFlag_HasGlowmap = 1u << 4,
	MaterialLightingFlag_NoShadowReceive = 1u << 5,
	MaterialLightingFlag_NoShadowCast = 1u << 6,
};

enum MaterialEmissiveMode : uint32_t
{
	MaterialEmissiveMode_None = 0,
	MaterialEmissiveMode_UseBaseTexture = 1,
	MaterialEmissiveMode_UseConstantColor = 2,
	MaterialEmissiveMode_UseGlowmapTexture = 3,
	MaterialEmissiveMode_UseAlbedo = MaterialEmissiveMode_UseBaseTexture,
};

struct MaterialData
{
	uint32_t textureIndex = 0;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t materialClass = 0;
	uint32_t lightingFlags = 0;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t sectorIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float lightLevel = 1.0f;
	float alpha = 1.0f;
	float roughnessHint = 0.45f;
	float metalnessHint = 0.0f;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	float emissiveReserved = 0.0f;
};

struct MaterialLightingMetadata
{
	FGameTexture* texture = nullptr;
	uint64_t materialKey = 0;
	uint64_t textureContentKey = 0;
	uint64_t glowmapContentKey = 0;
	uint64_t normalContentKey = 0;
	uint64_t metallicContentKey = 0;
	uint64_t roughnessContentKey = 0;
	uint32_t textureId = 0;
	uint32_t baseTextureId = 0;
	uint32_t textureIndex = 0;
	uint32_t glowmapTextureIndex = UINT32_MAX;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	uint32_t paletteIndex = 0;
	uint32_t materialFlags = 0;
	uint32_t lightingFlags = 0;
	uint32_t materialClass = 0;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	SurfaceSourceType sourceType = {};
	int32_t sectorIndex = -1;
	int32_t actorIndex = -1;
	int32_t shade = 0;
	float alpha = 1.0f;
	float lightLevel = 1.0f;
	float averageColor[3] = { 1.0f, 1.0f, 1.0f };
	float glowColor[3] = {};
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	float visibleFullbrightBoost = 1.0f;
};

struct TextureUpload
{
	uint64_t key = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool indexed = false;
	FTexture* sourceTexture = nullptr;
	std::vector<uint8_t> pixels;
};

struct MaterialBridgeData
{
	std::vector<MaterialData> materials;
	std::vector<MaterialLightingMetadata> lightMetadata;
	std::vector<TextureUpload> textures;
	std::vector<uint8_t> paletteLookup;
	uint32_t paletteWidth = 256;
	uint32_t paletteHeight = 256;
};

void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials);
bool RealizeTextureUploadPayload(const TextureUpload& upload, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
}
