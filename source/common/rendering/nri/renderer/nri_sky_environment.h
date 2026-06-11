#pragma once

#include "../scene/nri_scene_bridge.h"
#include "../system/nri_local.h"

#include <chrono>
#include <cstdint>
#include <vector>

class FGameTexture;
struct MapRecord;

struct NRICachedSkyTexture
{
	uint64_t key = 0;
	nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
	NRITextureResource resource;
};

struct NRISkyState
{
	nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
	nri_scene::PTSkySourceType sourceType = nri_scene::PTSkySourceType::None;
	FGameTexture* texture = nullptr;
	uint32_t faceMask = 0;
	float brightness = 1.0f;
	bool flipTop = false;
};

struct NRIPreservedStaticMapSkyState
{
	bool valid = false;
	uint64_t buildSerial = 0;
	nri_scene::SceneView sceneView;
};

struct RendererSkyPerfTraceStats
{
	uint32_t ensureSceneTexturesCalls = 0;
	uint32_t ensureSceneTexturesPreserveTrueCalls = 0;
	uint32_t ensureSceneTexturesPreserveFalseCalls = 0;
	uint32_t ensureSkyCalls = 0;
	uint32_t preserveExistingHits = 0;
	uint32_t reuseActiveCubemapHits = 0;
	uint32_t probeAttempts = 0;
	uint32_t probeSuccesses = 0;
	uint32_t reuseActiveProbeHits = 0;
	uint32_t activateCachedCubemapHits = 0;
	uint32_t createCachedCubemapHits = 0;
	uint32_t keepLastCubemapHits = 0;
	uint32_t holdLevelCubemapHits = 0;
	uint32_t solidReuseHits = 0;
	uint32_t solidActivateHits = 0;
	uint32_t solidCreateHits = 0;
	uint32_t probeFaceCalls = 0;
	uint32_t buildCubemapUploadCalls = 0;
	uint32_t residentStaticSceneTextureBuilds = 0;
	uint32_t combinedOverlayTextureBuilds = 0;
	uint32_t lightingInvalidationRequests = 0;
	uint32_t lightingInvalidationsApplied = 0;
	uint32_t emissiveMaterialDirtyEvents = 0;
	uint64_t ensureSkyTimeUs = 0;
	uint64_t probeCubemapTimeUs = 0;
	uint64_t probeFaceTimeUs = 0;
	uint64_t buildCubemapUploadTimeUs = 0;
};

extern RendererSkyPerfTraceStats gRendererSkyPerfTraceStats;

bool ShouldTraceSkyPerf();
bool ShouldEmitRendererTemporalTraceLogs();
void ResetRendererSkyPerfTraceStats();
const char* GetSkyModeName(nri_scene::PTSkyMode mode);
const char* GetSkySourceTypeName(nri_scene::PTSkySourceType sourceType);
float GetSkyBrightnessMultiplier();
bool SkyBrightnessMatches(float a, float b);

class ScopedSkyPerfTimer
{
public:
	explicit ScopedSkyPerfTimer(uint64_t& targetUs);
	~ScopedSkyPerfTimer();

	ScopedSkyPerfTimer(const ScopedSkyPerfTimer&) = delete;
	ScopedSkyPerfTimer& operator=(const ScopedSkyPerfTimer&) = delete;

private:
	uint64_t* mTarget = nullptr;
	std::chrono::steady_clock::time_point mStart = {};
};

class NRISkyEnvironment
{
public:
	std::vector<NRICachedSkyTexture>& CachedTextures() { return mCache; }
	const std::vector<NRICachedSkyTexture>& CachedTextures() const { return mCache; }
	uint32_t CachedTextureCount() const { return (uint32_t)mCache.size(); }

	uint64_t& ActiveKey() { return mActiveKey; }
	uint64_t ActiveKey() const { return mActiveKey; }

	NRISkyState& ActiveState() { return mActiveState; }
	const NRISkyState& ActiveState() const { return mActiveState; }

	MapRecord*& Level() { return mLevel; }
	MapRecord* Level() const { return mLevel; }

	NRISkyState& LastTracedState() { return mLastTracedState; }
	const NRISkyState& LastTracedState() const { return mLastTracedState; }

	uint64_t& LastTracedResolvedKey() { return mLastTracedResolvedKey; }
	uint64_t LastTracedResolvedKey() const { return mLastTracedResolvedKey; }

	bool& HasTracedState() { return mHasTracedState; }
	bool HasTracedState() const { return mHasTracedState; }

	NRIPreservedStaticMapSkyState& PreservedStaticMapSky() { return mPreservedStaticMapSky; }
	const NRIPreservedStaticMapSkyState& PreservedStaticMapSky() const { return mPreservedStaticMapSky; }

	NRITextureResource* ActiveTexture();
	const NRITextureResource* ActiveTexture() const;
	uint32_t FindCachedTexture(uint64_t key, uint32_t width, uint32_t height) const;
	uint32_t AddCachedTexture(NRICachedSkyTexture&& texture);
	void Activate(uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode, float brightness);
	void ResetActiveForLevel(MapRecord* level);
	void ResetTrace();
	void ClearCache();

private:
	std::vector<NRICachedSkyTexture> mCache;
	uint64_t mActiveKey = 0;
	uint32_t mActiveIndex = UINT32_MAX;
	MapRecord* mLevel = nullptr;
	NRISkyState mActiveState = {};
	NRISkyState mLastTracedState = {};
	uint64_t mLastTracedResolvedKey = 0;
	bool mHasTracedState = false;
	NRIPreservedStaticMapSkyState mPreservedStaticMapSky = {};
};
