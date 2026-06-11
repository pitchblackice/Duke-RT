#include "nri_sky_environment.h"

#include <utility>

NRITextureResource* NRISkyEnvironment::ActiveTexture()
{
	return mActiveIndex < mCache.size() ? &mCache[mActiveIndex].resource : nullptr;
}

const NRITextureResource* NRISkyEnvironment::ActiveTexture() const
{
	return mActiveIndex < mCache.size() ? &mCache[mActiveIndex].resource : nullptr;
}

uint32_t NRISkyEnvironment::FindCachedTexture(uint64_t key, uint32_t width, uint32_t height) const
{
	for (uint32_t i = 0; i < (uint32_t)mCache.size(); ++i)
	{
		const NRICachedSkyTexture& cached = mCache[i];
		if (cached.key == key &&
			cached.resource.width == width &&
			cached.resource.height == height)
		{
			return i;
		}
	}

	return UINT32_MAX;
}

uint32_t NRISkyEnvironment::AddCachedTexture(NRICachedSkyTexture&& texture)
{
	mCache.push_back(std::move(texture));
	return (uint32_t)mCache.size() - 1;
}

void NRISkyEnvironment::Activate(uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode, float brightness)
{
	mActiveIndex = index;
	mActiveKey = key;
	mActiveState.mode = mode;
	mActiveState.sourceType = sourceView.sky.sourceType;
	mActiveState.texture = sourceView.sky.texture;
	mActiveState.faceMask = sourceView.sky.faceMask;
	mActiveState.brightness = brightness;
	mActiveState.flipTop = sourceView.sky.flipTop;
}

void NRISkyEnvironment::ResetActiveForLevel(MapRecord* level)
{
	mActiveIndex = UINT32_MAX;
	mActiveKey = 0;
	mActiveState = {};
	mLevel = level;
}

void NRISkyEnvironment::ResetTrace()
{
	mLastTracedState = {};
	mLastTracedResolvedKey = 0;
	mHasTracedState = false;
}

void NRISkyEnvironment::ClearCache()
{
	mCache.clear();
	mActiveIndex = UINT32_MAX;
	mActiveKey = 0;
	mLevel = nullptr;
	mActiveState = {};
	ResetTrace();
}
