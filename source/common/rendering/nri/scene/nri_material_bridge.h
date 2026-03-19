#pragma once

#include "nri_scene_bridge.h"

#include <vector>

namespace nri_scene
{
struct MaterialData
{
	uint32_t textureIndex = 0;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t reserved = 0;
	float lightLevel = 1.0f;
	float alpha = 1.0f;
	float reserved1 = 0.0f;
	float reserved2 = 0.0f;
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
	std::vector<TextureUpload> textures;
	std::vector<uint8_t> paletteLookup;
	uint32_t paletteWidth = 256;
	uint32_t paletteHeight = 256;
};

void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials);
}
