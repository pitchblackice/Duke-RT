#include "nri_scene_textures.h"

#include <utility>

uint32_t NRISceneTextureResidency::FindCacheIndex(uint64_t key) const
{
	const auto it = mTextureCacheKeyIndex.find(key);
	if (it == mTextureCacheKeyIndex.end())
	{
		return UINT32_MAX;
	}

	const uint32_t cacheIndex = it->second;
	if (cacheIndex >= mTextureCache.size() || mTextureCache[cacheIndex].key != key)
	{
		return UINT32_MAX;
	}

	return cacheIndex;
}

uint32_t NRISceneTextureResidency::AddCachedTexture(NRISceneCachedTexture&& texture)
{
	const uint32_t cacheIndex = (uint32_t)mTextureCache.size();
	mTextureCacheKeyIndex[texture.key] = cacheIndex;
	mTextureCache.push_back(std::move(texture));
	return cacheIndex;
}

void NRISceneTextureResidency::ClearLiveResources()
{
	mLiveResources.clear();
}

void NRISceneTextureResidency::TrackLiveResource(NRITextureResource& resource)
{
	if (resource.texture == nullptr)
	{
		return;
	}

	for (NRITextureResource* existing : mLiveResources)
	{
		if (existing == &resource)
		{
			return;
		}
	}

	mLiveResources.push_back(&resource);
}

void NRISceneTextureResidency::ClearCachedTextures()
{
	mTextureCache.clear();
	mTextureCacheKeyIndex.clear();
	mLiveResources.clear();
}
