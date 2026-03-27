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
};

enum MaterialEmissiveMode : uint32_t
{
	MaterialEmissiveMode_None = 0,
	MaterialEmissiveMode_UseAlbedo = 1,
	MaterialEmissiveMode_UseConstantColor = 2,
};

struct MaterialData
{
	uint32_t textureIndex = 0;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t materialClass = 0;
	float lightLevel = 1.0f;
	float alpha = 1.0f;
	float roughnessHint = 0.45f;
	float metalnessHint = 0.0f;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	float emissiveReserved[2] = {};
};

struct MaterialLightingMetadata
{
	FGameTexture* texture = nullptr;
	uint64_t materialKey = 0;
	uint64_t textureContentKey = 0;
	uint64_t glowmapContentKey = 0;
	uint32_t textureId = 0;
	uint32_t paletteIndex = 0;
	uint32_t materialFlags = 0;
	uint32_t lightingFlags = 0;
	uint32_t materialClass = 0;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	int32_t shade = 0;
	float alpha = 1.0f;
	float lightLevel = 1.0f;
	float averageColor[3] = { 1.0f, 1.0f, 1.0f };
	float glowColor[3] = {};
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
};

struct TextureUpload
{
	uint64_t key = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool indexed = false;
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
}
