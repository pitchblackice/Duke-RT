#include "nri_scene_bridge.h"

#include "nri_geometry_bridge.h"
#include "nri_portal_bridge.h"
#include "nri_texture_signature.h"

#include "c_cvars.h"
#include "coreactor.h"
#include "hw_portal.h"
#include "hw_voxels.h"
#include "image.h"
#include "model_kvx.h"
#include "skyboxtexture.h"
#include "gametexture.h"
#include "texturemanager.h"
#include "texinfo.h"
#include "textures.h"
#include "v_video.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

EXTERN_CVAR(Bool, r_voxels)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, nri_ptactorspritetrace)
CVAR(Bool, nri_voxelstats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxeltrianglebudget, 250000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelmaxtriangles, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelcaptureactors, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelpersistentpromoteframes, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelmeshbuilds, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptloadingtrace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptloadingvoxelactors, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptloadingvoxelvariants, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptloadingvoxelvariantprims, 1000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	using namespace nri_scene;

	constexpr float kAttachedWallSpriteDepthNudge = 0.01f;
	constexpr uint32_t kTransientVoxelLiveSurfacePrimitiveLimit = 20000;

	SkyPerfStats gSkyPerfStats = {};
	DynamicCapturePerfStats gDynamicCapturePerfStats = {};
	VoxelMeshPrecacheStats gVoxelLoadingWarmupStats = {};
	uint32_t gVoxelMeshCacheFrame = 0;
	uint32_t gVoxelMeshCacheCaptureDepth = 0;
	uint32_t gVoxelMeshBuildsThisFrame = 0;

	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedDynamicCaptureTimer
	{
	public:
		explicit ScopedDynamicCaptureTimer(double& targetMs)
			: mTarget(&targetMs), mStart(std::chrono::steady_clock::now())
		{
		}

		~ScopedDynamicCaptureTimer()
		{
			*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	struct AverageColorCacheEntry
	{
		bool success = false;
		float color[3] = {};
	};

	struct CachedSkyInspection
	{
		bool valid = false;
		bool hasAverageColor = false;
		bool isCubemap = false;
		bool isThreeFace = false;
		bool flipTop = false;
		uint32_t faceMask = 0;
		float color[3] = {};
	};

	std::unordered_map<const FTexture*, AverageColorCacheEntry> gFrameLocalAverageTextureColorCache;
	std::unordered_map<uint64_t, AverageColorCacheEntry> gPersistentAverageTextureColorCache;
	std::unordered_map<const FGameTexture*, CachedSkyInspection> gSkyInspectionCache;
	struct VoxelMeshCacheEntry
	{
		FVoxelMeshData mesh;
		uint32_t deferredFrame = 0;
		bool built = false;
		bool valid = false;
	};

	struct VoxelMeshVariantSurfaceCacheEntry
	{
		SurfaceRef canonicalSurface;
		uint64_t meshVariantHash = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		bool built = false;
		bool valid = false;
	};

	std::unordered_map<const FVoxelModel*, VoxelMeshCacheEntry> gVoxelMeshCache;
	std::unordered_map<uint64_t, VoxelMeshVariantSurfaceCacheEntry> gVoxelMeshVariantSurfaceCache;
	uint64_t gVoxelActorCacheFrame = 0;
	uint32_t gVoxelActorCacheCaptureDepth = 0;
	uint64_t gVoxelActorCacheSerial = 1;

	struct VoxelMeshVariantKey
	{
		const voxmodel_t* voxel = nullptr;
		const FVoxelModel* model = nullptr;
		FTextureID sourcePicnum = {};
		int resolvedVoxelIndex = -1;
		uint32_t geometryState = 0;
	};

	struct VoxelMaterialVariantKey
	{
		FGameTexture* voxelTexture = nullptr;
		FGameTexture* emissiveSourceTexture = nullptr;
		int palette = 0;
		int shade = 0;
		uint32_t alphaBits = 0;
		uint32_t materialFlags = 0;
	};

	struct VoxelInstanceKey
	{
		int32_t actorIndex = -1;
		DCoreActor* actorPtr = nullptr;
		uint32_t actorGeneration = 0;
	};

	struct VoxelActorCacheEntry
	{
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t bakedSurfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t transformBasisSignature = 0;
		uint64_t identityKey = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t instanceKeyHash = 0;
		VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
		uint64_t desiredSignature = 0;
		uint64_t desiredMeshKeyHash = 0;
		uint64_t desiredMaterialKeyHash = 0;
		uint64_t desiredMeshVariantHash = 0;
		uint64_t desiredMaterialVariantHash = 0;
		uint64_t desiredSurfaceSignature = 0;
		uint64_t pendingFrame = 0;
		uint64_t surfaceFrame = 0;
		uint8_t pendingReason = 0;
		int32_t actorIndex = -1;
		uintptr_t actorPtr = 0;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		SurfaceRef surface;
		uint64_t lastSeenFrame = 0;
		uint32_t primitiveCount = 0;
		float currentTranslation[3] = {};
		float bakedTranslation[3] = {};
		float currentTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		bool persistentReady = false;
		bool hasSurface = false;
		SurfaceRef lightSurface;
	};

	std::unordered_map<uint64_t, VoxelActorCacheEntry> gVoxelActorCache;

	uint32_t CountSurfacePrimitives(const SurfaceRef& surface);

	void AddVoxelMeshPrecacheStats(VoxelMeshPrecacheStats& target, const VoxelMeshPrecacheStats& delta)
	{
		target.textureCandidates += delta.textureCandidates;
		target.actorCandidates += delta.actorCandidates;
		target.modelCandidates += delta.modelCandidates;
		target.meshVariantCandidates += delta.meshVariantCandidates;
		target.meshHits += delta.meshHits;
		target.meshBuilds += delta.meshBuilds;
		target.meshInvalid += delta.meshInvalid;
		target.meshSkipped += delta.meshSkipped;
		target.meshVariantHits += delta.meshVariantHits;
		target.meshVariantBuilds += delta.meshVariantBuilds;
		target.meshVariantInvalid += delta.meshVariantInvalid;
		target.vertices += delta.vertices;
		target.indices += delta.indices;
		target.primitives += delta.primitives;
		target.variantPrimitives += delta.variantPrimitives;
		target.buildMs += delta.buildMs;
	}

	void RecordVoxelMeshPrecacheStats(const VoxelMeshPrecacheStats& delta, VoxelMeshPrecacheStats* stats)
	{
		if (stats != nullptr)
		{
			AddVoxelMeshPrecacheStats(*stats, delta);
		}
		AddVoxelMeshPrecacheStats(gVoxelLoadingWarmupStats, delta);
	}

	FVoxelModel* ResolveVoxelTextureModel(FTextureID texid, int* outVoxelIndex = nullptr)
	{
		if (outVoxelIndex != nullptr)
		{
			*outVoxelIndex = -1;
		}
		if (!texid.isValid())
		{
			return nullptr;
		}

		const int voxelIndex = GetExtInfo(texid).tiletovox;
		if (outVoxelIndex != nullptr)
		{
			*outVoxelIndex = voxelIndex;
		}
		if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr)
		{
			return nullptr;
		}
		return voxmodels[voxelIndex]->model;
	}

	VoxelMeshVariantKey BuildLoadingVoxelMeshVariantKey(FTextureID texid, FVoxelModel* model, int voxelIndex)
	{
		VoxelMeshVariantKey key = {};
		key.voxel = voxelIndex >= 0 && voxelIndex < MAXVOXELS ? voxmodels[voxelIndex] : nullptr;
		key.model = model;
		key.sourcePicnum = texid;
		key.resolvedVoxelIndex = voxelIndex;
		key.geometryState = 0;
		return key;
	}

	struct VoxelCaptureBudget
	{
		uint32_t remainingTriangles = 0;
		uint32_t remainingCacheUpdates = 0;
		bool unlimited = false;
		bool unlimitedCacheUpdates = false;
		bool spentTriangleBudget = false;
	};

	enum class VoxelActorStability : uint8_t
	{
		Uncacheable,
		New,
		Stable,
		TransformRebake,
		Changed,
	};

	enum class VoxelActorPendingReason : uint8_t
	{
		None,
		ActorBudget,
		MeshDeferred,
		TriangleBudget,
		SurfaceBuildFailed,
		ActorNotLive,
	};

	struct VoxelActorCacheLookup
	{
		VoxelActorStability stability = VoxelActorStability::Uncacheable;
		uint64_t identityKey = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t transformBasisSignature = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t instanceKeyHash = 0;
		VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
		int32_t actorIndex = -1;
		uintptr_t actorPtr = 0;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		float currentTranslation[3] = {};
		float currentTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		VoxelActorCacheEntry* entry = nullptr;
	};

	bool IsUsableGameTexturePointer(FGameTexture* texture);

	bool ShouldTraceSkyPerf()
	{
		return nri_pttraceframes > 0;
	}

	class ScopedSkyPerfTimer
	{
	public:
		explicit ScopedSkyPerfTimer(uint64_t& targetUs)
			: mTarget(ShouldTraceSkyPerf() ? &targetUs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSkyPerfTimer()
		{
			if (mTarget != nullptr)
			{
				const auto elapsed = std::chrono::steady_clock::now() - mStart;
				*mTarget += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			}
		}

		ScopedSkyPerfTimer(const ScopedSkyPerfTimer&) = delete;
		ScopedSkyPerfTimer& operator=(const ScopedSkyPerfTimer&) = delete;

	private:
		uint64_t* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	bool TryBuildPersistentAverageColorSignature(FGameTexture* texture, TextureSignature& outSignature)
	{
		outSignature = {};
		return TryBuildAverageColorTextureSignature(texture, outSignature) &&
			outSignature.valid &&
			outSignature.persistentEligible;
	}

	bool TryLoadPersistentAverageColor(const TextureSignature& signature, float* outColor, bool& outSuccess)
	{
		outSuccess = false;
		if (!signature.valid || !signature.persistentEligible)
		{
			return false;
		}

		const auto it = gPersistentAverageTextureColorCache.find(signature.key);
		if (it == gPersistentAverageTextureColorCache.end())
		{
			return false;
		}

		outSuccess = it->second.success;
		if (outSuccess)
		{
			Copy3(it->second.color, outColor);
		}
		return true;
	}

	void StorePersistentAverageColor(const TextureSignature& signature, bool success, const float* color)
	{
		if (!signature.valid || !signature.persistentEligible)
		{
			return;
		}

		AverageColorCacheEntry& entry = gPersistentAverageTextureColorCache[signature.key];
		entry.success = success;
		if (success && color != nullptr)
		{
			Copy3(color, entry.color);
		}
	}

	bool TryLoadFrameLocalAverageColor(FTexture* baseTexture, float* outColor, bool& outSuccess)
	{
		outSuccess = false;
		if (baseTexture == nullptr)
		{
			return false;
		}

		const auto it = gFrameLocalAverageTextureColorCache.find(baseTexture);
		if (it == gFrameLocalAverageTextureColorCache.end())
		{
			return false;
		}

		outSuccess = it->second.success;
		if (outSuccess)
		{
			Copy3(it->second.color, outColor);
		}
		return true;
	}

	void StoreFrameLocalAverageColor(FTexture* baseTexture, bool success, const float* color)
	{
		if (baseTexture == nullptr)
		{
			return;
		}

		AverageColorCacheEntry& entry = gFrameLocalAverageTextureColorCache[baseTexture];
		entry.success = success;
		if (success && color != nullptr)
		{
			Copy3(color, entry.color);
		}
	}

	bool TryLoadCachedSkyInspection(FGameTexture* texture, CachedSkyInspection& outInspection)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return false;
		}

		const auto it = gSkyInspectionCache.find(texture);
		if (it == gSkyInspectionCache.end())
		{
			return false;
		}

		outInspection = it->second;
		return true;
	}

	void StoreCachedSkyInspection(FGameTexture* texture, const CachedSkyInspection& inspection)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return;
		}

		gSkyInspectionCache[texture] = inspection;
	}

	struct SkyCandidate
	{
		bool valid = false;
		bool hasAverageColor = false;
		bool hasFallbackColor = false;
		bool isCubemap = false;
		bool isThreeFace = false;
		bool flipTop = false;
		uint32_t faceMask = 0;
		uint32_t priority = 0;
		float color[3] = {};
	};

	bool IsUsableGameTexturePointer(FGameTexture* texture)
	{
		const uintptr_t value = (uintptr_t)texture;
		if (value <= 0x10000 ||
			value == (uintptr_t)-1 ||
			(value & (sizeof(void*) - 1)) != 0)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION pointerInfo = {};
		if (VirtualQuery(texture, &pointerInfo, sizeof(pointerInfo)) != sizeof(pointerInfo) ||
			pointerInfo.State != MEM_COMMIT ||
			(pointerInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
		{
			return false;
		}

		void* vtable = nullptr;
		__try
		{
			vtable = *(void**)texture;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
		}

		if (vtable == nullptr)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION vtableInfo = {};
		return VirtualQuery(vtable, &vtableInfo, sizeof(vtableInfo)) == sizeof(vtableInfo) &&
			vtableInfo.State == MEM_COMMIT &&
			(vtableInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
	}

	int GetOwnerActorIndex(const HWWall& wall)
	{
		return wall.Sprite != nullptr && wall.Sprite->ownerActor != nullptr ? wall.Sprite->ownerActor->GetIndex() : -1;
	}

	int GetOwnerActorIndex(const HWFlat& flat)
	{
		return flat.Sprite != nullptr && flat.Sprite->ownerActor != nullptr ? flat.Sprite->ownerActor->GetIndex() : -1;
	}

	int GetOwnerActorIndex(const HWSprite& sprite)
	{
		return sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr ? sprite.Sprite->ownerActor->GetIndex() : -1;
	}

	CapturedVertex MakeCapturedVertex(const FFlatVertex& source)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = source.x;
		vertex.position[1] = source.z;
		vertex.position[2] = source.y;
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = source.u;
		vertex.uv[1] = source.v;
		return vertex;
	}

	uint32_t MakeSkyPriority(PTSkyMode mode, PTSkySourceType sourceType)
	{
		uint32_t priority = mode == PTSkyMode::Cubemap ? 100u : (mode == PTSkyMode::SolidColor ? 10u : 0u);
		switch (sourceType)
		{
		case PTSkySourceType::Portal:
			return priority + 3u;
		case PTSkySourceType::Flat:
			return priority + 2u;
		case PTSkySourceType::Wall:
			return priority + 1u;
		default:
			return priority;
		}
	}

	bool TryComputeAverageColorFromBaseTexture(FTexture* baseTexture, float* outColor)
	{
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorBaseCalls++;
		}
		ScopedSkyPerfTimer timer(gSkyPerfStats.averageColorTimeUs);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		double sum[3] = {};
		const size_t pixelCount = (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight;
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorPixels += (uint64_t)pixelCount;
		}
		for (size_t i = 0; i < pixelCount; ++i)
		{
			const uint8_t* pixel = texBuffer.mBuffer + i * 4u;
			sum[0] += pixel[2];
			sum[1] += pixel[1];
			sum[2] += pixel[0];
		}

		const double scale = pixelCount > 0 ? 1.0 / (255.0 * (double)pixelCount) : 0.0;
		outColor[0] = (float)(sum[0] * scale);
		outColor[1] = (float)(sum[1] * scale);
		outColor[2] = (float)(sum[2] * scale);
		return true;
	}

	bool TryGetAverageTextureColorRecursive(FGameTexture* texture, float* outColor, int depth);

	void ApplyCachedSkyInspectionToCandidate(const CachedSkyInspection& inspection, uint32_t fallbackColor, PTSkySourceType sourceType, SkyCandidate& outCandidate)
	{
		outCandidate = {};
		outCandidate.valid = inspection.valid;
		outCandidate.isCubemap = inspection.isCubemap;
		outCandidate.isThreeFace = inspection.isThreeFace;
		outCandidate.flipTop = inspection.flipTop;
		outCandidate.faceMask = inspection.faceMask;
		outCandidate.priority = MakeSkyPriority(inspection.isCubemap ? PTSkyMode::Cubemap : PTSkyMode::SolidColor, sourceType);
		if (inspection.hasAverageColor)
		{
			outCandidate.hasAverageColor = true;
			Copy3(inspection.color, outCandidate.color);
		}
		else if (fallbackColor != 0)
		{
			const PalEntry fallback = PalEntry(fallbackColor);
			outCandidate.color[0] = fallback.r / 255.0f;
			outCandidate.color[1] = fallback.g / 255.0f;
			outCandidate.color[2] = fallback.b / 255.0f;
			outCandidate.hasFallbackColor = true;
		}
	}

	bool TryInspectSkyTextureInner(FGameTexture* texture, CachedSkyInspection& outInspection)
	{
		__try
		{
			if (!IsUsableGameTexturePointer(texture))
			{
				return false;
			}

			outInspection = {};
			outInspection.valid = true;
			if (TryGetAverageTextureColor(texture, outInspection.color))
			{
				outInspection.hasAverageColor = true;
			}

			FTexture* baseTexture = nullptr;
			__try
			{
				baseTexture = texture->GetTexture();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				baseTexture = nullptr;
			}

			auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
			if (skybox == nullptr)
			{
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectSolidCandidates++;
				}
				return true;
			}

			outInspection.flipTop = skybox->GetSkyFlip();
			outInspection.isThreeFace = skybox->Is3Face();
			for (int i = 0; i < 6; ++i)
			{
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectFaceWalks++;
				}
				FGameTexture* face = nullptr;
				__try
				{
					face = skybox->GetSkyFace(i);
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					face = nullptr;
				}

				if (IsUsableGameTexturePointer(face))
				{
					outInspection.faceMask |= 1u << i;
				}
			}

			if (!outInspection.isThreeFace && outInspection.faceMask == 0x3fu)
			{
				outInspection.isCubemap = true;
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectCubemapCandidates++;
				}
			}
			else if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.inspectSolidCandidates++;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TryInspectSkyTexture(FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType, SkyCandidate& outCandidate)
	{
		CachedSkyInspection inspection = {};
		if (!TryLoadCachedSkyInspection(texture, inspection))
		{
			if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.inspectCalls++;
			}
			ScopedSkyPerfTimer timer(gSkyPerfStats.inspectTimeUs);
			if (!TryInspectSkyTextureInner(texture, inspection))
			{
				outCandidate = {};
				return false;
			}
			StoreCachedSkyInspection(texture, inspection);
		}

		ApplyCachedSkyInspectionToCandidate(inspection, fallbackColor, sourceType, outCandidate);
		return outCandidate.valid;
	}

	void ApplySkyCandidate(SceneView& outView, FGameTexture* texture, const SkyCandidate& candidate, PTSkySourceType sourceType)
	{
		if (!candidate.valid || candidate.priority < outView.sky.priority)
		{
			return;
		}

		if (candidate.hasAverageColor || candidate.hasFallbackColor)
		{
			Copy3(candidate.color, outView.skyColor);
		}

		if (candidate.priority == outView.sky.priority && outView.sky.texture != nullptr)
		{
			return;
		}

		outView.sky.mode = candidate.isCubemap ? PTSkyMode::Cubemap : PTSkyMode::SolidColor;
		outView.sky.sourceType = sourceType;
		outView.sky.texture = texture;
		outView.sky.faceMask = candidate.faceMask;
		outView.sky.priority = candidate.priority;
		outView.sky.flipTop = candidate.flipTop;
		outView.sky.isThreeFace = candidate.isThreeFace;
	}

	bool TryGetAverageTextureColorRecursive(FGameTexture* texture, float* outColor, int depth)
	{
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorRecursiveCalls++;
		}
		if (!IsUsableGameTexturePointer(texture) || depth > 4)
		{
			return false;
		}

		FTexture* baseTexture = nullptr;
		__try
		{
			baseTexture = texture->GetTexture();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		if (baseTexture == nullptr)
		{
			return false;
		}

		TextureSignature persistentSignature = {};
		const bool hasPersistentSignature = TryBuildPersistentAverageColorSignature(texture, persistentSignature);

		bool cachedSuccess = false;
		if (hasPersistentSignature && TryLoadPersistentAverageColor(persistentSignature, outColor, cachedSuccess))
		{
			return cachedSuccess;
		}

		if (TryLoadFrameLocalAverageColor(baseTexture, outColor, cachedSuccess))
		{
			return cachedSuccess;
		}

		float computedColor[3] = {};
		if (TryComputeAverageColorFromBaseTexture(baseTexture, computedColor))
		{
			StoreFrameLocalAverageColor(baseTexture, true, computedColor);
			if (hasPersistentSignature)
			{
				StorePersistentAverageColor(persistentSignature, true, computedColor);
			}
			Copy3(computedColor, outColor);
			return true;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
		if (skybox == nullptr)
		{
			StoreFrameLocalAverageColor(baseTexture, false, nullptr);
			if (hasPersistentSignature)
			{
				StorePersistentAverageColor(persistentSignature, false, nullptr);
			}
			return false;
		}

		float accumulated[3] = {};
		int sampledFaces = 0;
		for (int i = 0; i < 6; ++i)
		{
			if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.recursiveSkyboxFaceSamples++;
			}
			float faceColor[3] = {};
			FGameTexture* skyFace = nullptr;
			__try
			{
				skyFace = skybox->GetSkyFace(i);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				skyFace = nullptr;
			}
			if (TryGetAverageTextureColorRecursive(skyFace, faceColor, depth + 1))
			{
				accumulated[0] += faceColor[0];
				accumulated[1] += faceColor[1];
				accumulated[2] += faceColor[2];
				sampledFaces++;
			}
		}

		if (sampledFaces > 0)
		{
			const float invCount = 1.0f / sampledFaces;
			computedColor[0] = accumulated[0] * invCount;
			computedColor[1] = accumulated[1] * invCount;
			computedColor[2] = accumulated[2] * invCount;
			StoreFrameLocalAverageColor(baseTexture, true, computedColor);
			Copy3(computedColor, outColor);
			return true;
		}

		FGameTexture* previous = nullptr;
		__try
		{
			previous = skybox->previous;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			previous = nullptr;
		}
		const bool success = TryGetAverageTextureColorRecursive(previous, computedColor, depth + 1);
		StoreFrameLocalAverageColor(baseTexture, success, success ? computedColor : nullptr);
		if (success)
		{
			Copy3(computedColor, outColor);
		}
		return success;
	}

	unsigned int CountDrawListItems(HWDrawInfo& di, DrawListType type)
	{
		return di.drawlists[type].Size();
	}

	unsigned int CountFanTriangles(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (unsigned int)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (unsigned int)surface.vertices.size() - 2u : 0u;
	}

	unsigned int CountTriangleListTriangles(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (unsigned int)(surface.indices.size() / 3u);
		}
		return (unsigned int)(surface.vertices.size() / 3u);
	}

	void ApplyActorPreviousTransform(SurfaceRef& surface, DCoreActor* actor)
	{
		if (actor == nullptr)
		{
			return;
		}

		const DVector3 worldDelta = actor->spr.pos - actor->opos;
		const float renderDelta[3] = {
			(float)worldDelta.X,
			(float)-worldDelta.Z,
			(float)-worldDelta.Y
		};

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.prevPosition[0] = vertex.position[0] - renderDelta[0];
			vertex.prevPosition[1] = vertex.position[1] - renderDelta[1];
			vertex.prevPosition[2] = vertex.position[2] - renderDelta[2];
		}
	}

	bool TryComputeSurfaceNormal(const SurfaceRef& surface, float* outNormal);

	bool TryFindNearbyWallSpriteBackingWall(const HWWall& wall, walltype*& outWall, DVector2& outNearestPoint)
	{
		outWall = nullptr;
		outNearestPoint = {};
		if (wall.Sprite == nullptr || wall.Sprite->sectp == nullptr)
		{
			return false;
		}

		if (wall.walldist != nullptr)
		{
			outWall = wall.walldist;
			outNearestPoint = NearestPointOnWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, outWall, false);
			return true;
		}

		double maxOrthDist = 3.0 * maptoworld;
		const double maxDistSq = maxOrthDist * maxOrthDist;
		const DAngle maxAngDelta = DAngle360 / 1024;
		walltype* bestWall = nullptr;
		DVector2 bestNearestPoint = {};

		for (auto& candidate : wall.Sprite->sectp->walls)
		{
			const DVector2 delta = candidate.delta();
			const DAngle deltaAng = absangle(delta.Angle(), wall.Sprite->Angles.Yaw);
			if (deltaAng < DAngle90 - maxAngDelta || deltaAng > DAngle90 + maxAngDelta)
			{
				continue;
			}

			DVector2 nearestPoint = NearestPointOnWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, &candidate, false);
			if (!((wall.Sprite->Angles.Yaw.Buildang()) & 510))
			{
				double newDist = DBL_MAX;
				if (delta.X == 0.0)
				{
					newDist = fabs(wall.Sprite->pos.X - candidate.pos.X);
				}
				else if (delta.Y == 0.0)
				{
					newDist = fabs(wall.Sprite->pos.Y - candidate.pos.Y);
				}

				if (newDist < maxOrthDist)
				{
					maxOrthDist = newDist;
					bestWall = &candidate;
					bestNearestPoint = nearestPoint;
				}
			}
			else
			{
				const double wallDistSq = SquareDistToWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, &candidate, &nearestPoint);
				if (wallDistSq <= maxDistSq)
				{
					outWall = &candidate;
					outNearestPoint = nearestPoint;
					return true;
				}
			}
		}

		if (bestWall == nullptr)
		{
			return false;
		}

		outWall = bestWall;
		outNearestPoint = bestNearestPoint;
		return true;
	}

	void NudgeAttachedWallSpriteSurface(const HWWall& wall, SurfaceRef& surface)
	{
		if (wall.Sprite == nullptr || surface.vertices.empty())
		{
			return;
		}

		float offset[3] = {};
		walltype* backingWall = nullptr;
		DVector2 nearestPoint = {};
		if (TryFindNearbyWallSpriteBackingWall(wall, backingWall, nearestPoint))
		{
			DVector2 nudgeDirection = {};
			const DVector2 spriteCenter = wall.Sprite->pos.XY();
			const DVector2 wallToSprite = spriteCenter - nearestPoint;
			if (wallToSprite.LengthSquared() > 1.0e-8)
			{
				nudgeDirection = wallToSprite.Unit();
			}
			else
			{
				// Exact on-wall placements need a stable wall-side fallback. Match the
				// same sector-owned wall normal convention used by pushmove().
				const DVector2 wallNormal = backingWall->delta().Rotated90CCW();
				if (wallNormal.LengthSquared() <= 1.0e-8)
				{
					return;
				}

				nudgeDirection = wallNormal.Unit();
			}

			offset[0] = (float)(nudgeDirection.X * kAttachedWallSpriteDepthNudge);
			offset[2] = (float)(-nudgeDirection.Y * kAttachedWallSpriteDepthNudge);
		}
		else
		{
			float normal[3] = {};
			if (!TryComputeSurfaceNormal(surface, normal))
			{
				return;
			}

			offset[0] = normal[0] * kAttachedWallSpriteDepthNudge;
			offset[1] = normal[1] * kAttachedWallSpriteDepthNudge;
			offset[2] = normal[2] * kAttachedWallSpriteDepthNudge;
		}

		// The raster path uses depth bias / draw-order handling for wall-flush
		// sprite content. PT needs a small geometric equivalent or nearby walls
		// can win the closest-hit test.

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += offset[0];
			vertex.position[1] += offset[1];
			vertex.position[2] += offset[2];
			vertex.prevPosition[0] += offset[0];
			vertex.prevPosition[1] += offset[1];
			vertex.prevPosition[2] += offset[2];
		}
	}

	bool TryComputeSurfaceNormal(const SurfaceRef& surface, float* outNormal)
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const CapturedVertex& a = surface.vertices[0];
		const CapturedVertex& b = surface.vertices[1];
		const CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / sqrtf(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	void NudgeSpriteFlatSurface(const HWFlat& flat, SurfaceRef& surface)
	{
		if (flat.Sprite == nullptr || surface.vertices.empty())
		{
			return;
		}

		float normal[3] = {};
		if (!TryComputeSurfaceNormal(surface, normal))
		{
			return;
		}

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * kAttachedWallSpriteDepthNudge;
			vertex.position[1] += normal[1] * kAttachedWallSpriteDepthNudge;
			vertex.position[2] += normal[2] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[0] += normal[0] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[1] += normal[1] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[2] += normal[2] * kAttachedWallSpriteDepthNudge;
		}
	}

	bool IsEffectivelyOpaque(const FRenderStyle& style, float alpha)
	{
		return alpha >= 0.999f &&
			style.BlendOp == STYLEOP_Add &&
			style.SrcAlpha == STYLEALPHA_Src &&
			style.DestAlpha == STYLEALPHA_InvSrc &&
			style.Flags == 0;
	}

	bool IsOpaqueSurface(const HWWall& wall)
	{
		return wall.texture != nullptr &&
			wall.vertcount >= 3 &&
			IsEffectivelyOpaque(wall.RenderStyle, wall.alpha);
	}

	bool IsSkyWall(const HWWall& wall)
	{
		return (wall.flags & HWWall::HWF_SKYHACK) != 0;
	}

	bool IsPortalSourceWall(const HWWall& wall)
	{
		if (wall.seg == nullptr)
		{
			return false;
		}

		switch (wall.seg->portalflags)
		{
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
			return true;
		default:
			return false;
		}
	}

	SurfaceProvenance MakeWallProvenance(const walltype* seg, SurfaceSourceType sourceType, uint32_t drawListType, int actorIndex, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = sourceType;
		provenance.actorIndex = actorIndex;
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (seg != nullptr)
		{
			provenance.sectorIndex = seg->sector;
			provenance.wallIndex = wall.IndexOf(seg);
			provenance.nextSectorIndex = seg->nextsector;
			provenance.cstat = (uint32_t)seg->cstat;
		}
		return provenance;
	}

	SurfaceProvenance MakeFlatProvenance(const HWFlat& flat, uint32_t drawListType, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = flat.plane == plane_ceiling ? SurfaceSourceType::CeilingFlat : SurfaceSourceType::FloorFlat;
		provenance.actorIndex = GetOwnerActorIndex(flat);
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (flat.sec != nullptr)
		{
			provenance.sectorIndex = sector.IndexOf(flat.sec);
			provenance.cstat = flat.plane == plane_ceiling ? (uint32_t)flat.sec->ceilingstat : (uint32_t)flat.sec->floorstat;
		}
		return provenance;
	}

	SurfaceProvenance MakeSpriteProvenance(const HWSprite& sprite, SurfaceSourceType sourceType, uint32_t drawListType, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = sourceType;
		provenance.actorIndex = GetOwnerActorIndex(sprite);
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr && sprite.Sprite->ownerActor->spr.sectp != nullptr)
		{
			provenance.sectorIndex = sector.IndexOf(sprite.Sprite->ownerActor->spr.sectp);
		}
		return provenance;
	}

	bool IsOpaqueSurface(const HWFlat& flat)
	{
		return flat.texture != nullptr &&
			flat.vertcount >= 3 &&
			IsEffectivelyOpaque(flat.RenderStyle, flat.alpha) &&
			flat.Sprite == nullptr &&
			true;
	}

	bool IsOpaqueSpriteFlat(const HWFlat& flat)
	{
		return flat.texture != nullptr &&
			flat.vertcount >= 3 &&
			IsEffectivelyOpaque(flat.RenderStyle, flat.alpha) &&
			flat.Sprite != nullptr;
	}

	bool IsSkyFlat(const HWFlat& flat)
	{
		if (flat.sec == nullptr)
		{
			return false;
		}

		if (flat.plane == plane_ceiling)
		{
			return (flat.sec->ceilingstat & CSTAT_SECTOR_SKY) != 0;
		}

		return (flat.sec->floorstat & CSTAT_SECTOR_SKY) != 0;
	}

	bool IsPortalSourceFlat(const HWFlat& flat)
	{
		if (flat.stack || flat.sec == nullptr)
		{
			return true;
		}

		const int flags = flat.sec->portalflags;
		if (flat.plane == plane_ceiling)
		{
			return flags == PORTAL_SECTOR_CEILING || flags == PORTAL_SECTOR_CEILING_REFLECT;
		}

		return flags == PORTAL_SECTOR_FLOOR || flags == PORTAL_SECTOR_FLOOR_REFLECT;
	}

	bool DrawListUsesAlphaClip(uint32_t drawListType)
	{
		switch (drawListType)
		{
		case GLDL_MASKEDWALLS:
		case GLDL_MASKEDWALLSS:
		case GLDL_MASKEDWALLSD:
		case GLDL_MASKEDWALLSV:
		case GLDL_MASKEDWALLSH:
		case GLDL_MASKEDFLATS:
		case GLDL_MASKEDSLOPEFLATS:
		case GLDL_TRANSLUCENT:
		case GLDL_MODELS:
			return true;
		default:
			return false;
		}
	}

	void CaptureWalls(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats, SceneView& outView)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || !IsOpaqueSurface(*wall))
			{
				continue;
			}

			if (IsSkyWall(*wall))
			{
				stats.skySurfaces++;
				UpdateSceneSky(outView, wall->texture, 0, PTSkySourceType::Wall);
				continue;
			}

			if (IsPortalSourceWall(*wall))
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			uint32_t extraFlags = wall->Sprite != nullptr ? MaterialFlag_Sprite : MaterialFlag_None;
			if (wall->Sprite != nullptr || DrawListUsesAlphaClip(drawListType))
			{
				extraFlags |= MaterialFlag_AlphaClip;
			}
			surface.material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, extraFlags);
			surface.provenance = MakeWallProvenance(wall->seg, SurfaceSourceType::DrawListWall, drawListType, GetOwnerActorIndex(*wall), surface.material.flags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			surface.vertices.reserve(wall->vertcount);
			for (uint32_t i = 0; i < wall->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (wall->Sprite != nullptr && wall->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, wall->Sprite->ownerActor);
			}

			NudgeAttachedWallSpriteSurface(*wall, surface);

			outWalls.push_back(std::move(surface));
		}
	}

	void CaptureMirrorBorders(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || wall->type != RENDERWALL_MIRRORSURFACE)
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, MaterialFlag_Mirror);
			surface.provenance = MakeWallProvenance(wall->seg, SurfaceSourceType::MirrorWall, drawListType, GetOwnerActorIndex(*wall), surface.material.flags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			surface.vertices.reserve(wall->vertcount);
			for (uint32_t i = 0; i < wall->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			outWalls.push_back(std::move(surface));
			stats.mirrorSurfaces++;
		}
	}

	void CaptureFlats(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outFlats, SceneDebugStats& stats, SceneView& outView)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr || !IsOpaqueSurface(*flat))
			{
				continue;
			}

			if (IsSkyFlat(*flat))
			{
				stats.skySurfaces++;
				UpdateSceneSky(outView, flat->texture, 0, PTSkySourceType::Flat);
				continue;
			}

			if (IsPortalSourceFlat(*flat))
			{
				continue;
			}

			flat->MakeVertices(&di);
			if (flat->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			uint32_t extraFlags = MaterialFlag_Flat;
			if (DrawListUsesAlphaClip(drawListType))
			{
				extraFlags |= MaterialFlag_AlphaClip;
			}
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, extraFlags);
			surface.provenance = MakeFlatProvenance(*flat, drawListType, surface.material.flags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			surface.vertices.reserve((uint32_t)flat->vertcount);
			for (int i = 0; i < flat->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (flat->Sprite != nullptr && flat->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, flat->Sprite->ownerActor);
			}

			outFlats.push_back(std::move(surface));
		}
	}

	void CaptureSpriteFlats(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outFlats)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr)
			{
				continue;
			}

			flat->MakeVertices(&di);
			if (!IsOpaqueSpriteFlat(*flat))
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat | MaterialFlag_Sprite | MaterialFlag_AlphaClip);
			surface.provenance = MakeFlatProvenance(*flat, drawListType, surface.material.flags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			surface.vertices.reserve((uint32_t)flat->vertcount);
			for (int i = 0; i < flat->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (flat->Sprite != nullptr && flat->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, flat->Sprite->ownerActor);
			}

			NudgeSpriteFlatSurface(*flat, surface);

			outFlats.push_back(std::move(surface));
		}
	}

	bool IsOpaqueSprite(const HWSprite& sprite)
	{
		return sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			IsEffectivelyOpaque(sprite.RenderStyle, sprite.alpha);
	}

	bool IsOwnedByActor(const HWSprite& sprite, int32_t actorIndex)
	{
		return
			sprite.Sprite != nullptr &&
			sprite.Sprite->ownerActor != nullptr &&
			(int32_t)sprite.Sprite->ownerActor->GetIndex() == actorIndex;
	}

	bool IsCapturableActorShadowTempSprite(const HWSprite& sprite)
	{
		return
			sprite.Sprite != nullptr &&
			sprite.Sprite->statnum == 99 &&
			sprite.Sprite->pal == 4 &&
			sprite.Sprite->shade == 127 &&
			(sprite.Sprite->cstat & CSTAT_SPRITE_TRANSLUCENT) != 0;
	}

	bool IsCapturableActorFacingSprite(const HWSprite& sprite, int32_t actorIndex)
	{
		return
			IsOwnedByActor(sprite, actorIndex) &&
			sprite.Sprite != nullptr &&
			(sprite.Sprite->statnum != 99 || IsCapturableActorShadowTempSprite(sprite)) &&
			sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			sprite.alpha > (1.0f / 255.0f);
	}

	void CaptureFacingSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOpaqueSprite(*sprite))
			{
				continue;
			}

			if (sprite->vertexindex < 0)
			{
				sprite->CreateVertices(&di);
			}

			if (sprite->vertexindex < 0)
			{
				continue;
			}

			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(sprite->vertexindex);
			if (vertices == nullptr)
			{
				continue;
			}

			SurfaceRef surface = {};
			uint32_t extraFlags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				extraFlags |= MaterialFlag_FacingBillboard;
			}
			surface.material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, extraFlags);
			surface.provenance = MakeSpriteProvenance(*sprite, SurfaceSourceType::FacingSprite, drawListType, surface.material.flags);
			surface.vertices.reserve(4);
			for (uint32_t i = 0; i < 4; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				if ((int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0 && screen != nullptr)
				{
					PathTracingActorSpriteTraceEvent event = {};
					event.stage = PathTracingActorSpriteTraceStage::CaptureScene;
					event.actorIndex = sprite->Sprite->ownerActor->GetIndex();
					event.spriteStatnum = sprite->Sprite->statnum;
					event.spritePicnum = sprite->Sprite->picnum;
					event.baseTextureId = sprite->Sprite->spritetexture().GetIndex();
					event.resolvedTextureId = sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1;
					event.palette = sprite->palette;
					event.shade = sprite->shade;
					event.cstat = sprite->Sprite->cstat;
					event.cstat2 = sprite->Sprite->cstat2;
					event.drawListType = drawListType;
					event.noAnimate = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
					event.fullbright = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
					event.resolvedGameTexture = sprite->texture;
					screen->EmitPathTracingActorSpriteTraceEvent(event);
				}
			}

			outSprites.push_back(std::move(surface));
		}
	}

	void CaptureActorFacingSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, int32_t actorIndex, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsCapturableActorFacingSprite(*sprite, actorIndex))
			{
				continue;
			}

			if (sprite->vertexindex < 0)
			{
				sprite->CreateVertices(&di);
			}

			if (sprite->vertexindex < 0)
			{
				continue;
			}

			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(sprite->vertexindex);
			if (vertices == nullptr)
			{
				continue;
			}

			SurfaceRef surface = {};
			uint32_t extraFlags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				extraFlags |= MaterialFlag_FacingBillboard;
			}
			surface.material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, extraFlags);
			surface.provenance = MakeSpriteProvenance(*sprite, SurfaceSourceType::FacingSprite, drawListType, surface.material.flags);
			surface.vertices.reserve(4);
			for (uint32_t i = 0; i < 4; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				if ((int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0 && screen != nullptr)
				{
					PathTracingActorSpriteTraceEvent event = {};
					event.stage = PathTracingActorSpriteTraceStage::CaptureActorScene;
					event.actorIndex = sprite->Sprite->ownerActor->GetIndex();
					event.spriteStatnum = sprite->Sprite->statnum;
					event.spritePicnum = sprite->Sprite->picnum;
					event.baseTextureId = sprite->Sprite->spritetexture().GetIndex();
					event.resolvedTextureId = sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1;
					event.palette = sprite->palette;
					event.shade = sprite->shade;
					event.cstat = sprite->Sprite->cstat;
					event.cstat2 = sprite->Sprite->cstat2;
					event.drawListType = drawListType;
					event.noAnimate = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
					event.fullbright = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
					event.resolvedGameTexture = sprite->texture;
					screen->EmitPathTracingActorSpriteTraceEvent(event);
				}
			}

			outSprites.push_back(std::move(surface));
		}
	}

	void TransformModelPoint(const VSMatrix& matrix, float x, float y, float z, CapturedVertex& outVertex, float u, float v)
	{
		float point[4] = { x, y, z, 1.0f };
		float transformed[4] = {};
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, transformed);

		outVertex.position[0] = transformed[0];
		outVertex.position[1] = transformed[1];
		outVertex.position[2] = transformed[2];
		outVertex.prevPosition[0] = transformed[0];
		outVertex.prevPosition[1] = transformed[1];
		outVertex.prevPosition[2] = transformed[2];
		outVertex.uv[0] = u;
		outVertex.uv[1] = v;
	}

	CapturedVertex MakeCapturedModelVertex(const VSMatrix& matrix, const FModelVertex& source)
	{
		CapturedVertex vertex = {};
		TransformModelPoint(matrix, source.x, source.y, source.z, vertex, source.u, source.v);
		return vertex;
	}

	CapturedVertex MakeCapturedLocalModelVertex(const FModelVertex& source)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = source.x;
		vertex.position[1] = source.y;
		vertex.position[2] = source.z;
		vertex.prevPosition[0] = source.x;
		vertex.prevPosition[1] = source.y;
		vertex.prevPosition[2] = source.z;
		vertex.uv[0] = source.u;
		vertex.uv[1] = source.v;
		return vertex;
	}

	void AddVoxelProxyFace(const VSMatrix& matrix, const float* extents, const int* indices, SurfaceRef& outSurface)
	{
		static const float corners[8][3] = {
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 1.0f, 1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 1.0f, 0.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 1.0f, 1.0f },
		};

		static const float uvs[4][2] = {
			{ 0.0f, 0.0f },
			{ 1.0f, 0.0f },
			{ 1.0f, 1.0f },
			{ 0.0f, 1.0f },
		};

		for (int i = 0; i < 4; ++i)
		{
			const float* local = corners[indices[i]];
			CapturedVertex vertex = {};
			TransformModelPoint(matrix, local[0] * extents[0], local[1] * extents[1], local[2] * extents[2], vertex, uvs[i][0], uvs[i][1]);
			outSurface.vertices.push_back(vertex);
		}
	}

	FGameTexture* GetVoxelReplacementEmissiveSourceTexture(const HWSprite& sprite)
	{
		if (sprite.Sprite == nullptr)
		{
			return nullptr;
		}

		FGameTexture* sourceTexture = TexMan.GetGameTexture(sprite.Sprite->spritetexture());
		return sourceTexture != nullptr && sourceTexture->isValid() ? sourceTexture : nullptr;
	}

	void CaptureVoxelProxySprite(const HWSprite& sprite, uint32_t drawListType, FGameTexture* voxelTexture, std::vector<SurfaceRef>& outSprites)
	{
		static const int faces[6][4] = {
			{ 0, 1, 2, 3 },
			{ 4, 5, 6, 7 },
			{ 0, 4, 7, 3 },
			{ 1, 5, 6, 2 },
			{ 3, 2, 6, 7 },
			{ 0, 1, 5, 4 },
		};

		const float extents[3] = {
			(float)sprite.voxel->siz.X,
			(float)sprite.voxel->siz.Z,
			(float)sprite.voxel->siz.Y
		};

		for (const auto& face : faces)
		{
			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(voxelTexture, sprite.palette, sprite.shade, sprite.alpha, MaterialFlag_Sprite | MaterialFlag_AlphaClip);
			surface.material.emissiveSourceTexture = GetVoxelReplacementEmissiveSourceTexture(sprite);
			surface.material.flags |= MaterialFlag_PointSampled;
			surface.material.flags &= ~MaterialFlag_Indexed;
			surface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, surface.material.flags);
			surface.vertices.reserve(4);
			AddVoxelProxyFace(sprite.rotmat, extents, face, surface);
			if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite.Sprite->ownerActor);
			}
			outSprites.push_back(std::move(surface));
		}
	}

	MaterialRef MakeVoxelPaletteMaterialRef(FGameTexture* voxelTexture, FGameTexture* emissiveSourceTexture, int palette, int shade, float alpha, uint32_t extraFlags)
	{
		MaterialRef material = MakeMaterialRef(voxelTexture, palette, shade, alpha, extraFlags | MaterialFlag_PointSampled);
		material.emissiveSourceTexture = emissiveSourceTexture;
		material.flags &= ~MaterialFlag_Indexed;
		return material;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
		return hash;
	}

	uint64_t QuantizeSignatureFloat(double value, double scale)
	{
		if (!std::isfinite(value))
		{
			return 0ull;
		}
		return (uint64_t)std::llround(value * scale);
	}

	uint32_t QuantizeSignatureFloat32(double value, double scale)
	{
		return (uint32_t)std::min<uint64_t>(QuantizeSignatureFloat(value, scale), UINT32_MAX);
	}

	int ResolveVoxelIndex(const HWSprite& sprite)
	{
		if (sprite.voxel == nullptr)
		{
			return -1;
		}

		if (sprite.Sprite != nullptr)
		{
			const int textureVoxelIndex = GetExtInfo(sprite.Sprite->spritetexture()).tiletovox;
			if (textureVoxelIndex >= 0 && textureVoxelIndex < MAXVOXELS && voxmodels[textureVoxelIndex] == sprite.voxel)
			{
				return textureVoxelIndex;
			}
		}

		for (int i = 0; i < MAXVOXELS; ++i)
		{
			if (voxmodels[i] == sprite.voxel)
			{
				return i;
			}
		}
		return -1;
	}

	VoxelMeshVariantKey BuildVoxelMeshVariantKey(const HWSprite& sprite)
	{
		VoxelMeshVariantKey key = {};
		key.voxel = sprite.voxel;
		key.model = sprite.voxel != nullptr ? sprite.voxel->model : nullptr;
		key.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->spritetexture() : FTextureID();
		key.resolvedVoxelIndex = ResolveVoxelIndex(sprite);
		key.geometryState = 0;
		return key;
	}

	VoxelMaterialVariantKey BuildVoxelMaterialVariantKey(FGameTexture* voxelTexture, const MaterialRef& material)
	{
		VoxelMaterialVariantKey key = {};
		key.voxelTexture = voxelTexture;
		key.emissiveSourceTexture = material.emissiveSourceTexture;
		key.palette = material.palette;
		key.shade = material.shade;
		key.alphaBits = QuantizeSignatureFloat32(material.alpha, 65535.0);
		key.materialFlags = material.flags;
		return key;
	}

	VoxelInstanceKey BuildVoxelInstanceKey(int32_t actorIndex, DCoreActor* actor)
	{
		VoxelInstanceKey key = {};
		key.actorIndex = actorIndex;
		key.actorPtr = actor;
		key.actorGeneration = 0;
		return key;
	}

	uint64_t BuildVoxelMeshVariantKeyHash(const VoxelMeshVariantKey& key)
	{
		if (key.model == nullptr)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)key.model);
		hash = HashCombine64(hash, (uint64_t)key.geometryState);
		return hash;
	}

	uint64_t BuildVoxelMaterialVariantKeyHash(const VoxelMaterialVariantKey& key)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, key.voxelTexture != nullptr ? (uint64_t)(uint32_t)key.voxelTexture->GetID().GetIndex() : 0ull);
		hash = HashCombine64(hash, key.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)key.emissiveSourceTexture->GetID().GetIndex() : 0ull);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.palette);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.shade);
		hash = HashCombine64(hash, (uint64_t)key.alphaBits);
		hash = HashCombine64(hash, (uint64_t)key.materialFlags);
		return hash;
	}

	uint64_t BuildVoxelInstanceKeyHash(const VoxelInstanceKey& key)
	{
		if (key.actorIndex < 0 || key.actorPtr == nullptr)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.actorIndex);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)key.actorPtr);
		hash = HashCombine64(hash, (uint64_t)key.actorGeneration);
		return hash;
	}

	bool TryBuildVoxelActorIdentity(const HWSprite& sprite, VoxelActorCacheLookup& lookup)
	{
		if (sprite.Sprite == nullptr || sprite.Sprite->ownerActor == nullptr || sprite.voxel == nullptr || sprite.voxel->model == nullptr)
		{
			return false;
		}

		const int32_t actorIndex = (int32_t)sprite.Sprite->ownerActor->GetIndex();
		if (actorIndex < 0)
		{
			return false;
		}

		const VoxelInstanceKey instanceKey = BuildVoxelInstanceKey(actorIndex, sprite.Sprite->ownerActor);
		lookup.identityKey = BuildVoxelInstanceKeyHash(instanceKey);
		if (lookup.identityKey == 0)
		{
			return false;
		}

		lookup.actorIndex = actorIndex;
		lookup.actorPtr = (uintptr_t)sprite.Sprite->ownerActor;
		lookup.voxelPtr = (uintptr_t)sprite.voxel;
		lookup.voxelModelPtr = (uintptr_t)sprite.voxel->model;
		lookup.sourcePicnum = sprite.Sprite->spritetexture().GetIndex();
		lookup.instanceKeyHash = lookup.identityKey;
		return true;
	}

	uint64_t BuildVoxelActorMeshKeyHash(const HWSprite& sprite)
	{
		return BuildVoxelMeshVariantKeyHash(BuildVoxelMeshVariantKey(sprite));
	}

	uint64_t BuildVoxelActorSurfaceSignature(const HWSprite& sprite)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel->model);

		if (sprite.Sprite != nullptr)
		{
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat);
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat2);
		}

		return hash;
	}

	uint64_t BuildVoxelActorTransformBasisSignature(const HWSprite& sprite)
	{
		uint64_t hash = 1469598103934665603ull;
		const FLOATTYPE* matrix = sprite.rotmat.get();
		for (int i = 0; i < 16; ++i)
		{
			if (i == 12 || i == 13 || i == 14)
			{
				continue;
			}
			hash = HashCombine64(hash, QuantizeSignatureFloat((double)matrix[i], 4096.0));
		}
		return hash;
	}

	void CopyVoxelActorTranslation(const HWSprite& sprite, float outTranslation[3])
	{
		const FLOATTYPE* matrix = sprite.rotmat.get();
		outTranslation[0] = (float)matrix[12];
		outTranslation[1] = (float)matrix[13];
		outTranslation[2] = (float)matrix[14];
	}

	void CopyVoxelActorTransform(const HWSprite& sprite, float outTransform[12])
	{
		const FLOATTYPE* matrix = sprite.rotmat.get();
		outTransform[0] = (float)matrix[0];
		outTransform[1] = (float)matrix[4];
		outTransform[2] = (float)matrix[8];
		outTransform[3] = (float)matrix[12];
		outTransform[4] = (float)matrix[1];
		outTransform[5] = (float)matrix[5];
		outTransform[6] = (float)matrix[9];
		outTransform[7] = (float)matrix[13];
		outTransform[8] = (float)matrix[2];
		outTransform[9] = (float)matrix[6];
		outTransform[10] = (float)matrix[10];
		outTransform[11] = (float)matrix[14];
	}

	bool SameVoxelTransform(const float a[12], const float b[12])
	{
		constexpr float Epsilon = 0.0001f;
		for (size_t i = 0; i < 12; ++i)
		{
			if (std::abs(a[i] - b[i]) > Epsilon)
			{
				return false;
			}
		}
		return true;
	}

	void FillVoxelTranslationInstanceTransform(const float currentTranslation[3], const float bakedTranslation[3], float outTransform[12])
	{
		outTransform[0] = 1.0f;
		outTransform[1] = 0.0f;
		outTransform[2] = 0.0f;
		outTransform[3] = currentTranslation[0] - bakedTranslation[0];
		outTransform[4] = 0.0f;
		outTransform[5] = 1.0f;
		outTransform[6] = 0.0f;
		outTransform[7] = currentTranslation[1] - bakedTranslation[1];
		outTransform[8] = 0.0f;
		outTransform[9] = 0.0f;
		outTransform[10] = 1.0f;
		outTransform[11] = currentTranslation[2] - bakedTranslation[2];
	}

	uint64_t BuildVoxelActorMaterialSignature(FGameTexture* voxelTexture, const MaterialRef& material)
	{
		return BuildVoxelMaterialVariantKeyHash(BuildVoxelMaterialVariantKey(voxelTexture, material));
	}

	uint64_t BuildVoxelActorSignature(uint64_t geometrySignature, uint64_t materialSignature)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, geometrySignature);
		hash = HashCombine64(hash, materialSignature);
		return hash;
	}

	const char* GetVoxelActorPendingReasonName(VoxelActorPendingReason reason)
	{
		switch (reason)
		{
		case VoxelActorPendingReason::ActorBudget: return "actor-budget";
		case VoxelActorPendingReason::MeshDeferred: return "mesh-deferred";
		case VoxelActorPendingReason::TriangleBudget: return "triangle-budget";
		case VoxelActorPendingReason::SurfaceBuildFailed: return "surface-build-failed";
		case VoxelActorPendingReason::ActorNotLive: return "actor-not-live";
		default: return "none";
		}
	}

	DynamicVoxelEscapeReason GetDynamicVoxelEscapeReasonForPending(VoxelActorPendingReason reason)
	{
		switch (reason)
		{
		case VoxelActorPendingReason::ActorBudget: return DynamicVoxelEscapeReason::ActorBudget;
		case VoxelActorPendingReason::MeshDeferred:
		case VoxelActorPendingReason::TriangleBudget: return DynamicVoxelEscapeReason::BuildBudget;
		case VoxelActorPendingReason::SurfaceBuildFailed: return DynamicVoxelEscapeReason::MissingSurface;
		case VoxelActorPendingReason::ActorNotLive: return DynamicVoxelEscapeReason::LifecycleTransient;
		default: return DynamicVoxelEscapeReason::VariantPending;
		}
	}

	bool IsDynamicVoxelEscapeEligibleForPersistent(DynamicVoxelEscapeReason reason)
	{
		switch (reason)
		{
		case DynamicVoxelEscapeReason::CameraOrWeaponSpecial:
		case DynamicVoxelEscapeReason::LifecycleTransient:
		case DynamicVoxelEscapeReason::ValidationQuarantine:
			return false;
		default:
			return true;
		}
	}

	bool IsDynamicVoxelEscapeForcedDynamic(DynamicVoxelEscapeReason reason)
	{
		return reason == DynamicVoxelEscapeReason::CameraOrWeaponSpecial ||
			reason == DynamicVoxelEscapeReason::LifecycleTransient;
	}

	bool IsExpectedDynamicVoxelEscape(DynamicVoxelEscapeReason reason)
	{
		switch (reason)
		{
		case DynamicVoxelEscapeReason::CameraOrWeaponSpecial:
		case DynamicVoxelEscapeReason::LifecycleTransient:
		case DynamicVoxelEscapeReason::ValidationQuarantine:
		case DynamicVoxelEscapeReason::MissingSurface:
			return true;
		default:
			return false;
		}
	}

	const char* GetVoxelMeshBakeSpaceName(VoxelMeshBakeSpace bakeSpace)
	{
		switch (bakeSpace)
		{
		case VoxelMeshBakeSpace::LocalSpace: return "local";
		case VoxelMeshBakeSpace::BakedTransform: return "baked";
		default: return "unknown";
		}
	}

	bool IsVoxelMeshTransformKeyed(VoxelMeshBakeSpace bakeSpace)
	{
		return bakeSpace != VoxelMeshBakeSpace::LocalSpace;
	}

	const char* GetVoxelActorStabilityName(VoxelActorStability stability)
	{
		switch (stability)
		{
		case VoxelActorStability::New: return "new";
		case VoxelActorStability::Stable: return "stable";
		case VoxelActorStability::TransformRebake: return "transform-rebake";
		case VoxelActorStability::Changed: return "changed";
		default: return "uncacheable";
		}
	}

	void EmitVoxelActorStateTrace(
		const HWSprite* sprite,
		const VoxelActorCacheLookup* lookup,
		const VoxelActorCacheEntry* entry,
		const char* action,
		VoxelActorPendingReason reason = VoxelActorPendingReason::None)
	{
		if (!nri_voxelstats)
		{
			return;
		}

		const int actorIndex =
			lookup != nullptr ? lookup->actorIndex :
			entry != nullptr ? entry->actorIndex :
			sprite != nullptr && sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr ? sprite->Sprite->ownerActor->GetIndex() :
			-1;
		const int statnum = sprite != nullptr && sprite->Sprite != nullptr ? sprite->Sprite->statnum : -1;
		const int picnum = sprite != nullptr && sprite->Sprite != nullptr ? sprite->Sprite->picnum : -1;
		const uint64_t meshKey =
			lookup != nullptr && lookup->meshKeyHash != 0 ? lookup->meshKeyHash :
			entry != nullptr ? entry->meshKeyHash :
			0;
		const uint64_t materialKey =
			lookup != nullptr && lookup->materialKeyHash != 0 ? lookup->materialKeyHash :
			entry != nullptr ? entry->materialKeyHash :
			0;
		const uint64_t instanceKey =
			lookup != nullptr && lookup->instanceKeyHash != 0 ? lookup->instanceKeyHash :
			entry != nullptr ? entry->instanceKeyHash :
			0;
		const uint64_t surfaceSignature =
			lookup != nullptr && lookup->surfaceSignature != 0 ? lookup->surfaceSignature :
			entry != nullptr ? entry->surfaceSignature :
			0;
		const uint64_t desiredMeshKey = entry != nullptr ? entry->desiredMeshKeyHash : 0;
		const uint64_t desiredMaterialKey = entry != nullptr ? entry->desiredMaterialKeyHash : 0;
		const uint64_t meshVariant =
			lookup != nullptr && lookup->meshVariantHash != 0 ? lookup->meshVariantHash :
			entry != nullptr ? entry->meshVariantHash :
			0;
		const uint64_t materialVariant =
			lookup != nullptr && lookup->materialVariantHash != 0 ? lookup->materialVariantHash :
			entry != nullptr ? entry->materialVariantHash :
			0;
		const uint64_t desiredMeshVariant = entry != nullptr ? entry->desiredMeshVariantHash : 0;
		const uint64_t desiredMaterialVariant = entry != nullptr ? entry->desiredMaterialVariantHash : 0;
		const uint64_t desiredSurfaceSignature = entry != nullptr ? entry->desiredSurfaceSignature : 0;
		const uint64_t transformBasisSignature =
			lookup != nullptr && lookup->transformBasisSignature != 0 ? lookup->transformBasisSignature :
			entry != nullptr ? entry->transformBasisSignature :
			0;
		const VoxelMeshBakeSpace meshBakeSpace =
			lookup != nullptr && lookup->meshBakeSpace != VoxelMeshBakeSpace::Unknown ? lookup->meshBakeSpace :
			entry != nullptr ? entry->meshBakeSpace :
			VoxelMeshBakeSpace::Unknown;
		const int32_t resolvedVoxelIndex =
			lookup != nullptr && lookup->resolvedVoxelIndex >= 0 ? lookup->resolvedVoxelIndex :
			entry != nullptr ? entry->resolvedVoxelIndex :
			-1;
		const bool hasSurface = entry != nullptr && entry->hasSurface;
		const bool persistentReady = entry != nullptr && entry->persistentReady;
		const VoxelActorPendingReason pendingReason =
			entry != nullptr ? (VoxelActorPendingReason)entry->pendingReason : VoxelActorPendingReason::None;
		const char* readyState =
			persistentReady ? "persistent" :
			hasSurface ? "surface-only" :
			pendingReason != VoxelActorPendingReason::None ? "pending" :
			"missing";
		const uint64_t pendingAge =
			entry != nullptr && entry->pendingFrame != 0 && gVoxelActorCacheFrame >= entry->pendingFrame ?
			gVoxelActorCacheFrame - entry->pendingFrame :
			0;
		const uint64_t surfaceAge =
			entry != nullptr && entry->surfaceFrame != 0 && gVoxelActorCacheFrame >= entry->surfaceFrame ?
			gVoxelActorCacheFrame - entry->surfaceFrame :
			0;
		const uint32_t primitiveCount = entry != nullptr ? entry->primitiveCount : 0u;

		Printf("PERF pt voxel actor state NRI: frame=%llu actor=%d stat=%d pic=%d action=%s reason=%s stability=%s mesh_key=0x%llx mat_key=0x%llx mesh_variant=0x%llx mat_variant=0x%llx inst_key=0x%llx voxel_index=%d basis_sig=0x%llx space=%s transform_keyed=%u surface_sig=0x%llx desired_mesh=0x%llx desired_mat=0x%llx desired_mesh_variant=0x%llx desired_mat_variant=0x%llx desired_surface=0x%llx persistent=%u has_surface=%u ready=%s prims=%u pending=%s pending_age=%llu surface_age=%llu last_seen=%llu\n",
			(unsigned long long)gVoxelActorCacheFrame,
			actorIndex,
			statnum,
			picnum,
			action != nullptr ? action : "unknown",
			GetVoxelActorPendingReasonName(reason),
			lookup != nullptr ? GetVoxelActorStabilityName(lookup->stability) : "n/a",
			(unsigned long long)meshKey,
			(unsigned long long)materialKey,
			(unsigned long long)meshVariant,
			(unsigned long long)materialVariant,
			(unsigned long long)instanceKey,
			resolvedVoxelIndex,
			(unsigned long long)transformBasisSignature,
			GetVoxelMeshBakeSpaceName(meshBakeSpace),
			IsVoxelMeshTransformKeyed(meshBakeSpace) ? 1u : 0u,
			(unsigned long long)surfaceSignature,
			(unsigned long long)desiredMeshKey,
			(unsigned long long)desiredMaterialKey,
			(unsigned long long)desiredMeshVariant,
			(unsigned long long)desiredMaterialVariant,
			(unsigned long long)desiredSurfaceSignature,
			persistentReady ? 1u : 0u,
			hasSurface ? 1u : 0u,
			readyState,
			primitiveCount,
			GetVoxelActorPendingReasonName(pendingReason),
			(unsigned long long)pendingAge,
			(unsigned long long)surfaceAge,
			(unsigned long long)(entry != nullptr ? entry->lastSeenFrame : 0));
	}

	void EmitVoxelActorKeyTrace(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, const char* action, VoxelActorPendingReason reason = VoxelActorPendingReason::None)
	{
		EmitVoxelActorStateTrace(&sprite, &lookup, lookup.entry, action, reason);
		if ((int)nri_ptactorspritetrace <= 0 || (int)nri_pttraceframes <= 0 || screen == nullptr ||
			sprite.Sprite == nullptr || sprite.Sprite->ownerActor == nullptr)
		{
			return;
		}

		PathTracingActorSpriteTraceEvent event = {};
		event.stage = PathTracingActorSpriteTraceStage::CaptureScene;
		event.actorIndex = sprite.Sprite->ownerActor->GetIndex();
		event.spriteStatnum = sprite.Sprite->statnum;
		event.spritePicnum = sprite.Sprite->picnum;
		event.baseTextureId = sprite.Sprite->spritetexture().GetIndex();
		event.resolvedTextureId = sprite.voxel != nullptr && sprite.voxel->model != nullptr ? sprite.voxel->model->GetPaletteTexture().GetIndex() : -1;
		event.palette = sprite.palette;
		event.shade = sprite.shade;
		event.cstat = sprite.Sprite->cstat;
		event.cstat2 = sprite.Sprite->cstat2;
		event.drawListType = GLDL_MODELS;
		event.noAnimate = (sprite.Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
		event.fullbright = (sprite.Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
		event.resolvedGameTexture = nullptr;
		event.hasVoxelKeys = true;
		event.voxelMeshKeyHash = lookup.meshKeyHash;
		event.voxelMaterialKeyHash = lookup.materialKeyHash;
		event.voxelInstanceKeyHash = lookup.instanceKeyHash;
		event.voxelSurfaceSignature = lookup.surfaceSignature;
		event.voxelAction = action;
		screen->EmitPathTracingActorSpriteTraceEvent(event);
	}

	void InitializeVoxelActorCacheEntryIdentity(VoxelActorCacheEntry& entry, const VoxelActorCacheLookup& lookup)
	{
		entry.identityKey = lookup.identityKey;
		entry.actorIndex = lookup.actorIndex;
		entry.actorPtr = lookup.actorPtr;
		entry.voxelPtr = lookup.voxelPtr;
		entry.voxelModelPtr = lookup.voxelModelPtr;
		entry.sourcePicnum = lookup.sourcePicnum;
		entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
		entry.instanceKeyHash = lookup.instanceKeyHash;
		entry.meshBakeSpace = lookup.meshBakeSpace;
	}

	bool HasLastValidResidentVoxelSurface(const VoxelActorCacheLookup& lookup)
	{
		return lookup.entry != nullptr && lookup.entry->hasSurface && lookup.entry->persistentReady;
	}

	bool IsVoxelMeshVariantSurfaceReady(uint64_t meshVariantHash);

	bool IsVoxelActorSharedVariantReady(const VoxelActorCacheEntry& entry)
	{
		return entry.hasSurface &&
			entry.meshBakeSpace == VoxelMeshBakeSpace::LocalSpace &&
			IsVoxelMeshVariantSurfaceReady(entry.meshVariantHash);
	}

	bool CanPromoteVoxelActorCacheEntry(const VoxelActorCacheEntry& entry)
	{
		if (entry.persistentReady)
		{
			return true;
		}
		if (IsVoxelActorSharedVariantReady(entry))
		{
			return true;
		}
		const int configuredPromoteFrames = (int)nri_ptvoxelpersistentpromoteframes;
		const int promoteFrames = configuredPromoteFrames > 0 ? configuredPromoteFrames : 0;
		if (promoteFrames == 0 || entry.surfaceFrame == 0)
		{
			return true;
		}
		return gVoxelActorCacheFrame >= entry.surfaceFrame &&
			gVoxelActorCacheFrame - entry.surfaceFrame >= (uint64_t)promoteFrames;
	}

	void MarkVoxelActorVariantPending(const VoxelActorCacheLookup& lookup, VoxelActorPendingReason reason)
	{
		if (lookup.identityKey == 0)
		{
			return;
		}

		VoxelActorCacheEntry& entry = lookup.entry != nullptr ? *lookup.entry : gVoxelActorCache[lookup.identityKey];
		InitializeVoxelActorCacheEntryIdentity(entry, lookup);
		entry.desiredSignature = lookup.signature;
		entry.desiredMeshKeyHash = lookup.meshKeyHash;
		entry.desiredMaterialKeyHash = lookup.materialKeyHash;
		entry.desiredMeshVariantHash = lookup.meshVariantHash;
		entry.desiredMaterialVariantHash = lookup.materialVariantHash;
		entry.desiredSurfaceSignature = lookup.surfaceSignature;
		entry.pendingReason = (uint8_t)reason;
		entry.pendingFrame = gVoxelActorCacheFrame;
		entry.lastSeenFrame = gVoxelActorCacheFrame;
		EmitVoxelActorStateTrace(nullptr, &lookup, &entry, "variant-build-queued", reason);
	}

	void TraceVoxelActorFallbackLastValid(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, VoxelActorPendingReason reason)
	{
		EmitVoxelActorKeyTrace(sprite, lookup, "fallback-last-valid", reason);
	}

	void TraceVoxelActorFirstUseFallback(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, VoxelActorPendingReason reason)
	{
		EmitVoxelActorKeyTrace(sprite, lookup, "fallback-empty", reason);
	}

	VoxelActorCacheLookup TrackVoxelActorSignature(const HWSprite& sprite, FGameTexture* voxelTexture, const MaterialRef& material, SceneDebugStats& stats)
	{
		VoxelActorCacheLookup lookup = {};
		if (!TryBuildVoxelActorIdentity(sprite, lookup))
		{
			stats.voxelStableUncacheable++;
			stats.voxelStableSplitLive++;
			return lookup;
		}

		stats.voxelStableCandidates++;
		const uint64_t surfaceSignature = BuildVoxelActorSurfaceSignature(sprite);
		const uint64_t transformBasisSignature = BuildVoxelActorTransformBasisSignature(sprite);
		const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
		const VoxelMaterialVariantKey materialVariantKey = BuildVoxelMaterialVariantKey(voxelTexture, material);
		const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
		const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(materialVariantKey);
		const uint64_t meshKeyHash = meshVariantHash;
		const uint64_t geometrySignature = meshVariantHash;
		const uint64_t materialSignature = materialVariantHash;
		const uint64_t signature = BuildVoxelActorSignature(geometrySignature, materialSignature);
		lookup.signature = signature;
		lookup.geometrySignature = geometrySignature;
		lookup.surfaceSignature = surfaceSignature;
		lookup.transformBasisSignature = transformBasisSignature;
		lookup.materialSignature = materialSignature;
		lookup.meshKeyHash = meshKeyHash;
		lookup.materialKeyHash = materialSignature;
		lookup.meshVariantHash = meshVariantHash;
		lookup.materialVariantHash = materialVariantHash;
		lookup.meshBakeSpace = VoxelMeshBakeSpace::LocalSpace;
		lookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		CopyVoxelActorTranslation(sprite, lookup.currentTranslation);
		CopyVoxelActorTransform(sprite, lookup.currentTransform);
		auto found = gVoxelActorCache.find(lookup.identityKey);
		if (found == gVoxelActorCache.end())
		{
			stats.voxelStableSignatureMisses++;
			stats.voxelStableSplitLive++;
			lookup.stability = VoxelActorStability::New;
			EmitVoxelActorKeyTrace(sprite, lookup, "variant-miss");
			return lookup;
		}

		lookup.entry = &found->second;
		lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
		InitializeVoxelActorCacheEntryIdentity(*lookup.entry, lookup);
		lookup.entry->desiredSignature = signature;
		lookup.entry->desiredMeshKeyHash = meshKeyHash;
		lookup.entry->desiredMaterialKeyHash = materialSignature;
		lookup.entry->desiredMeshVariantHash = meshVariantHash;
		lookup.entry->desiredMaterialVariantHash = materialVariantHash;
		lookup.entry->desiredSurfaceSignature = surfaceSignature;
		const bool canUpdateByTranslationInstance =
			lookup.entry->hasSurface &&
			lookup.entry->persistentReady &&
			lookup.entry->geometrySignature == geometrySignature &&
			lookup.entry->materialSignature == materialSignature &&
			!SameVoxelTransform(lookup.entry->currentTransform, lookup.currentTransform);
		if (canUpdateByTranslationInstance)
		{
			lookup.entry->signature = signature;
			lookup.entry->surfaceSignature = surfaceSignature;
			lookup.entry->transformBasisSignature = transformBasisSignature;
			lookup.entry->meshBakeSpace = lookup.meshBakeSpace;
			lookup.entry->currentTranslation[0] = lookup.currentTranslation[0];
			lookup.entry->currentTranslation[1] = lookup.currentTranslation[1];
			lookup.entry->currentTranslation[2] = lookup.currentTranslation[2];
			std::copy(std::begin(lookup.currentTransform), std::end(lookup.currentTransform), std::begin(lookup.entry->currentTransform));
			lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			stats.voxelStableSignatureChanges++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			++gVoxelActorCacheSerial;
			EmitVoxelActorKeyTrace(sprite, lookup, "transform-instance");
			return lookup;
		}
		if (lookup.entry->signature == signature && lookup.entry->surfaceSignature == surfaceSignature && lookup.entry->hasSurface)
		{
			const bool promoted = !lookup.entry->persistentReady;
			if (!lookup.entry->persistentReady && CanPromoteVoxelActorCacheEntry(*lookup.entry))
			{
				lookup.entry->persistentReady = true;
				++gVoxelActorCacheSerial;
			}
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			stats.voxelStableSignatureHits++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			EmitVoxelActorKeyTrace(sprite, lookup, promoted ? (lookup.entry->persistentReady ? "promote" : "promote-deferred") : "hit");
			return lookup;
		}

		if (lookup.entry->geometrySignature == geometrySignature && lookup.entry->surfaceSignature == surfaceSignature && lookup.entry->hasSurface)
		{
			lookup.entry->signature = signature;
			lookup.entry->materialSignature = materialSignature;
			lookup.entry->materialKeyHash = lookup.materialKeyHash;
			lookup.entry->materialVariantHash = lookup.materialVariantHash;
			lookup.entry->meshBakeSpace = lookup.meshBakeSpace;
			lookup.entry->surface.material = material;
			lookup.entry->lightSurface.material = material;
			lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
			const bool promoted = !lookup.entry->persistentReady;
			if (!lookup.entry->persistentReady && CanPromoteVoxelActorCacheEntry(*lookup.entry))
			{
				lookup.entry->persistentReady = true;
			}
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			if (!promoted || lookup.entry->persistentReady)
			{
				++gVoxelActorCacheSerial;
			}
			stats.voxelStableSignatureChanges++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			EmitVoxelActorKeyTrace(sprite, lookup, promoted ? (lookup.entry->persistentReady ? "promote" : "promote-deferred") : "material-only");
			return lookup;
		}

		stats.voxelStableSignatureChanges++;
		stats.voxelStableSplitLive++;
		if (lookup.entry->geometrySignature == geometrySignature)
		{
			lookup.stability = VoxelActorStability::TransformRebake;
			const bool materialChanged = lookup.entry->materialSignature != materialSignature;
			EmitVoxelActorKeyTrace(sprite, lookup, materialChanged ? "transform-material" : "transform-only");
		}
		else
		{
			lookup.stability = VoxelActorStability::Changed;
			EmitVoxelActorKeyTrace(sprite, lookup, IsVoxelMeshVariantSurfaceReady(lookup.meshVariantHash) ? "variant-hit" : "variant-miss");
			EmitVoxelActorKeyTrace(sprite, lookup, "variant-switch");
		}
		return lookup;
	}

	bool TryConsumeReadOnlyVoxelActorCacheSurface(const HWSprite& sprite, FGameTexture* voxelTexture, const MaterialRef& material, SceneDebugStats& stats)
	{
		VoxelActorCacheLookup lookup = {};
		if (!TryBuildVoxelActorIdentity(sprite, lookup))
		{
			stats.voxelStableUncacheable++;
			return false;
		}

		stats.voxelStableCandidates++;
		const uint64_t surfaceSignature = BuildVoxelActorSurfaceSignature(sprite);
		const uint64_t transformBasisSignature = BuildVoxelActorTransformBasisSignature(sprite);
		const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
		const VoxelMaterialVariantKey materialVariantKey = BuildVoxelMaterialVariantKey(voxelTexture, material);
		const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
		const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(materialVariantKey);
		const uint64_t meshKeyHash = meshVariantHash;
		const uint64_t geometrySignature = meshVariantHash;
		const uint64_t materialSignature = materialVariantHash;
		const uint64_t signature = BuildVoxelActorSignature(geometrySignature, materialSignature);
		lookup.signature = signature;
		lookup.geometrySignature = geometrySignature;
		lookup.surfaceSignature = surfaceSignature;
		lookup.transformBasisSignature = transformBasisSignature;
		lookup.materialSignature = materialSignature;
		lookup.meshKeyHash = meshKeyHash;
		lookup.materialKeyHash = materialSignature;
		lookup.meshVariantHash = meshVariantHash;
		lookup.materialVariantHash = materialVariantHash;
		lookup.meshBakeSpace = VoxelMeshBakeSpace::LocalSpace;
		lookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		CopyVoxelActorTransform(sprite, lookup.currentTransform);
		auto found = gVoxelActorCache.find(lookup.identityKey);
		if (found == gVoxelActorCache.end() || !found->second.hasSurface)
		{
			stats.voxelStableSignatureMisses++;
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-miss");
			return false;
		}

		const VoxelActorCacheEntry& entry = found->second;
		if (entry.geometrySignature != geometrySignature)
		{
			stats.voxelStableSignatureChanges++;
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-fallback-last-valid");
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			return true;
		}

		if (entry.signature != signature)
		{
			stats.voxelStableSignatureChanges++;
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-material");
		}
		else if (entry.surfaceSignature != surfaceSignature)
		{
			stats.voxelStableSignatureChanges++;
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-transform");
		}
		else
		{
			stats.voxelStableSignatureHits++;
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-hit");
		}

		stats.voxelStableSplitStable++;
		stats.voxelCacheSurfaceHits++;
		return true;
	}

	void NormalizeCachedSurfacePreviousPositions(SurfaceRef& surface)
	{
		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
		}
	}

	uint32_t CountSurfacePrimitives(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	void StoreVoxelActorCacheSurface(
		const VoxelActorCacheLookup& lookup,
		const SurfaceRef& meshSurface,
		const SurfaceRef& lightSurface,
		VoxelMeshBakeSpace meshBakeSpace,
		bool sharedVariantReady,
		SceneDebugStats& stats)
	{
		if (lookup.identityKey == 0)
		{
			return;
		}

		VoxelActorCacheEntry& entry = gVoxelActorCache[lookup.identityKey];
		const bool hadSurface = entry.hasSurface;
		const bool wasPersistentReady = entry.persistentReady;
		entry.signature = lookup.signature;
		entry.geometrySignature = lookup.geometrySignature;
		entry.surfaceSignature = lookup.surfaceSignature;
		entry.bakedSurfaceSignature = lookup.surfaceSignature;
		entry.materialSignature = lookup.materialSignature;
		entry.transformBasisSignature = lookup.transformBasisSignature;
		entry.meshKeyHash = lookup.meshKeyHash;
		entry.materialKeyHash = lookup.materialKeyHash;
		entry.meshVariantHash = lookup.meshVariantHash;
		entry.materialVariantHash = lookup.materialVariantHash;
		InitializeVoxelActorCacheEntryIdentity(entry, lookup);
		entry.meshBakeSpace = meshBakeSpace;
		entry.desiredSignature = lookup.signature;
		entry.desiredMeshKeyHash = lookup.meshKeyHash;
		entry.desiredMaterialKeyHash = lookup.materialKeyHash;
		entry.desiredMeshVariantHash = lookup.meshVariantHash;
		entry.desiredMaterialVariantHash = lookup.materialVariantHash;
		entry.desiredSurfaceSignature = lookup.surfaceSignature;
		entry.pendingReason = (uint8_t)VoxelActorPendingReason::None;
		entry.pendingFrame = 0;
		entry.surface = meshSurface;
		entry.surface.material = lightSurface.material;
		entry.surface.provenance = lightSurface.provenance;
		NormalizeCachedSurfacePreviousPositions(entry.surface);
		entry.lightSurface = lightSurface;
		NormalizeCachedSurfacePreviousPositions(entry.lightSurface);
		entry.lastSeenFrame = gVoxelActorCacheFrame;
		entry.surfaceFrame = gVoxelActorCacheFrame;
		entry.primitiveCount = CountSurfacePrimitives(entry.surface);
		entry.currentTranslation[0] = lookup.currentTranslation[0];
		entry.currentTranslation[1] = lookup.currentTranslation[1];
		entry.currentTranslation[2] = lookup.currentTranslation[2];
		entry.bakedTranslation[0] = lookup.currentTranslation[0];
		entry.bakedTranslation[1] = lookup.currentTranslation[1];
		entry.bakedTranslation[2] = lookup.currentTranslation[2];
		std::copy(std::begin(lookup.currentTransform), std::end(lookup.currentTransform), std::begin(entry.currentTransform));
		// Transform rebakes and state variant switches are transitional updates of an
		// already valid actor. Keep already-resident actors renderable and let the
		// persistent actor path update the resource in place. First-use actors still
		// wait for the normal stable-frame promotion path unless the shared canonical
		// variant is ready, in which case actor promotion latency must not force a
		// large voxel through the dynamic overlay.
		entry.persistentReady =
			sharedVariantReady ||
			(wasPersistentReady &&
				(lookup.stability == VoxelActorStability::TransformRebake ||
				 lookup.stability == VoxelActorStability::Changed));
		entry.hasSurface = true;
		++gVoxelActorCacheSerial;
		if (sharedVariantReady && !wasPersistentReady)
		{
			EmitVoxelActorStateTrace(nullptr, &lookup, &entry, "shared-variant-promote", VoxelActorPendingReason::None);
		}

		if (lookup.stability == VoxelActorStability::New || !hadSurface)
		{
			stats.voxelCacheSurfaceStores++;
		}
		else if (lookup.stability == VoxelActorStability::TransformRebake)
		{
			stats.voxelCacheTransformRebakes++;
		}
		else if (lookup.stability == VoxelActorStability::Changed)
		{
			stats.voxelCacheSurfaceRebuilds++;
		}
	}

	bool IsLiveActorVoxelCacheOwner(DCoreActor* actor)
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}
		if ((actor->sprext.renderflags & (SPREXT_NOTMD | SPREXT_TEMPINVISIBLE)) != 0 ||
			(actor->spr.cstat2 & CSTAT2_SPRITE_NOMODEL) != 0 ||
			(actor->spr.cstat & CSTAT_SPRITE_INVISIBLE) != 0)
		{
			return false;
		}
		return tilehasvoxel(actor->spr.spritetexture()) != 0;
	}

	bool IsLiveActorVoxelWarmupCandidate(DCoreActor* actor)
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}
		if ((actor->sprext.renderflags & (SPREXT_NOTMD | SPREXT_TEMPINVISIBLE)) != 0 ||
			(actor->spr.cstat2 & CSTAT2_SPRITE_NOMODEL) != 0 ||
			(actor->spr.cstat & CSTAT_SPRITE_INVISIBLE) != 0 ||
			actor->spr.scale.X == 0.0 ||
			actor->spr.scale.Y == 0.0)
		{
			return false;
		}
		return true;
	}

	void AddUniqueLoadingActorTextureCandidate(FTextureID texid, std::vector<FTextureID>& candidates, std::unordered_set<int>& seenTextureIds)
	{
		if (!texid.isValid())
		{
			return;
		}

		const int textureId = texid.GetIndex();
		if (seenTextureIds.insert(textureId).second)
		{
			candidates.push_back(texid);
		}
	}

	void BuildLoadingActorTextureCandidates(DCoreActor* actor, std::vector<FTextureID>& candidates)
	{
		candidates.clear();
		if (actor == nullptr)
		{
			return;
		}

		std::unordered_set<int> seenTextureIds;
		auto addBaseAndAnimated = [&](FTextureID texid)
		{
			AddUniqueLoadingActorTextureCandidate(texid, candidates, seenTextureIds);
			if (texid.isValid() && (actor->spr.cstat2 & CSTAT2_SPRITE_NOANIMATE) == 0)
			{
				FTextureID animatedTexid = texid;
				tileUpdatePicnum(animatedTexid, actor->GetIndex() & 16383);
				AddUniqueLoadingActorTextureCandidate(animatedTexid, candidates, seenTextureIds);
			}
		};

		addBaseAndAnimated(actor->spr.spritetexture());
		addBaseAndAnimated(actor->dispictex);
	}

	float GetLoadingActorAlpha(DCoreActor* actor)
	{
		if (actor == nullptr)
		{
			return 1.0f;
		}

		float alpha = 1.0f;
		if ((actor->spr.cstat & CSTAT_SPRITE_TRANSLUCENT) != 0)
		{
			alpha = GetAlphaFromBlend((actor->spr.cstat & CSTAT_SPRITE_TRANS_FLIP) ? DAMETH_TRANS2 : DAMETH_TRANS1, 0);
		}
		alpha *= 1.f - actor->sprext.alpha;
		return alpha;
	}

	void BuildLiveActorIdentityKeys(std::unordered_set<uint64_t>& outKeys)
	{
		outKeys.clear();
		if (!r_voxels)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (DCoreActor* actor = it.Next())
		{
			if (!IsLiveActorVoxelCacheOwner(actor))
			{
				continue;
			}

			const int32_t actorIndex = (int32_t)actor->GetIndex();
			if (actorIndex < 0)
			{
				continue;
			}

			// Cache identity is actor-based. Retain off-camera voxel-capable actors
			// for reflections, but do not let an actor that no longer resolves to a
			// voxel keep stale geometry alive at its last captured transform.
			const uint64_t identityKey = BuildVoxelInstanceKeyHash(BuildVoxelInstanceKey(actorIndex, actor));
			if (identityKey != 0)
			{
				outKeys.insert(identityKey);
			}
		}
	}

	bool BeginVoxelActorCacheFrame()
	{
		const bool rootCapture = gVoxelActorCacheCaptureDepth++ == 0;
		if (rootCapture)
		{
			++gVoxelActorCacheFrame;
			if (gVoxelActorCacheFrame == 0)
			{
				gVoxelActorCacheFrame = 1;
			}
		}
		return rootCapture;
	}

	uint64_t EstimateSurfaceVertexBytes(const SurfaceRef& surface)
	{
		return (uint64_t)surface.vertices.size() * (uint64_t)sizeof(CapturedVertex);
	}

	uint64_t EstimateSurfaceIndexBytes(const SurfaceRef& surface)
	{
		return (uint64_t)surface.indices.size() * (uint64_t)sizeof(uint32_t);
	}

	uint64_t EstimateSurfacePrimitiveBytes(uint32_t primitiveCount)
	{
		return (uint64_t)primitiveCount * (uint64_t)sizeof(PrimitiveData);
	}

	uint64_t EstimateSurfaceMaterialBytes(const SurfaceRef& surface)
	{
		return surface.vertices.empty() ? 0ull : (uint64_t)sizeof(MaterialRef);
	}

	void InsertDynamicVoxelEscapeTopEntry(
		std::array<DynamicVoxelEscapeTraceEntry, DynamicVoxelEscapeTraceCount>& entries,
		unsigned int& count,
		const DynamicVoxelEscapeTraceEntry& entry)
	{
		if (!entry.valid)
		{
			return;
		}

		size_t insertIndex = DynamicVoxelEscapeTraceCount;
		for (size_t i = 0; i < DynamicVoxelEscapeTraceCount; ++i)
		{
			const DynamicVoxelEscapeTraceEntry& current = entries[i];
			if (!current.valid ||
				entry.totalBytes > current.totalBytes ||
				(entry.totalBytes == current.totalBytes && entry.primitiveCount > current.primitiveCount))
			{
				insertIndex = i;
				break;
			}
		}
		if (insertIndex >= DynamicVoxelEscapeTraceCount)
		{
			return;
		}

		for (size_t i = DynamicVoxelEscapeTraceCount - 1; i > insertIndex; --i)
		{
			entries[i] = entries[i - 1];
		}
		entries[insertIndex] = entry;
		count = (unsigned int)(std::min<size_t>)(DynamicVoxelEscapeTraceCount, count + 1u);
	}

	void RecordDynamicVoxelEscape(
		SceneDebugStats& stats,
		const HWSprite& sprite,
		const VoxelActorCacheLookup& lookup,
		const SurfaceRef& surface,
		DynamicVoxelEscapeReason reason)
	{
		const uint32_t primitiveCount = CountSurfacePrimitives(surface);
		const uint64_t vertexBytes = EstimateSurfaceVertexBytes(surface);
		const uint64_t indexBytes = EstimateSurfaceIndexBytes(surface);
		const uint64_t primitiveBytes = EstimateSurfacePrimitiveBytes(primitiveCount);
		const uint64_t materialBytes = EstimateSurfaceMaterialBytes(surface);
		const uint64_t totalBytes = vertexBytes + indexBytes + primitiveBytes + materialBytes;

		stats.dynamicVoxelEscapeActorCount++;
		stats.dynamicVoxelEscapePrimitiveCount += primitiveCount;
		stats.dynamicVoxelEscapeVertexBytes += vertexBytes;
		stats.dynamicVoxelEscapeIndexBytes += indexBytes;
		stats.dynamicVoxelEscapePrimitiveBytes += primitiveBytes;
		stats.dynamicVoxelEscapeMaterialBytes += materialBytes;
		stats.dynamicVoxelEscapeTotalBytes += totalBytes;
		if (IsDynamicVoxelEscapeEligibleForPersistent(reason))
		{
			stats.dynamicVoxelEscapeEligibleActorCount++;
		}
		if (IsDynamicVoxelEscapeForcedDynamic(reason))
		{
			stats.dynamicVoxelEscapeForcedActorCount++;
		}
		if (IsExpectedDynamicVoxelEscape(reason))
		{
			stats.dynamicVoxelExpectedEscapeActorCount++;
			stats.dynamicVoxelExpectedEscapePrimitiveCount += primitiveCount;
			stats.dynamicVoxelExpectedEscapeTotalBytes += totalBytes;
		}
		else
		{
			stats.dynamicVoxelUnexpectedEscapeActorCount++;
			stats.dynamicVoxelUnexpectedEscapePrimitiveCount += primitiveCount;
			stats.dynamicVoxelUnexpectedEscapeTotalBytes += totalBytes;
		}

		DynamicVoxelEscapeTraceEntry entry = {};
		entry.valid = true;
		entry.reason = reason;
		entry.actorIndex =
			lookup.actorIndex >= 0 ? lookup.actorIndex :
			sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr ? sprite.Sprite->ownerActor->GetIndex() :
			-1;
		entry.statnum = sprite.Sprite != nullptr ? sprite.Sprite->statnum : -1;
		entry.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->picnum : -1;
		entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
		entry.meshVariantHash = lookup.meshVariantHash;
		entry.materialVariantHash = lookup.materialVariantHash;
		entry.primitiveCount = primitiveCount;
		entry.vertexBytes = vertexBytes;
		entry.indexBytes = indexBytes;
		entry.primitiveBytes = primitiveBytes;
		entry.materialBytes = materialBytes;
		entry.totalBytes = totalBytes;
		entry.persistentReady = lookup.entry != nullptr && lookup.entry->persistentReady;
		entry.hasCachedSurface = lookup.entry != nullptr && lookup.entry->hasSurface;
		InsertDynamicVoxelEscapeTopEntry(stats.dynamicVoxelEscapeTopEntries, stats.dynamicVoxelEscapeTopCount, entry);
		if (!IsExpectedDynamicVoxelEscape(reason))
		{
			InsertDynamicVoxelEscapeTopEntry(stats.dynamicVoxelUnexpectedEscapeTopEntries, stats.dynamicVoxelUnexpectedEscapeTopCount, entry);
		}
	}

	struct VoxelDuplicateVariantAggregate
	{
		uint64_t meshKeyHash = 0;
		uint64_t exampleBasisSignature = 0;
		int32_t sourcePicnum = -1;
		uint32_t actorCount = 0;
		uint32_t persistentActorCount = 0;
		uint32_t transformKeyedActorCount = 0;
		uint32_t localSpaceActorCount = 0;
		uint32_t bakedTransformActorCount = 0;
		uint32_t unknownSpaceActorCount = 0;
		uint32_t primitiveCountPerActor = 0;
		uint32_t totalDuplicatedPrimitives = 0;
		uint64_t duplicatedBytes = 0;
		std::unordered_set<uint64_t> basisSignatures;
	};

	void CollectVoxelActorCacheDuplicationStats(SceneDebugStats& stats)
	{
		stats.voxelCachePrimitives = 0;
		stats.voxelCacheActorSurfaces = 0;
		stats.voxelCacheUniqueMeshKeys = 0;
		stats.voxelCacheUniqueMaterialKeys = 0;
		stats.voxelCacheLocalSpaceSurfaces = 0;
		stats.voxelCacheBakedTransformSurfaces = 0;
		stats.voxelCacheUnknownSpaceSurfaces = 0;
		stats.voxelCacheTransformKeyedSurfaces = 0;
		stats.voxelCacheUniqueTransformBases = 0;
		stats.voxelCacheInvariantWarnings = 0;
		stats.voxelCacheDuplicatedVertexBytes = 0;
		stats.voxelCacheDuplicatedIndexBytes = 0;
		stats.voxelCacheDuplicatedPrimitiveBytes = 0;
		stats.voxelCacheDuplicatedTotalBytes = 0;
		stats.voxelCacheDuplicateTopCount = 0;
		stats.voxelCacheDuplicateTopEntries = {};

		std::unordered_set<uint64_t> meshKeys;
		std::unordered_set<uint64_t> materialKeys;
		std::unordered_set<uint64_t> basisSignatures;
		std::unordered_map<uint64_t, VoxelDuplicateVariantAggregate> meshAggregates;
		meshKeys.reserve(gVoxelActorCache.size());
		materialKeys.reserve(gVoxelActorCache.size());
		basisSignatures.reserve(gVoxelActorCache.size());
		meshAggregates.reserve(gVoxelActorCache.size());

		for (const auto& pair : gVoxelActorCache)
		{
			const VoxelActorCacheEntry& entry = pair.second;
			if (!entry.hasSurface)
			{
				continue;
			}

			const uint64_t vertexBytes = EstimateSurfaceVertexBytes(entry.surface);
			const uint64_t indexBytes = EstimateSurfaceIndexBytes(entry.surface);
			const uint64_t primitiveBytes = EstimateSurfacePrimitiveBytes(entry.primitiveCount);
			const uint64_t totalBytes = vertexBytes + indexBytes + primitiveBytes;

			stats.voxelCacheActorSurfaces++;
			stats.voxelCachePrimitives += entry.primitiveCount;
			stats.voxelCacheDuplicatedVertexBytes += vertexBytes;
			stats.voxelCacheDuplicatedIndexBytes += indexBytes;
			stats.voxelCacheDuplicatedPrimitiveBytes += primitiveBytes;
			stats.voxelCacheDuplicatedTotalBytes += totalBytes;
			basisSignatures.insert(entry.transformBasisSignature);

			switch (entry.meshBakeSpace)
			{
			case VoxelMeshBakeSpace::LocalSpace:
				stats.voxelCacheLocalSpaceSurfaces++;
				break;
			case VoxelMeshBakeSpace::BakedTransform:
				stats.voxelCacheBakedTransformSurfaces++;
				break;
			default:
				stats.voxelCacheUnknownSpaceSurfaces++;
				break;
			}
			if (IsVoxelMeshTransformKeyed(entry.meshBakeSpace))
			{
				stats.voxelCacheTransformKeyedSurfaces++;
			}

			const uint64_t meshVariantHash = entry.meshVariantHash != 0 ? entry.meshVariantHash : entry.meshKeyHash;
			if (meshVariantHash != 0)
			{
				meshKeys.insert(meshVariantHash);
				VoxelDuplicateVariantAggregate& aggregate = meshAggregates[meshVariantHash];
				if (aggregate.actorCount == 0)
				{
					aggregate.meshKeyHash = meshVariantHash;
					aggregate.exampleBasisSignature = entry.transformBasisSignature;
					aggregate.sourcePicnum = entry.sourcePicnum;
					aggregate.primitiveCountPerActor = entry.primitiveCount;
				}
				else if (aggregate.sourcePicnum != entry.sourcePicnum)
				{
					aggregate.sourcePicnum = -1;
				}
				aggregate.actorCount++;
				aggregate.persistentActorCount += entry.persistentReady ? 1u : 0u;
				aggregate.basisSignatures.insert(entry.transformBasisSignature);
				if (IsVoxelMeshTransformKeyed(entry.meshBakeSpace))
				{
					aggregate.transformKeyedActorCount++;
				}
				switch (entry.meshBakeSpace)
				{
				case VoxelMeshBakeSpace::LocalSpace:
					aggregate.localSpaceActorCount++;
					break;
				case VoxelMeshBakeSpace::BakedTransform:
					aggregate.bakedTransformActorCount++;
					break;
				default:
					aggregate.unknownSpaceActorCount++;
					break;
				}
				aggregate.primitiveCountPerActor = (std::max)(aggregate.primitiveCountPerActor, entry.primitiveCount);
				aggregate.totalDuplicatedPrimitives += entry.primitiveCount;
				aggregate.duplicatedBytes += totalBytes;
			}
			const uint64_t materialVariantHash = entry.materialVariantHash != 0 ? entry.materialVariantHash : entry.materialKeyHash;
			if (materialVariantHash != 0)
			{
				materialKeys.insert(materialVariantHash);
			}
		}

		stats.voxelCacheUniqueMeshKeys = (unsigned int)meshKeys.size();
		stats.voxelCacheUniqueMaterialKeys = (unsigned int)materialKeys.size();
		stats.voxelCacheUniqueTransformBases = (unsigned int)basisSignatures.size();

		std::vector<VoxelDuplicateVariantAggregate> aggregates;
		aggregates.reserve(meshAggregates.size());
		for (const auto& pair : meshAggregates)
		{
			if (pair.second.bakedTransformActorCount > 0 && pair.second.basisSignatures.size() > 1)
			{
				stats.voxelCacheInvariantWarnings++;
				if (nri_voxelstats)
				{
					Printf(
						"PERF pt voxel invariant warning NRI: frame=%llu mesh_key=0x%llx source_pic=%d actors=%u space=baked basis_count=%u example_basis=0x%llx\n",
						(unsigned long long)gVoxelActorCacheFrame,
						(unsigned long long)pair.second.meshKeyHash,
						pair.second.sourcePicnum,
						pair.second.actorCount,
						(uint32_t)pair.second.basisSignatures.size(),
						(unsigned long long)pair.second.exampleBasisSignature);
				}
			}
			if (pair.second.actorCount > 1)
			{
				aggregates.push_back(pair.second);
			}
		}
		std::sort(aggregates.begin(), aggregates.end(), [](const auto& a, const auto& b)
		{
			if (a.totalDuplicatedPrimitives != b.totalDuplicatedPrimitives)
			{
				return a.totalDuplicatedPrimitives > b.totalDuplicatedPrimitives;
			}
			if (a.actorCount != b.actorCount)
			{
				return a.actorCount > b.actorCount;
			}
			return a.meshKeyHash < b.meshKeyHash;
		});

		stats.voxelCacheDuplicateTopCount =
			(unsigned int)(std::min)(aggregates.size(), (size_t)VoxelDuplicateVariantTraceCount);
		for (uint32_t i = 0; i < stats.voxelCacheDuplicateTopCount; ++i)
		{
			VoxelDuplicateVariantTraceEntry& top = stats.voxelCacheDuplicateTopEntries[i];
			const VoxelDuplicateVariantAggregate& aggregate = aggregates[i];
			top.valid = true;
			top.meshKeyHash = aggregate.meshKeyHash;
			top.exampleBasisSignature = aggregate.exampleBasisSignature;
			top.sourcePicnum = aggregate.sourcePicnum;
			top.actorCount = aggregate.actorCount;
			top.persistentActorCount = aggregate.persistentActorCount;
			top.uniqueBasisSignatureCount = (uint32_t)aggregate.basisSignatures.size();
			top.transformKeyedActorCount = aggregate.transformKeyedActorCount;
			if (aggregate.localSpaceActorCount > 0)
			{
				top.bakeSpace = VoxelMeshBakeSpace::LocalSpace;
			}
			else if (aggregate.bakedTransformActorCount > 0)
			{
				top.bakeSpace = VoxelMeshBakeSpace::BakedTransform;
			}
			else
			{
				top.bakeSpace = VoxelMeshBakeSpace::Unknown;
			}
			top.primitiveCountPerActor = aggregate.primitiveCountPerActor;
			top.totalDuplicatedPrimitives = aggregate.totalDuplicatedPrimitives;
			top.duplicatedBytes = aggregate.duplicatedBytes;
		}
	}

	void PruneVoxelActorCache(SceneDebugStats& stats)
	{
		std::unordered_set<uint64_t> liveActorKeys;
		BuildLiveActorIdentityKeys(liveActorKeys);

		for (auto it = gVoxelActorCache.begin(); it != gVoxelActorCache.end(); )
		{
			if (liveActorKeys.find(it->first) == liveActorKeys.end())
			{
				EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "remove", VoxelActorPendingReason::ActorNotLive);
				it = gVoxelActorCache.erase(it);
				stats.voxelCacheSurfaceRemoves++;
				++gVoxelActorCacheSerial;
				continue;
			}
			if (it->second.hasSurface && it->second.lastSeenFrame != gVoxelActorCacheFrame)
			{
				stats.voxelCacheNotCaptured++;
				EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "retained-not-captured", VoxelActorPendingReason::None);
			}
			++it;
		}

		stats.voxelCacheEntries = (unsigned int)gVoxelActorCache.size();
		CollectVoxelActorCacheDuplicationStats(stats);
	}

	void EndVoxelActorCacheFrame(SceneDebugStats& stats, bool rootCapture)
	{
		if (gVoxelActorCacheCaptureDepth > 0)
		{
			--gVoxelActorCacheCaptureDepth;
		}
		if (rootCapture)
		{
			PruneVoxelActorCache(stats);
		}
	}

	bool BeginVoxelMeshCacheFrame()
	{
		const bool rootCapture = gVoxelMeshCacheCaptureDepth++ == 0;
		if (rootCapture)
		{
			++gVoxelMeshCacheFrame;
			if (gVoxelMeshCacheFrame == 0)
			{
				gVoxelMeshCacheFrame = 1;
			}
			gVoxelMeshBuildsThisFrame = 0;
		}
		return rootCapture;
	}

	void EndVoxelMeshCacheFrame(bool rootCapture)
	{
		if (gVoxelMeshCacheCaptureDepth > 0)
		{
			--gVoxelMeshCacheCaptureDepth;
		}
		if (rootCapture)
		{
			gVoxelMeshBuildsThisFrame = 0;
		}
	}

	bool CanBuildVoxelMeshThisFrame()
	{
		const int buildBudget = (int)nri_ptvoxelmeshbuilds;
		return buildBudget <= 0 || gVoxelMeshBuildsThisFrame < (uint32_t)buildBudget;
	}

	const FVoxelMeshData* GetCachedVoxelMesh(FVoxelModel* model, bool& outDeferred)
	{
		outDeferred = false;
		if (model == nullptr)
		{
			return nullptr;
		}

		auto found = gVoxelMeshCache.find(model);
		if (found == gVoxelMeshCache.end())
		{
			if (!CanBuildVoxelMeshThisFrame())
			{
				outDeferred = true;
				gDynamicCapturePerfStats.voxelMeshCacheDeferred++;
				return nullptr;
			}

			VoxelMeshCacheEntry entry = {};
			model->BuildCpuMesh(entry.mesh);
			entry.built = true;
			entry.valid = entry.mesh.vertices.Size() > 0 && entry.mesh.indices.Size() >= 3;
			gVoxelMeshBuildsThisFrame++;
			gDynamicCapturePerfStats.voxelMeshCacheBuilds++;
			if (!entry.valid)
			{
				gDynamicCapturePerfStats.voxelMeshCacheInvalid++;
			}
			found = gVoxelMeshCache.emplace(model, std::move(entry)).first;
		}

		const VoxelMeshCacheEntry& entry = found->second;
		if (!entry.built || !entry.valid)
		{
			return nullptr;
		}
		return &entry.mesh;
	}

	bool TrySpendVoxelTriangleBudget(uint32_t triangleCount, VoxelCaptureBudget& budget)
	{
		if ((int)nri_ptvoxelmaxtriangles > 0 && triangleCount > (uint32_t)(int)nri_ptvoxelmaxtriangles)
		{
			return false;
		}

		if (budget.unlimited)
		{
			return true;
		}

		if (!budget.spentTriangleBudget && triangleCount > budget.remainingTriangles)
		{
			budget.remainingTriangles = 0;
			budget.spentTriangleBudget = true;
			return true;
		}

		if (triangleCount > budget.remainingTriangles)
		{
			return false;
		}

		budget.remainingTriangles -= triangleCount;
		budget.spentTriangleBudget = true;
		return true;
	}

	bool TrySpendVoxelCacheUpdateBudget(VoxelCaptureBudget& budget)
	{
		if (budget.unlimitedCacheUpdates)
		{
			return true;
		}
		if (budget.remainingCacheUpdates == 0)
		{
			return false;
		}

		--budget.remainingCacheUpdates;
		return true;
	}

	bool BuildCanonicalVoxelMeshSurface(const FVoxelMeshData& mesh, SurfaceRef& outSurface)
	{
		const unsigned int indexCount = mesh.indices.Size();
		outSurface = {};
		outSurface.vertices.reserve(mesh.vertices.Size());
		for (unsigned int i = 0; i < mesh.vertices.Size(); ++i)
		{
			outSurface.vertices.push_back(MakeCapturedLocalModelVertex(mesh.vertices[i]));
		}

		const unsigned int vertexCount = mesh.vertices.Size();
		outSurface.indices.reserve(indexCount);
		for (unsigned int i = 0; i + 2u < indexCount; i += 3u)
		{
			const unsigned int i0 = mesh.indices[i + 0u];
			const unsigned int i1 = mesh.indices[i + 1u];
			const unsigned int i2 = mesh.indices[i + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
			{
				continue;
			}

			outSurface.indices.push_back(i0);
			outSurface.indices.push_back(i1);
			outSurface.indices.push_back(i2);
		}

		if (outSurface.indices.empty())
		{
			return false;
		}

		return true;
	}

	bool IsVoxelMeshVariantSurfaceReady(uint64_t meshVariantHash)
	{
		auto found = gVoxelMeshVariantSurfaceCache.find(meshVariantHash);
		return found != gVoxelMeshVariantSurfaceCache.end() &&
			found->second.built &&
			found->second.valid;
	}

	const SurfaceRef* GetCachedVoxelMeshVariantSurface(
		const VoxelActorCacheLookup& lookup,
		const FVoxelMeshData& mesh,
		bool recordPerf = true)
	{
		if (lookup.meshVariantHash == 0)
		{
			return nullptr;
		}

		auto found = gVoxelMeshVariantSurfaceCache.find(lookup.meshVariantHash);
		if (found == gVoxelMeshVariantSurfaceCache.end())
		{
			VoxelMeshVariantSurfaceCacheEntry entry = {};
			entry.meshVariantHash = lookup.meshVariantHash;
			entry.sourcePicnum = lookup.sourcePicnum;
			entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
			entry.built = true;
			entry.valid = BuildCanonicalVoxelMeshSurface(mesh, entry.canonicalSurface);
			if (recordPerf)
			{
				gDynamicCapturePerfStats.voxelCanonicalSurfaceBuilds++;
			}
			if (!entry.valid)
			{
				if (recordPerf)
				{
					gDynamicCapturePerfStats.voxelCanonicalSurfaceInvalid++;
				}
			}
			found = gVoxelMeshVariantSurfaceCache.emplace(lookup.meshVariantHash, std::move(entry)).first;
		}
		else
		{
			if (recordPerf)
			{
				gDynamicCapturePerfStats.voxelCanonicalSurfaceHits++;
			}
		}

		const VoxelMeshVariantSurfaceCacheEntry& entry = found->second;
		if (!entry.built || !entry.valid)
		{
			return nullptr;
		}
		return &entry.canonicalSurface;
	}

	bool BuildVoxelMeshSurfaceFromCanonical(
		const HWSprite& sprite,
		uint32_t drawListType,
		const SurfaceRef& canonicalSurface,
		const MaterialRef& voxelMaterial,
		SurfaceRef& outSurface)
	{
		outSurface = {};
		outSurface.material = voxelMaterial;
		outSurface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, outSurface.material.flags);
		outSurface.vertices.reserve(canonicalSurface.vertices.size());
		for (const CapturedVertex& source : canonicalSurface.vertices)
		{
			CapturedVertex vertex = {};
			TransformModelPoint(sprite.rotmat, source.position[0], source.position[1], source.position[2], vertex, source.uv[0], source.uv[1]);
			outSurface.vertices.push_back(vertex);
		}
		outSurface.indices = canonicalSurface.indices;
		if (outSurface.indices.empty())
		{
			return false;
		}

		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
		{
			ApplyActorPreviousTransform(outSurface, sprite.Sprite->ownerActor);
		}
		return true;
	}

	bool BuildVoxelMeshSurface(
		const HWSprite& sprite,
		uint32_t drawListType,
		const VoxelActorCacheLookup& lookup,
		const FVoxelMeshData& mesh,
		const MaterialRef& voxelMaterial,
		SurfaceRef& outSurface)
	{
		VoxelActorCacheLookup effectiveLookup = lookup;
		if (effectiveLookup.meshVariantHash == 0)
		{
			const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
			effectiveLookup.meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
			effectiveLookup.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->spritetexture().GetIndex() : -1;
			effectiveLookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		}

		if (const SurfaceRef* canonicalSurface = GetCachedVoxelMeshVariantSurface(effectiveLookup, mesh))
		{
			return BuildVoxelMeshSurfaceFromCanonical(sprite, drawListType, *canonicalSurface, voxelMaterial, outSurface);
		}

		SurfaceRef canonicalSurface = {};
		if (!BuildCanonicalVoxelMeshSurface(mesh, canonicalSurface))
		{
			return false;
		}
		return BuildVoxelMeshSurfaceFromCanonical(sprite, drawListType, canonicalSurface, voxelMaterial, outSurface);
	}

	bool ShouldUseTransientVoxelActorCapture(const HWSprite& sprite)
	{
		if (sprite.Sprite == nullptr)
		{
			return false;
		}

		// Duke security cameras (CAMERA1..CAMERA5, tiles 621..625) are actor-driven
		// camera props. They currently exercise a driver-hung path when promoted
		// into per-actor persistent BLASes, so keep them in the transient dynamic
		// overlay until the durable actor/AS representation models this class.
		return sprite.Sprite->picnum >= 621 && sprite.Sprite->picnum <= 625;
	}

	bool CaptureVoxelMeshSprite(const HWSprite& sprite, uint32_t drawListType, VoxelCaptureBudget& budget, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats, DynamicVoxelCaptureMode captureMode)
	{
		if (sprite.modelframe >= 0 || sprite.voxel == nullptr || sprite.voxel->model == nullptr)
		{
			return false;
		}

		FGameTexture* voxelTexture = TexMan.GetGameTexture(sprite.voxel->model->GetPaletteTexture());
		if (voxelTexture == nullptr || !voxelTexture->isValid())
		{
			stats.voxelStableUncacheable++;
			return false;
		}

		FGameTexture* emissiveSourceTexture = GetVoxelReplacementEmissiveSourceTexture(sprite);
		const MaterialRef voxelMaterial = MakeVoxelPaletteMaterialRef(voxelTexture, emissiveSourceTexture, sprite.palette, sprite.shade, sprite.alpha, MaterialFlag_Sprite);
		const bool forceTransientVoxel = ShouldUseTransientVoxelActorCapture(sprite);
		if (forceTransientVoxel)
		{
			captureMode = DynamicVoxelCaptureMode::Transient;
		}
		if (captureMode == DynamicVoxelCaptureMode::ReadOnlyCache)
		{
			if (!TryConsumeReadOnlyVoxelActorCacheSurface(sprite, voxelTexture, voxelMaterial, stats))
			{
				stats.voxelCacheNotCaptured++;
			}
			return true;
		}

		VoxelActorCacheLookup cacheLookup = {};
		if (captureMode == DynamicVoxelCaptureMode::Authoritative)
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelClassifyMs);
			cacheLookup = TrackVoxelActorSignature(sprite, voxelTexture, voxelMaterial, stats);
		}
		if (cacheLookup.stability == VoxelActorStability::Stable && cacheLookup.entry != nullptr && cacheLookup.entry->hasSurface)
		{
			return true;
		}

		const bool cacheSurfaceUpdate = captureMode == DynamicVoxelCaptureMode::Authoritative && cacheLookup.stability != VoxelActorStability::Stable;
		const bool transformRebakeAlreadyResident =
			cacheLookup.stability == VoxelActorStability::TransformRebake &&
			cacheLookup.entry != nullptr &&
			cacheLookup.entry->persistentReady;
		const bool cacheUpdateConsumesActorBudget = cacheSurfaceUpdate && !transformRebakeAlreadyResident;
		auto deferDesiredVariant = [&](VoxelActorPendingReason reason) -> bool
		{
			if (cacheSurfaceUpdate)
			{
				MarkVoxelActorVariantPending(cacheLookup, reason);
				stats.voxelCacheDeferred++;
				gDynamicCapturePerfStats.voxelCacheDeferred++;
				if (HasLastValidResidentVoxelSurface(cacheLookup))
				{
					TraceVoxelActorFallbackLastValid(sprite, cacheLookup, reason);
					return true;
				}
			}

			TraceVoxelActorFirstUseFallback(sprite, cacheLookup, reason);
			return true;
		};
		if (cacheUpdateConsumesActorBudget && !TrySpendVoxelCacheUpdateBudget(budget))
		{
			return deferDesiredVariant(VoxelActorPendingReason::ActorBudget);
		}

		const FVoxelMeshData* mesh = nullptr;
		bool meshDeferred = false;
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelMeshMs);
			mesh = GetCachedVoxelMesh(sprite.voxel->model, meshDeferred);
		}
		if (meshDeferred)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
			}

			if (!forceTransientVoxel)
			{
				CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			}
			return true;
		}
		if (mesh == nullptr)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
			}
			stats.voxelStableUncacheable++;
			return false;
		}

		const unsigned int indexCount = mesh->indices.Size();
		const uint32_t triangleCount = indexCount / 3u;
		if (!TrySpendVoxelTriangleBudget(triangleCount, budget))
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::TriangleBudget);
			}

			if (!forceTransientVoxel)
			{
				CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			}
			return true;
		}

		SurfaceRef exactSurface = {};
		bool hasExactSurface = false;
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSurfaceMs);
			hasExactSurface = BuildVoxelMeshSurface(sprite, drawListType, cacheLookup, *mesh, voxelMaterial, exactSurface);
		}
		if (!hasExactSurface)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::SurfaceBuildFailed);
			}
			return false;
		}

		if (cacheSurfaceUpdate)
		{
			const unsigned int previousStores = stats.voxelCacheSurfaceStores;
			const unsigned int previousRebuilds = stats.voxelCacheSurfaceRebuilds;
			const unsigned int previousTransformRebakes = stats.voxelCacheTransformRebakes;
			const bool wasPersistentReady = cacheLookup.entry != nullptr && cacheLookup.entry->persistentReady;
			const bool hadSurface = cacheLookup.entry != nullptr && cacheLookup.entry->hasSurface;
			const uint32_t exactPrimitiveCount = CountSurfacePrimitives(exactSurface);
			const SurfaceRef* canonicalSurface = GetCachedVoxelMeshVariantSurface(cacheLookup, *mesh, false);
			const SurfaceRef& storedMeshSurface = canonicalSurface != nullptr ? *canonicalSurface : exactSurface;
			const VoxelMeshBakeSpace storedBakeSpace =
				canonicalSurface != nullptr ? VoxelMeshBakeSpace::LocalSpace : VoxelMeshBakeSpace::BakedTransform;
			const bool sharedVariantReady =
				storedBakeSpace == VoxelMeshBakeSpace::LocalSpace &&
				IsVoxelMeshVariantSurfaceReady(cacheLookup.meshVariantHash);
			{
				ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelStoreMs);
				StoreVoxelActorCacheSurface(cacheLookup, storedMeshSurface, exactSurface, storedBakeSpace, sharedVariantReady, stats);
			}
			gDynamicCapturePerfStats.voxelCacheStores += stats.voxelCacheSurfaceStores - previousStores;
			gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheSurfaceRebuilds - previousRebuilds;
			gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheTransformRebakes - previousTransformRebakes;
			const auto storedEntry = gVoxelActorCache.find(cacheLookup.identityKey);
			const bool nowPersistentReady =
				cacheLookup.identityKey != 0 &&
				storedEntry != gVoxelActorCache.end() &&
				storedEntry->second.persistentReady;
			if (!wasPersistentReady &&
				!nowPersistentReady &&
				(hadSurface || exactPrimitiveCount <= kTransientVoxelLiveSurfacePrimitiveLimit))
			{
				const VoxelActorPendingReason pendingReason =
					cacheLookup.entry != nullptr ? (VoxelActorPendingReason)cacheLookup.entry->pendingReason : VoxelActorPendingReason::None;
				const DynamicVoxelEscapeReason escapeReason =
					cacheLookup.identityKey == 0 ? DynamicVoxelEscapeReason::NotCacheable :
					GetDynamicVoxelEscapeReasonForPending(pendingReason);
				RecordDynamicVoxelEscape(stats, sprite, cacheLookup, exactSurface, escapeReason);
				outSprites.push_back(std::move(exactSurface));
			}
			return true;
		}

		const DynamicVoxelEscapeReason escapeReason =
			forceTransientVoxel ? DynamicVoxelEscapeReason::CameraOrWeaponSpecial :
			captureMode == DynamicVoxelCaptureMode::Transient ? DynamicVoxelEscapeReason::LifecycleTransient :
			cacheLookup.identityKey == 0 ? DynamicVoxelEscapeReason::NotCacheable :
			DynamicVoxelEscapeReason::Unknown;
		RecordDynamicVoxelEscape(stats, sprite, cacheLookup, exactSurface, escapeReason);
		outSprites.push_back(std::move(exactSurface));
		return true;
	}

	VoxelCaptureBudget MakeVoxelCaptureBudget()
	{
		VoxelCaptureBudget budget = {};
		budget.unlimited = (int)nri_ptvoxeltrianglebudget <= 0;
		budget.remainingTriangles = budget.unlimited ? 0u : (uint32_t)(int)nri_ptvoxeltrianglebudget;
		budget.unlimitedCacheUpdates = (int)nri_ptvoxelcaptureactors <= 0;
		budget.remainingCacheUpdates = budget.unlimitedCacheUpdates ? 0u : (uint32_t)(int)nri_ptvoxelcaptureactors;
		return budget;
	}

	void CaptureModelSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats, DynamicVoxelCaptureMode captureMode)
	{
		const bool rootMeshCapture = BeginVoxelMeshCacheFrame();
		std::vector<HWSprite*> sprites;
		sprites.reserve(list.sprites.Size());
		auto finish = [&]()
		{
			EndVoxelMeshCacheFrame(rootMeshCapture);
		};

		for (auto* sprite : list.sprites)
		{
			if (sprite != nullptr)
			{
				sprites.push_back(sprite);
			}
		}

		const DVector3& viewPos = di.Viewpoint.Pos;
		std::stable_sort(sprites.begin(), sprites.end(), [&viewPos](const HWSprite* a, const HWSprite* b)
		{
			const double adx = (double)a->x - viewPos.X;
			const double ady = (double)a->y + viewPos.Y;
			const double adz = (double)a->z + viewPos.Z;
			const double bdx = (double)b->x - viewPos.X;
			const double bdy = (double)b->y + viewPos.Y;
			const double bdz = (double)b->z + viewPos.Z;
			return adx * adx + ady * ady + adz * adz < bdx * bdx + bdy * bdy + bdz * bdz;
		});

		VoxelCaptureBudget budget = MakeVoxelCaptureBudget();
		for (auto* sprite : sprites)
		{
			if (sprite == nullptr)
			{
				continue;
			}

			stats.modelDrawItems++;

			if (sprite->modelframe > 0)
			{
				stats.unsupportedModelDrawItems++;
				continue;
			}

			if (sprite->modelframe >= 0 || sprite->voxel == nullptr || sprite->voxel->model == nullptr)
			{
				continue;
			}

			if (CaptureVoxelMeshSprite(*sprite, drawListType, budget, outSprites, stats, captureMode))
			{
				stats.voxelProxyDrawItems++;
			}
		}
		finish();
	}

	void CaptureActorModelSprites(HWDrawList& list, uint32_t drawListType, int32_t actorIndex, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats)
	{
		const bool rootMeshCapture = BeginVoxelMeshCacheFrame();
		VoxelCaptureBudget budget = MakeVoxelCaptureBudget();
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOwnedByActor(*sprite, actorIndex))
			{
				continue;
			}

			stats.modelDrawItems++;

			if (sprite->modelframe > 0)
			{
				stats.unsupportedModelDrawItems++;
				continue;
			}

			if (sprite->modelframe >= 0 || sprite->voxel == nullptr || sprite->voxel->model == nullptr)
			{
				continue;
			}

			if (CaptureVoxelMeshSprite(*sprite, drawListType, budget, outSprites, stats, DynamicVoxelCaptureMode::Transient))
			{
				stats.voxelProxyDrawItems++;
			}
		}
		EndVoxelMeshCacheFrame(rootMeshCapture);
	}
}

namespace nri_scene
{
const char* GetDynamicVoxelEscapeReasonName(DynamicVoxelEscapeReason reason)
{
	switch (reason)
	{
	case DynamicVoxelEscapeReason::VariantPending: return "variant-pending";
	case DynamicVoxelEscapeReason::MaterialPending: return "material-pending";
	case DynamicVoxelEscapeReason::ActorBudget: return "actor-budget";
	case DynamicVoxelEscapeReason::BuildBudget: return "build-budget";
	case DynamicVoxelEscapeReason::UnsupportedTransform: return "unsupported-transform";
	case DynamicVoxelEscapeReason::NonLocalSpace: return "non-local-space";
	case DynamicVoxelEscapeReason::CameraOrWeaponSpecial: return "camera-or-weapon-special";
	case DynamicVoxelEscapeReason::LifecycleTransient: return "lifecycle-transient";
	case DynamicVoxelEscapeReason::NotCacheable: return "not-cacheable";
	case DynamicVoxelEscapeReason::ValidationQuarantine: return "validation-quarantine";
	case DynamicVoxelEscapeReason::FallbackDisabled: return "fallback-disabled";
	case DynamicVoxelEscapeReason::MissingSurface: return "missing-surface";
	default: return "unknown";
	}
}

void ResetAverageTextureColorCache()
{
	gFrameLocalAverageTextureColorCache.clear();
	gSkyInspectionCache.clear();
}

void ResetSkyPerfStats()
{
	gSkyPerfStats = {};
}

SkyPerfStats ConsumeSkyPerfStats()
{
	SkyPerfStats stats = gSkyPerfStats;
	gSkyPerfStats = {};
	return stats;
}

DynamicCapturePerfStats ConsumeDynamicCapturePerfStats()
{
	DynamicCapturePerfStats stats = gDynamicCapturePerfStats;
	gDynamicCapturePerfStats = {};
	return stats;
}

bool PrecacheVoxelModelCpuMesh(FVoxelModel* model, VoxelMeshPrecacheStats* stats)
{
	VoxelMeshPrecacheStats delta = {};
	delta.modelCandidates = 1;

	if (!r_voxels || model == nullptr)
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=model model=%p reason=%s\n",
				model,
				r_voxels ? "null-model" : "voxels-disabled");
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	auto found = gVoxelMeshCache.find(model);
	if (found != gVoxelMeshCache.end())
	{
		if (found->second.built && found->second.valid)
		{
			delta.meshHits = 1;
			delta.vertices = found->second.mesh.vertices.Size();
			delta.indices = found->second.mesh.indices.Size();
			delta.primitives = delta.indices / 3;
			if ((int)nri_ptloadingtrace >= 2)
			{
				Printf("NRI PT loading voxel mesh: event=hit model=%p vertices=%u indices=%u tris=%u\n",
					model,
					delta.vertices,
					delta.indices,
					delta.primitives);
			}
			RecordVoxelMeshPrecacheStats(delta, stats);
			return true;
		}

		delta.meshInvalid = 1;
		RecordVoxelMeshPrecacheStats(delta, stats);
		return false;
	}

	VoxelMeshCacheEntry entry = {};
	const auto start = std::chrono::steady_clock::now();
	model->BuildCpuMesh(entry.mesh);
	const auto end = std::chrono::steady_clock::now();
	entry.built = true;
	entry.valid = entry.mesh.vertices.Size() > 0 && entry.mesh.indices.Size() >= 3;

	delta.meshBuilds = 1;
	delta.buildMs = DurationMs(start, end);
	delta.vertices = entry.mesh.vertices.Size();
	delta.indices = entry.mesh.indices.Size();
	delta.primitives = delta.indices / 3;
	if (!entry.valid)
	{
		delta.meshInvalid = 1;
	}

	if ((int)nri_ptloadingtrace >= 2)
	{
		Printf("NRI PT loading voxel mesh: event=build model=%p valid=%s vertices=%u indices=%u tris=%u ms=%.3f\n",
			model,
			entry.valid ? "true" : "false",
			delta.vertices,
			delta.indices,
			delta.primitives,
			delta.buildMs);
	}

	gVoxelMeshCache.emplace(model, std::move(entry));
	RecordVoxelMeshPrecacheStats(delta, stats);
	return delta.meshInvalid == 0;
}

bool PrecacheVoxelTextureCpuMesh(FTextureID texid, VoxelMeshPrecacheStats* stats)
{
	VoxelMeshPrecacheStats delta = {};
	delta.textureCandidates = 1;

	if (!r_voxels || !texid.isValid())
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=texture tex=%d voxel=-1 reason=%s\n",
				texid.isValid() ? texid.GetIndex() : -1,
				r_voxels ? "invalid-texture" : "voxels-disabled");
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	int voxelIndex = -1;
	FVoxelModel* model = ResolveVoxelTextureModel(texid, &voxelIndex);
	if (model == nullptr)
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=texture tex=%d voxel=%d reason=no-voxel-model\n",
				texid.isValid() ? texid.GetIndex() : -1,
				voxelIndex);
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	RecordVoxelMeshPrecacheStats(delta, stats);
	const bool meshReady = PrecacheVoxelModelCpuMesh(model, stats);
	if (!meshReady)
	{
		return false;
	}

	const VoxelMeshVariantKey meshVariantKey = BuildLoadingVoxelMeshVariantKey(texid, model, voxelIndex);
	const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
	auto foundMesh = gVoxelMeshCache.find(model);
	if (meshVariantHash == 0 || foundMesh == gVoxelMeshCache.end() || !foundMesh->second.built || !foundMesh->second.valid)
	{
		return meshReady;
	}

	VoxelMeshPrecacheStats variantDelta = {};
	variantDelta.meshVariantCandidates = 1;
	VoxelActorCacheLookup lookup = {};
	lookup.meshVariantHash = meshVariantHash;
	lookup.sourcePicnum = texid.isValid() ? texid.GetIndex() : -1;
	lookup.resolvedVoxelIndex = voxelIndex;
	const bool hadVariantSurface = IsVoxelMeshVariantSurfaceReady(meshVariantHash);
	const SurfaceRef* surface = GetCachedVoxelMeshVariantSurface(lookup, foundMesh->second.mesh, false);
	if (hadVariantSurface)
	{
		variantDelta.meshVariantHits = 1;
	}
	else if (surface != nullptr)
	{
		variantDelta.meshVariantBuilds = 1;
	}
	else
	{
		variantDelta.meshVariantInvalid = 1;
	}
	variantDelta.variantPrimitives = surface != nullptr ? CountSurfacePrimitives(*surface) : 0u;
	if ((int)nri_ptloadingtrace >= 2)
	{
		Printf("NRI PT loading voxel variant: event=%s source=texture tex=%d voxel=%d mesh_variant=0x%llx transform_keyed=0 tris=%u\n",
			hadVariantSurface ? "hit" : (surface != nullptr ? "build" : "invalid"),
			texid.isValid() ? texid.GetIndex() : -1,
			voxelIndex,
			(unsigned long long)meshVariantHash,
			variantDelta.variantPrimitives);
	}
	RecordVoxelMeshPrecacheStats(variantDelta, stats);
	return meshReady;
}

void PrecacheLiveActorVoxelMeshes(VoxelMeshPrecacheStats* stats)
{
	if (!r_voxels || (int)nri_ptloadingvoxelactors <= 0)
	{
		return;
	}

	std::unordered_set<uint64_t> seenMeshVariants;
	std::vector<FTextureID> candidateTexids;
	TSpriteIterator<DCoreActor> it;
	while (DCoreActor* actor = it.Next())
	{
		if (!IsLiveActorVoxelWarmupCandidate(actor))
		{
			continue;
		}

		BuildLoadingActorTextureCandidates(actor, candidateTexids);
		bool countedActor = false;
		for (const FTextureID texid : candidateTexids)
		{
			int voxelIndex = -1;
			FVoxelModel* model = ResolveVoxelTextureModel(texid, &voxelIndex);
			if (model == nullptr)
			{
				continue;
			}

			if (!countedActor)
			{
				VoxelMeshPrecacheStats actorDelta = {};
				actorDelta.actorCandidates = 1;
				RecordVoxelMeshPrecacheStats(actorDelta, stats);
				countedActor = true;
			}

			const VoxelMeshVariantKey meshVariantKey = BuildLoadingVoxelMeshVariantKey(texid, model, voxelIndex);
			const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
			if (meshVariantHash != 0 && !seenMeshVariants.insert(meshVariantHash).second)
			{
				if ((int)nri_ptloadingtrace >= 2)
				{
					Printf("NRI PT loading voxel actor: event=variant-hit actor=%d tex=%d voxel=%d mesh_variant=0x%llx transform_keyed=0\n",
						(int)actor->GetIndex(),
						texid.GetIndex(),
						voxelIndex,
						(unsigned long long)meshVariantHash);
				}
				continue;
			}

			if ((int)nri_ptloadingtrace >= 2)
			{
				Printf("NRI PT loading voxel actor: event=variant-request actor=%d tex=%d voxel=%d mesh_variant=0x%llx transform_keyed=0\n",
					(int)actor->GetIndex(),
					texid.GetIndex(),
					voxelIndex,
					(unsigned long long)meshVariantHash);
			}
			PrecacheVoxelTextureCpuMesh(texid, stats);
		}
	}
}

void PrintAndResetLoadingWarmupStats(const char* phase)
{
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading warmup: phase=%s r_voxels=%u textures=%u actors=%u models=%u mesh_variants=%u mesh_hits=%u mesh_builds=%u mesh_invalid=%u mesh_skipped=%u variant_hits=%u variant_builds=%u variant_invalid=%u vertices=%u indices=%u tris=%u variant_tris=%u build_ms=%.3f\n",
			phase != nullptr ? phase : "unknown",
			r_voxels ? 1u : 0u,
			gVoxelLoadingWarmupStats.textureCandidates,
			gVoxelLoadingWarmupStats.actorCandidates,
			gVoxelLoadingWarmupStats.modelCandidates,
			gVoxelLoadingWarmupStats.meshVariantCandidates,
			gVoxelLoadingWarmupStats.meshHits,
			gVoxelLoadingWarmupStats.meshBuilds,
			gVoxelLoadingWarmupStats.meshInvalid,
			gVoxelLoadingWarmupStats.meshSkipped,
			gVoxelLoadingWarmupStats.meshVariantHits,
			gVoxelLoadingWarmupStats.meshVariantBuilds,
			gVoxelLoadingWarmupStats.meshVariantInvalid,
			gVoxelLoadingWarmupStats.vertices,
			gVoxelLoadingWarmupStats.indices,
			gVoxelLoadingWarmupStats.primitives,
			gVoxelLoadingWarmupStats.variantPrimitives,
			gVoxelLoadingWarmupStats.buildMs);
	}
	gVoxelLoadingWarmupStats = {};
}

void Copy3(const float* source, float* destination)
{
	destination[0] = source[0];
	destination[1] = source[1];
	destination[2] = source[2];
}

bool TryGetAverageTextureColor(FGameTexture* texture, float* outColor)
{
	__try
	{
		return TryGetAverageTextureColorRecursive(texture, outColor, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	return false;
	}
}

MaterialRef MakeMaterialRef(FGameTexture* texture, int palette, int shade, float alpha, uint32_t extraFlags)
{
	MaterialRef material = {};
	material.texture = texture;
	material.palette = palette;
	material.shade = shade;
	material.alpha = alpha;
	material.flags = extraFlags;

	if (texture != nullptr)
	{
		auto* baseTexture = texture->GetTexture();
		if (baseTexture != nullptr && baseTexture->GetImage() != nullptr && baseTexture->GetImage()->UseGamePalette())
		{
			material.flags |= MaterialFlag_Indexed;
		}

		if (texture->isFullbright())
		{
			material.flags |= MaterialFlag_Fullbright;
		}
	}

	return material;
}

void UpdateSceneSky(SceneView& outView, FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType)
{
	if (ShouldTraceSkyPerf())
	{
		gSkyPerfStats.updateCalls++;
		switch (sourceType)
		{
		case PTSkySourceType::Wall:
			gSkyPerfStats.wallUpdateCalls++;
			break;
		case PTSkySourceType::Flat:
			gSkyPerfStats.flatUpdateCalls++;
			break;
		case PTSkySourceType::Portal:
			gSkyPerfStats.portalUpdateCalls++;
			break;
		default:
			break;
		}
	}
	ScopedSkyPerfTimer timer(gSkyPerfStats.updateTimeUs);
	SkyCandidate candidate = {};
	if (TryInspectSkyTexture(texture, fallbackColor, sourceType, candidate))
	{
		ApplySkyCandidate(outView, texture, candidate, sourceType);
	}
}

SceneDebugStats CollectDebugStats(HWDrawInfo& di)
{
	SceneDebugStats stats = {};

	stats.wallDrawItems =
		CountDrawListItems(di, GLDL_PLAINWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSD) +
		CountDrawListItems(di, GLDL_MASKEDWALLSV) +
		CountDrawListItems(di, GLDL_MASKEDWALLSH) +
		CountDrawListItems(di, GLDL_TRANSLUCENTBORDER);

	stats.flatDrawItems =
		CountDrawListItems(di, GLDL_PLAINFLATS) +
		CountDrawListItems(di, GLDL_MASKEDFLATS) +
		CountDrawListItems(di, GLDL_MASKEDSLOPEFLATS);

	stats.spriteDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT) + CountDrawListItems(di, GLDL_MODELS);
	stats.modelDrawItems = CountDrawListItems(di, GLDL_MODELS);
	stats.translucentDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT);
	stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems + stats.translucentDrawItems;
	stats.triangleEstimate = 0;
	stats.materialRefs = 0;
	return stats;
}

bool CaptureDynamicScene(HWDrawInfo& di, SceneView& outView, DynamicVoxelCaptureMode voxelCaptureMode)
{
	outView = {};
	outView.drawInfo = &di;
	gDynamicCapturePerfStats.calls++;
	const bool rootVoxelCacheFrame = [&]()
	{
		if (voxelCaptureMode != DynamicVoxelCaptureMode::Authoritative)
		{
			return false;
		}
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.voxelFrameMs);
		return BeginVoxelActorCacheFrame();
	}();
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.countMs);
		outView.stats.wallDrawItems =
			CountDrawListItems(di, GLDL_MASKEDWALLSS) +
			CountDrawListItems(di, GLDL_MASKEDWALLSD) +
			CountDrawListItems(di, GLDL_MASKEDWALLSV) +
			CountDrawListItems(di, GLDL_MASKEDWALLSH);
		outView.stats.flatDrawItems =
			CountDrawListItems(di, GLDL_MASKEDFLATS) +
			CountDrawListItems(di, GLDL_MASKEDSLOPEFLATS);
		outView.stats.spriteDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT) + CountDrawListItems(di, GLDL_MODELS);
		outView.stats.translucentDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT);
		outView.stats.modelDrawItems = CountDrawListItems(di, GLDL_MODELS);
	}
	outView.stats.totalDrawItems = outView.stats.wallDrawItems + outView.stats.flatDrawItems + outView.stats.spriteDrawItems;

	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.wallsMs);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], GLDL_MASKEDWALLSS, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], GLDL_MASKEDWALLSD, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], GLDL_MASKEDWALLSV, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], GLDL_MASKEDWALLSH, outView.opaqueWalls, outView.stats, outView);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.flatsMs);
		CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats);
		CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.facingSpritesMs);
		CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, outView.opaqueSprites);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSpritesMs);
		CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats, voxelCaptureMode);
	}
	if (voxelCaptureMode == DynamicVoxelCaptureMode::Authoritative)
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.voxelFrameMs);
		EndVoxelActorCacheFrame(outView.stats, rootVoxelCacheFrame);
	}

	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.statsMs);
		for (const auto& wall : outView.opaqueWalls)
		{
			outView.stats.triangleEstimate += CountFanTriangles(wall);
			outView.stats.materialRefs++;
		}

		for (const auto& flat : outView.opaqueFlats)
		{
			outView.stats.triangleEstimate += CountTriangleListTriangles(flat);
			outView.stats.materialRefs++;
		}

		for (const auto& sprite : outView.opaqueSprites)
		{
			outView.stats.triangleEstimate += CountFanTriangles(sprite);
			outView.stats.materialRefs++;
		}
	}

	gDynamicCapturePerfStats.wallSurfaces += (uint32_t)outView.opaqueWalls.size();
	gDynamicCapturePerfStats.flatSurfaces += (uint32_t)outView.opaqueFlats.size();
	gDynamicCapturePerfStats.spriteSurfaces += (uint32_t)outView.opaqueSprites.size();
	gDynamicCapturePerfStats.voxelProxySurfaces += outView.stats.voxelProxyDrawItems;
	gDynamicCapturePerfStats.unsupportedModelSurfaces += outView.stats.unsupportedModelDrawItems;

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}

bool CaptureActorSpriteScene(HWDrawInfo& di, int32_t actorIndex, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
	CaptureActorFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, actorIndex, outView.opaqueSprites);
	CaptureActorModelSprites(di.drawlists[GLDL_MODELS], GLDL_MODELS, actorIndex, outView.opaqueSprites, outView.stats);

	outView.stats.spriteDrawItems = (uint32_t)outView.opaqueSprites.size();
	outView.stats.totalDrawItems = outView.stats.spriteDrawItems;
	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += CountFanTriangles(sprite);
		outView.stats.materialRefs++;
		if (sprite.provenance.sourceType != SurfaceSourceType::VoxelProxySprite)
		{
			outView.stats.translucentDrawItems++;
		}
	}

	return !outView.opaqueSprites.empty();
}

uint64_t GetPersistentVoxelCacheSerial()
{
	return gVoxelActorCacheSerial;
}

bool BuildPersistentVoxelCacheSceneView(SceneView& outView)
{
	outView = {};
	std::vector<PersistentVoxelCacheEntryView> entries;
	if (!BuildPersistentVoxelCacheEntries(entries))
	{
		return false;
	}

	outView.opaqueSprites.reserve(entries.size());
	outView.stats.voxelCacheEntries = (unsigned int)entries.size();
	for (const auto& entry : entries)
	{
		if (entry.surface == nullptr)
		{
			continue;
		}
		outView.opaqueSprites.push_back(*entry.surface);
		outView.stats.triangleEstimate += entry.primitiveCount;
		outView.stats.voxelCachePrimitives += entry.primitiveCount;
		outView.stats.materialRefs++;
	}

	outView.stats.voxelCacheEntries = (unsigned int)outView.opaqueSprites.size();
	outView.stats.spriteDrawItems = (unsigned int)outView.opaqueSprites.size();
	outView.stats.modelDrawItems = outView.stats.spriteDrawItems;
	outView.stats.voxelProxyDrawItems = outView.stats.spriteDrawItems;
	outView.stats.totalDrawItems = outView.stats.spriteDrawItems;
	return true;
}

bool BuildPersistentVoxelCacheEntries(std::vector<PersistentVoxelCacheEntryView>& outEntries)
{
	outEntries.clear();
	if (gVoxelActorCache.empty())
	{
		return false;
	}

	std::vector<std::pair<uint64_t, const VoxelActorCacheEntry*>> sortedEntries;
	sortedEntries.reserve(gVoxelActorCache.size());
	for (const auto& pair : gVoxelActorCache)
	{
		if (pair.second.hasSurface && pair.second.persistentReady)
		{
			sortedEntries.emplace_back(pair.first, &pair.second);
		}
	}

	if (sortedEntries.empty())
	{
		return false;
	}

	std::sort(sortedEntries.begin(), sortedEntries.end(), [](const auto& a, const auto& b)
	{
		return a.first < b.first;
	});

	outEntries.reserve(sortedEntries.size());
	for (const auto& entry : sortedEntries)
	{
		PersistentVoxelCacheEntryView view = {};
		view.identityKey = entry.first;
		view.signature = entry.second->signature;
		view.geometrySignature = entry.second->geometrySignature;
		view.surfaceSignature = entry.second->surfaceSignature;
		view.bakedSurfaceSignature = entry.second->bakedSurfaceSignature != 0 ? entry.second->bakedSurfaceSignature : entry.second->surfaceSignature;
		view.materialSignature = entry.second->materialSignature;
		view.transformBasisSignature = entry.second->transformBasisSignature;
		view.meshKeyHash = entry.second->meshKeyHash;
		view.materialKeyHash = entry.second->materialKeyHash;
		view.meshVariantHash = entry.second->meshVariantHash;
		view.materialVariantHash = entry.second->materialVariantHash;
		view.meshBakeSpace = entry.second->meshBakeSpace;
		view.sourcePicnum = entry.second->sourcePicnum;
		view.resolvedVoxelIndex = entry.second->resolvedVoxelIndex;
		view.primitiveCount = entry.second->primitiveCount;
		if (entry.second->meshBakeSpace == VoxelMeshBakeSpace::LocalSpace)
		{
			std::copy(std::begin(entry.second->currentTransform), std::end(entry.second->currentTransform), std::begin(view.instanceTransform));
		}
		else
		{
			FillVoxelTranslationInstanceTransform(entry.second->currentTranslation, entry.second->bakedTranslation, view.instanceTransform);
		}
		view.currentTranslation[0] = entry.second->currentTranslation[0];
		view.currentTranslation[1] = entry.second->currentTranslation[1];
		view.currentTranslation[2] = entry.second->currentTranslation[2];
		view.bakedTranslation[0] = entry.second->bakedTranslation[0];
		view.bakedTranslation[1] = entry.second->bakedTranslation[1];
		view.bakedTranslation[2] = entry.second->bakedTranslation[2];
		view.surface = &entry.second->surface;
		view.lightSurface = &entry.second->lightSurface;
		outEntries.push_back(std::move(view));
	}

	return true;
}

bool BuildPrecachedVoxelVariantViews(std::vector<PrecachedVoxelVariantView>& outEntries)
{
	outEntries.clear();
	if (!r_voxels || (int)nri_ptloadingvoxelactors <= 0)
	{
		return false;
	}

	const uint32_t variantLimit = (int)nri_ptloadingvoxelvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptloadingvoxelvariants;
	const uint32_t primitiveLimit = (int)nri_ptloadingvoxelvariantprims <= 0 ? 0u : (uint32_t)(int)nri_ptloadingvoxelvariantprims;
	uint32_t primitiveTotal = 0;
	std::unordered_set<uint64_t> seenVariantPairs;
	std::vector<FTextureID> candidateTexids;

	TSpriteIterator<DCoreActor> it;
	while (DCoreActor* actor = it.Next())
	{
		if (outEntries.size() >= variantLimit)
		{
			break;
		}
		if (!IsLiveActorVoxelWarmupCandidate(actor))
		{
			continue;
		}

		BuildLoadingActorTextureCandidates(actor, candidateTexids);
		for (const FTextureID texid : candidateTexids)
		{
			if (outEntries.size() >= variantLimit)
			{
				break;
			}

			int voxelIndex = -1;
			FVoxelModel* model = ResolveVoxelTextureModel(texid, &voxelIndex);
			if (model == nullptr)
			{
				continue;
			}
			FGameTexture* voxelTexture = TexMan.GetGameTexture(model->GetPaletteTexture());
			if (voxelTexture == nullptr || !voxelTexture->isValid())
			{
				continue;
			}

			const VoxelMeshVariantKey meshVariantKey = BuildLoadingVoxelMeshVariantKey(texid, model, voxelIndex);
			const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
			if (meshVariantHash == 0)
			{
				continue;
			}
			auto foundMesh = gVoxelMeshCache.find(model);
			if (foundMesh == gVoxelMeshCache.end() || !foundMesh->second.built || !foundMesh->second.valid)
			{
				continue;
			}

			VoxelActorCacheLookup lookup = {};
			lookup.meshVariantHash = meshVariantHash;
			lookup.sourcePicnum = texid.GetIndex();
			lookup.resolvedVoxelIndex = voxelIndex;
			const SurfaceRef* surface = GetCachedVoxelMeshVariantSurface(lookup, foundMesh->second.mesh, false);
			if (surface == nullptr)
			{
				continue;
			}

			FGameTexture* emissiveSourceTexture = TexMan.GetGameTexture(texid);
			if (emissiveSourceTexture != nullptr && !emissiveSourceTexture->isValid())
			{
				emissiveSourceTexture = nullptr;
			}
			const MaterialRef material = MakeVoxelPaletteMaterialRef(
				voxelTexture,
				emissiveSourceTexture,
				actor->spr.pal,
				actor->spr.shade,
				GetLoadingActorAlpha(actor),
				MaterialFlag_Sprite);
			const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(BuildVoxelMaterialVariantKey(voxelTexture, material));
			const uint64_t pairHash = HashCombine64(meshVariantHash, materialVariantHash);
			if (materialVariantHash == 0 || !seenVariantPairs.insert(pairHash).second)
			{
				continue;
			}

			const uint32_t primitiveCount = CountSurfacePrimitives(*surface);
			if (primitiveLimit != 0 && primitiveTotal != 0 && primitiveTotal + primitiveCount > primitiveLimit)
			{
				if ((int)nri_ptloadingtrace >= 2)
				{
					Printf("NRI PT loading voxel variant: event=defer reason=preload-budget actor=%d tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx tris=%u prims_used=%u prims_limit=%u\n",
						(int)actor->GetIndex(),
						texid.GetIndex(),
						voxelIndex,
						(unsigned long long)meshVariantHash,
						(unsigned long long)materialVariantHash,
						primitiveCount,
						primitiveTotal,
						primitiveLimit);
				}
				continue;
			}

			PrecachedVoxelVariantView view = {};
			view.meshKeyHash = meshVariantHash;
			view.materialKeyHash = materialVariantHash;
			view.meshVariantHash = meshVariantHash;
			view.materialVariantHash = materialVariantHash;
			view.sourcePicnum = texid.GetIndex();
			view.resolvedVoxelIndex = voxelIndex;
			view.primitiveCount = primitiveCount;
			view.surface = surface;
			view.material = material;
			outEntries.push_back(std::move(view));
			primitiveTotal += primitiveCount;
		}
	}

	return !outEntries.empty();
}

bool CaptureScene(HWDrawInfo& di, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
	const bool rootVoxelCacheFrame = BeginVoxelActorCacheFrame();
	outView.stats = CollectDebugStats(di);

	CaptureWalls(di, di.drawlists[GLDL_PLAINWALLS], GLDL_PLAINWALLS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLS], GLDL_MASKEDWALLS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], GLDL_MASKEDWALLSS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], GLDL_MASKEDWALLSD, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], GLDL_MASKEDWALLSV, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], GLDL_MASKEDWALLSH, outView.opaqueWalls, outView.stats, outView);
	CaptureMirrorBorders(di, di.drawlists[GLDL_TRANSLUCENTBORDER], GLDL_TRANSLUCENTBORDER, outView.opaqueWalls, outView.stats);

	CaptureFlats(di, di.drawlists[GLDL_PLAINFLATS], GLDL_PLAINFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats);

	CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, outView.opaqueSprites);
	CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats, DynamicVoxelCaptureMode::Authoritative);
	CapturePortalViews(di, outView);
	EndVoxelActorCacheFrame(outView.stats, rootVoxelCacheFrame);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += CountFanTriangles(wall);
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += CountTriangleListTriangles(flat);
		outView.stats.materialRefs++;
	}

	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += CountFanTriangles(sprite);
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}
}
