#pragma once

#include "nri_scene_texture_slot_table.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <vector>

struct NRISceneMaterialTextureSlotResolveStats
{
	uint32_t materialRows = 0;
	uint32_t requiredFallbacks = 0;
	uint32_t optionalFallbacks = 0;
};

void NRIResolveSceneMaterialTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& gpuMaterials,
	NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);

void NRIResolveSceneMaterialBridgeTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	nri_scene::MaterialBridgeData& materials,
	uint32_t firstMaterialRow,
	NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);
