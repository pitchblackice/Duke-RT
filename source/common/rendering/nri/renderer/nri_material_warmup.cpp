#include "nri_renderer.h"
#include "nri_cvars.h"

#include "printf.h"

#include <chrono>

namespace
{
	double MaterialWarmupDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	void CopyMaterialWarmupStats(const NRIMaterialTextureWarmupResult& source, NRIPersistentVoxelMaterialWarmupStats& destination)
	{
		destination.textureRequests = source.textureRequests;
		destination.textureHits = source.textureHits;
		destination.textureMisses = source.textureMisses;
		destination.textureInserts = source.textureInserts;
		destination.estimatedBytes = source.estimatedBytes;
		destination.realizeMs = source.realizeMs;
	}
}

bool NRIRenderer::PreloadMaterialResources()
{
	NRIPersistentVoxelMaterialWarmupServices voxelWarmupServices = {};
	voxelWarmupServices.user = this;
	voxelWarmupServices.ensurePalette = [](void* user, const nri_scene::MaterialBridgeData& materials) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsurePaletteTexture(materials);
	};
	voxelWarmupServices.warmTextures = [](void* user, const nri_scene::MaterialBridgeData& materials, NRIPersistentVoxelMaterialWarmupStats& stats) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer == nullptr)
		{
			return false;
		}

		NRIMaterialTextureWarmupResult result = {};
		if (!renderer->mSceneTextures.WarmMaterialTextures(*renderer->mFrameBuffer, materials, result))
		{
			return false;
		}
		CopyMaterialWarmupStats(result, stats);
		return true;
	};

	const auto start = std::chrono::steady_clock::now();
	NRIMaterialTextureWarmupResult staticStats = {};
	NRIPersistentVoxelMaterialWarmupResult voxelWarmup = {};
	const bool hasStaticMaterials = mStaticMapScene.valid && !mStaticMapScene.materialBridge.materials.empty();
	bool paletteReady = true;
	if (hasStaticMaterials)
	{
		paletteReady = EnsurePaletteTexture(mStaticMapScene.materialBridge);
		if (!paletteReady ||
			mFrameBuffer == nullptr ||
			!mSceneTextures.WarmMaterialTextures(*mFrameBuffer, mStaticMapScene.materialBridge, staticStats))
		{
			return false;
		}
	}
	if (!mPersistentVoxels.WarmMaterialResources(voxelWarmupServices, voxelWarmup))
	{
		return false;
	}
	paletteReady = paletteReady && voxelWarmup.paletteReady;

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading material: event=warm palette=%u static_materials=%u static_textures=%u static_hits=%u static_misses=%u static_inserts=%u static_bytes=%llu voxel_materials=%u voxel_variants=%u voxel_textures=%u voxel_hits=%u voxel_misses=%u voxel_inserts=%u voxel_bytes=%llu cache=%u realize_ms=%.3f ms=%.3f\n",
			paletteReady ? 1u : 0u,
			hasStaticMaterials ? (uint32_t)mStaticMapScene.materialBridge.materials.size() : 0u,
			staticStats.textureRequests,
			staticStats.textureHits,
			staticStats.textureMisses,
			staticStats.textureInserts,
			(unsigned long long)staticStats.estimatedBytes,
			voxelWarmup.materialCount,
			voxelWarmup.variantResourceCount,
			voxelWarmup.textureStats.textureRequests,
			voxelWarmup.textureStats.textureHits,
			voxelWarmup.textureStats.textureMisses,
			voxelWarmup.textureStats.textureInserts,
			(unsigned long long)voxelWarmup.textureStats.estimatedBytes,
			mSceneTextures.CacheCount(),
			staticStats.realizeMs + voxelWarmup.textureStats.realizeMs,
			MaterialWarmupDurationMs(start, std::chrono::steady_clock::now()));
	}
	return true;
}
