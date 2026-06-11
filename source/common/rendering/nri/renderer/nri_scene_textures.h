#pragma once

#include "../system/nri_local.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class NRIRenderDevice;

namespace nri_scene
{
	struct MaterialBridgeData;
	struct TextureUpload;
}

struct NRISceneCachedTexture
{
	uint64_t key = 0;
	NRITextureResource resource;
};

struct SceneTextureOverflowDebugStats
{
	uint32_t textureCountLastBuild = 0;
	uint32_t truncatedTextureCountLastBuild = 0;
	uint32_t baseTextureClampCountLastBuild = 0;
	uint32_t normalTextureClampCountLastBuild = 0;
	uint32_t metallicTextureClampCountLastBuild = 0;
	uint32_t roughnessTextureClampCountLastBuild = 0;
	uint32_t emissiveTextureClampCountLastBuild = 0;
	uint64_t totalOverflowBuilds = 0;
	bool warningLogged = false;
};

struct SceneTextureCacheDebugStats
{
	uint32_t cacheEntriesLastBuild = 0;
	uint32_t cacheEntriesHighWater = 0;
	uint32_t lookupMissesLastBuild = 0;
	uint32_t insertCountLastBuild = 0;
	uint32_t transitionCountLastFrame = 0;
	double lookupMsLastBuild = 0.0;
	double realizeMsLastBuild = 0.0;
	double descriptorMsLastBuild = 0.0;
	double transitionMsLastFrame = 0.0;
};

struct SceneTextureResolveResult
{
	nri::Descriptor* descriptor = nullptr;
	bool cacheMiss = false;
	bool inserted = false;
	bool activeCanvasSelfReference = false;
	double lookupMs = 0.0;
	double realizeMs = 0.0;
};

class NRISceneTextureResidency
{
public:
	NRITextureResource& PaletteTexture() { return mPaletteTexture; }
	const NRITextureResource& PaletteTexture() const { return mPaletteTexture; }

	std::vector<NRISceneCachedTexture>& CachedTextures() { return mTextureCache; }
	const std::vector<NRISceneCachedTexture>& CachedTextures() const { return mTextureCache; }

	SceneTextureOverflowDebugStats& OverflowStats() { return mOverflowStats; }
	const SceneTextureOverflowDebugStats& OverflowStats() const { return mOverflowStats; }

	SceneTextureCacheDebugStats& CacheStats() { return mCacheStats; }
	const SceneTextureCacheDebugStats& CacheStats() const { return mCacheStats; }

	bool& LimitLogPrinted() { return mLimitLogPrinted; }
	bool LimitLogPrinted() const { return mLimitLogPrinted; }

	uint32_t CacheCount() const { return (uint32_t)mTextureCache.size(); }
	uint32_t FindCacheIndex(uint64_t key) const;
	uint32_t AddCachedTexture(NRISceneCachedTexture&& texture);

	bool EnsurePaletteTexture(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials);
	bool EnsureCacheEntry(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, double* outRealizeMs = nullptr);
	bool ResolveTextureDescriptor(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, bool tracePerf, SceneTextureResolveResult& outResult);
	uint32_t TransitionInputsForCompute(NRIRenderDevice& device);

	void ClearLiveResources();
	void TrackLiveResource(NRITextureResource& resource);
	const std::vector<NRITextureResource*>& LiveResources() const { return mLiveResources; }

	void ClearCachedTextures();

private:
	NRITextureResource mPaletteTexture;
	std::vector<NRISceneCachedTexture> mTextureCache;
	std::unordered_map<uint64_t, uint32_t> mTextureCacheKeyIndex;
	std::vector<NRITextureResource*> mLiveResources;
	SceneTextureOverflowDebugStats mOverflowStats = {};
	SceneTextureCacheDebugStats mCacheStats = {};
	bool mLimitLogPrinted = false;
};
