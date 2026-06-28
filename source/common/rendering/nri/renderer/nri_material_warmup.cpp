#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"

#include "printf.h"

#include <algorithm>
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
		destination.pending = source.pending;
		destination.textureBudgetHit = source.textureBudgetHit;
		destination.byteBudgetHit = source.byteBudgetHit;
		destination.msBudgetHit = source.msBudgetHit;
	}

}

bool NRIRenderer::PreloadMaterialResources()
{
	mPreloadMaterialStatus = {};
	mPreloadMaterialStatus.paletteReady = true;
	mPreloadMaterialStatus.staticReady = true;
	mPreloadMaterialStatus.voxelReady = true;
	mPreloadMaterialStatus.preloadSubmits = mFrameBuffer != nullptr ? mFrameBuffer->GetPreloadSubmitCountThisTick() : 0u;
	mPreloadMaterialStatus.preloadSubmitLimit = mFrameBuffer != nullptr ? mFrameBuffer->GetPreloadSubmitLimitThisTick() : 0u;

	const NRIMaterialTextureWarmupOptions warmupOptions =
	{
		(uint32_t)std::max<int>(0, (int)nri_ptpreloadmaterialtexturespersubmit),
		(uint64_t)std::max<int>(0, (int)nri_ptpreloadmaterialbytespersubmit),
		(double)std::max<int>(0, (int)nri_ptpreloadmaterialmaxms)
	};
	auto updateSubmitStatus = [&]()
	{
		mPreloadMaterialStatus.preloadSubmits = mFrameBuffer != nullptr ? mFrameBuffer->GetPreloadSubmitCountThisTick() : 0u;
		mPreloadMaterialStatus.preloadSubmitLimit = mFrameBuffer != nullptr ? mFrameBuffer->GetPreloadSubmitLimitThisTick() : 0u;
		mPreloadMaterialStatus.submitBudgetHit = mPreloadMaterialStatus.submitBudgetHit ||
			(mFrameBuffer != nullptr && mFrameBuffer->IsPreloadSubmitBudgetHit());
	};
	auto submitMaterialChunk = [&](const char* reason) -> bool
	{
		if (mFrameBuffer == nullptr)
		{
			mPreloadMaterialStatus.failed = true;
			return false;
		}
		if (!mFrameBuffer->SubmitWaitAndRestartCommandList(reason))
		{
			updateSubmitStatus();
			if (mFrameBuffer->IsPreloadSubmitBudgetHit())
			{
				mPreloadMaterialStatus.pending = true;
				return false;
			}
			mPreloadMaterialStatus.failed = true;
			return false;
		}
		updateSubmitStatus();
		return true;
	};

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
		if (!renderer->mSceneTextures.WarmMaterialTexturesBudgeted(*renderer->mFrameBuffer, materials, NRIMaterialTextureWarmupOptions
			{
				(uint32_t)std::max<int>(0, (int)nri_ptpreloadmaterialtexturespersubmit),
				(uint64_t)std::max<int>(0, (int)nri_ptpreloadmaterialbytespersubmit),
				(double)std::max<int>(0, (int)nri_ptpreloadmaterialmaxms)
			},
			renderer->mVoxelPreloadMaterialCursor,
			result))
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
	const uint64_t staticSourceKey = hasStaticMaterials ? mStaticMapScene.buildSerial : 0ull;
	if (mStaticPreloadMaterialCursor.sourceKey != staticSourceKey)
	{
		mStaticPreloadMaterialCursor = {};
		mStaticPreloadMaterialCursor.sourceKey = staticSourceKey;
	}
	const uint64_t voxelSourceKey = mPersistentVoxels.BuildSceneGenerationHash();
	if (mVoxelPreloadMaterialCursor.sourceKey != voxelSourceKey)
	{
		mVoxelPreloadMaterialCursor = {};
		mVoxelPreloadMaterialCursor.sourceKey = voxelSourceKey;
	}
	auto traceMaterialWarmup = [&]()
	{
		if ((int)nri_ptloadingtrace < 1)
		{
			return;
		}
		Printf("NRI PT loading material: event=warm palette=%u pending=%u failed=%u static_materials=%u static_textures=%u static_ready=%u static_pending=%u static_hits=%u static_misses=%u static_inserts=%u static_bytes=%llu voxel_materials=%u voxel_variants=%u voxel_textures=%u voxel_ready=%u voxel_pending=%u voxel_hits=%u voxel_misses=%u voxel_inserts=%u voxel_bytes=%llu cache=%u realize_ms=%.3f preload_submits=%u preload_submit_limit=%u submit_budget_hit=%u texture_budget_hit=%u byte_budget_hit=%u ms_budget_hit=%u ms=%.3f\n",
			mPreloadMaterialStatus.paletteReady ? 1u : 0u,
			mPreloadMaterialStatus.pending ? 1u : 0u,
			mPreloadMaterialStatus.failed ? 1u : 0u,
			mPreloadMaterialStatus.staticMaterialCount,
			staticStats.textureRequests,
			mPreloadMaterialStatus.staticTexturesReady,
			mPreloadMaterialStatus.staticTexturesPending,
			staticStats.textureHits,
			staticStats.textureMisses,
			staticStats.textureInserts,
			(unsigned long long)staticStats.estimatedBytes,
			voxelWarmup.materialCount,
			voxelWarmup.variantResourceCount,
			voxelWarmup.textureStats.textureRequests,
			mPreloadMaterialStatus.voxelTexturesReady,
			mPreloadMaterialStatus.voxelTexturesPending,
			voxelWarmup.textureStats.textureHits,
			voxelWarmup.textureStats.textureMisses,
			voxelWarmup.textureStats.textureInserts,
			(unsigned long long)voxelWarmup.textureStats.estimatedBytes,
			mSceneTextures.CacheCount(),
			staticStats.realizeMs + voxelWarmup.textureStats.realizeMs,
			mPreloadMaterialStatus.preloadSubmits,
			mPreloadMaterialStatus.preloadSubmitLimit,
			mPreloadMaterialStatus.submitBudgetHit ? 1u : 0u,
			mPreloadMaterialStatus.textureBudgetHit ? 1u : 0u,
			mPreloadMaterialStatus.byteBudgetHit ? 1u : 0u,
			mPreloadMaterialStatus.msBudgetHit ? 1u : 0u,
			MaterialWarmupDurationMs(start, std::chrono::steady_clock::now()));
	};

	bool paletteReady = true;
	if (hasStaticMaterials)
	{
		mPreloadMaterialStatus.hasStaticMaterials = true;
		mPreloadMaterialStatus.staticMaterialCount = (uint32_t)mStaticMapScene.materialBridge.materials.size();
		paletteReady = EnsurePaletteTexture(mStaticMapScene.materialBridge);
		if (!paletteReady ||
			mFrameBuffer == nullptr ||
			!mSceneTextures.WarmMaterialTexturesBudgeted(*mFrameBuffer, mStaticMapScene.materialBridge, warmupOptions, mStaticPreloadMaterialCursor, staticStats))
		{
			mPreloadMaterialStatus.failed = true;
			mPreloadMaterialStatus.paletteReady = paletteReady;
			traceMaterialWarmup();
			return false;
		}
		mPreloadMaterialStatus.staticTexturesReady = mStaticPreloadMaterialCursor.nextTextureIndex;
		mPreloadMaterialStatus.staticTexturesPending =
			mStaticMapScene.materialBridge.textures.size() > mStaticPreloadMaterialCursor.nextTextureIndex ?
			(uint32_t)(mStaticMapScene.materialBridge.textures.size() - mStaticPreloadMaterialCursor.nextTextureIndex) : 0u;
		mPreloadMaterialStatus.staticTexturesRealized = staticStats.textureInserts;
		mPreloadMaterialStatus.staticUploadBytes = staticStats.estimatedBytes;
		mPreloadMaterialStatus.staticReady = !staticStats.pending;
		mPreloadMaterialStatus.textureBudgetHit = mPreloadMaterialStatus.textureBudgetHit || staticStats.textureBudgetHit;
		mPreloadMaterialStatus.byteBudgetHit = mPreloadMaterialStatus.byteBudgetHit || staticStats.byteBudgetHit;
		mPreloadMaterialStatus.msBudgetHit = mPreloadMaterialStatus.msBudgetHit || staticStats.msBudgetHit;
		if (staticStats.textureInserts != 0 && staticStats.pending)
		{
			if (!submitMaterialChunk("material-preload-static"))
			{
				mPreloadMaterialStatus.pending = true;
				traceMaterialWarmup();
				return false;
			}
		}
		if (staticStats.pending)
		{
			mPreloadMaterialStatus.pending = true;
			traceMaterialWarmup();
			return false;
		}
	}
	if (!mPersistentVoxels.WarmMaterialResources(voxelWarmupServices, voxelWarmup))
	{
		mPreloadMaterialStatus.failed = !voxelWarmup.pending;
		mPreloadMaterialStatus.pending = voxelWarmup.pending;
		traceMaterialWarmup();
		return false;
	}
	paletteReady = paletteReady && voxelWarmup.paletteReady;
	mPreloadMaterialStatus.hasVoxelMaterials = voxelWarmup.hasMaterials;
	mPreloadMaterialStatus.voxelMaterialCount = voxelWarmup.materialCount;
	mPreloadMaterialStatus.voxelVariantResourceCount = voxelWarmup.variantResourceCount;
	mPreloadMaterialStatus.voxelTexturesReady = mVoxelPreloadMaterialCursor.nextTextureIndex;
	mPreloadMaterialStatus.voxelTexturesPending =
		voxelWarmup.hasMaterials && mPersistentVoxels.OverlayMaterialCount() > 0 ?
		(voxelWarmup.textureStats.pending ? 1u : 0u) : 0u;
	mPreloadMaterialStatus.voxelTexturesRealized = voxelWarmup.textureStats.textureInserts;
	mPreloadMaterialStatus.voxelUploadBytes = voxelWarmup.textureStats.estimatedBytes;
	mPreloadMaterialStatus.voxelReady = !voxelWarmup.textureStats.pending;
	mPreloadMaterialStatus.textureBudgetHit = mPreloadMaterialStatus.textureBudgetHit || voxelWarmup.textureStats.textureBudgetHit;
	mPreloadMaterialStatus.byteBudgetHit = mPreloadMaterialStatus.byteBudgetHit || voxelWarmup.textureStats.byteBudgetHit;
	mPreloadMaterialStatus.msBudgetHit = mPreloadMaterialStatus.msBudgetHit || voxelWarmup.textureStats.msBudgetHit;
	if (voxelWarmup.textureStats.textureInserts != 0 && voxelWarmup.textureStats.pending)
	{
		if (!submitMaterialChunk("material-preload-voxel"))
		{
			mPreloadMaterialStatus.pending = true;
			traceMaterialWarmup();
			return false;
		}
	}
	if (voxelWarmup.textureStats.pending)
	{
		mPreloadMaterialStatus.pending = true;
		traceMaterialWarmup();
		return false;
	}
	updateSubmitStatus();
	mPreloadMaterialStatus.paletteReady = paletteReady;
	mPreloadMaterialStatus.realizeMs = staticStats.realizeMs + voxelWarmup.textureStats.realizeMs;
	mPreloadMaterialStatus.pending = !mPreloadMaterialStatus.staticReady || !mPreloadMaterialStatus.voxelReady;
	mPreloadMaterialStatus.failed = false;

	traceMaterialWarmup();
	return true;
}
