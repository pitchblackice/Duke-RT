#pragma once

#include "../scene/nri_scene_surface_types.h"

#include <cstdint>

struct NRISurfaceProbeFrameState
{
	bool valid = false;
	bool usesStaticMapScene = false;
	uint32_t staticPrimitiveCount = 0;
	uint32_t runtimeSpaceLinkPrimitiveCount = 0;
	uint32_t runtimeMutationPrimitiveCount = 0;
	uint32_t dynamicPrimitiveCount = 0;
};

struct NRISurfaceProbeResult
{
	bool valid = false;
	bool hit = false;
	uint32_t sceneDataSource = UINT32_MAX;
	uint32_t sceneOwner = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	uint32_t materialIndex = UINT32_MAX;
	uint32_t primitiveFlags = 0;
	uint32_t materialLightingFlags = 0;
	uint32_t textureId = 0;
	uint32_t baseTextureId = 0;
	uint32_t materialClass = 0;
	uint32_t emissiveMode = 0;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	float lightLevel = 0.0f;
	float alpha = 1.0f;
	float metalnessHint = 0.0f;
	float roughnessHint = 0.45f;
	float averageColor[3] = { 1.0f, 1.0f, 1.0f };
	float emissiveColor[3] = {};
	float glowColor[3] = {};
	float distance = 0.0f;
	float position[3] = {};
	float normal[3] = {};
	nri_scene::SurfaceProvenance provenance = {};
};

struct NRISurfaceProbeEmissiveDiagnostics
{
	bool sceneLightSurfaceMatch = false;
	bool activeEmissiveSurfaceMatch = false;
	bool exactEmissivePrimitiveMatch = false;
	uint32_t sceneLightMaterialIndex = UINT32_MAX;
	uint32_t emissivePrimitiveMatchCount = 0;
	uint32_t emissiveSourceFlags = 0;
	uint32_t emissiveSourceRuleId = 0;
	uint32_t emissiveOverrideRuleId = 0;
	int32_t emissiveSectorIndex = -1;
	float emissivePrimitiveArea = 0.0f;
	float emissivePowerEstimate = 0.0f;
	float emissiveSelectionWeight = 0.0f;
	float emissiveSelectionPdf = 0.0f;
	float emissiveIntensity = 0.0f;
	float sectorResponseScale = 1.0f;
	float sectorReachScale = 1.0f;
	bool sectorResponseApplied = false;
	bool materialResponseEnabled = false;
	float materialResponseScale = 1.0f;
};

class NRISurfaceProbeTracker
{
public:
	void Reset()
	{
		mLast = {};
		mLastLogged = {};
	}

	const NRISurfaceProbeResult& Last() const { return mLast; }
	const NRISurfaceProbeResult& LastLogged() const { return mLastLogged; }
	void SetLast(const NRISurfaceProbeResult& result) { mLast = result; }
	void SetLastLogged(const NRISurfaceProbeResult& result) { mLastLogged = result; }

private:
	NRISurfaceProbeResult mLast = {};
	NRISurfaceProbeResult mLastLogged = {};
};
