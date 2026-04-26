#include "nri_scene_bridge.h"

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
CVAR(Int, nri_ptvoxeltrianglebudget, 250000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelmaxtriangles, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelcaptureactors, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptvoxelmeshbuilds, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	using namespace nri_scene;

	constexpr float kAttachedWallSpriteDepthNudge = 0.01f;

	SkyPerfStats gSkyPerfStats = {};
	DynamicCapturePerfStats gDynamicCapturePerfStats = {};
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
	std::unordered_map<const FVoxelModel*, VoxelMeshCacheEntry> gVoxelMeshCache;
	uint64_t gVoxelActorCacheFrame = 0;
	uint32_t gVoxelActorCacheCaptureDepth = 0;
	uint64_t gVoxelActorCacheSerial = 1;

	struct VoxelActorCacheEntry
	{
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t materialSignature = 0;
		uint64_t identityKey = 0;
		int32_t actorIndex = -1;
		uintptr_t actorPtr = 0;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
		SurfaceRef surface;
		uint64_t lastSeenFrame = 0;
		uint32_t primitiveCount = 0;
		bool persistentReady = false;
		bool hasSurface = false;
	};

	std::unordered_map<uint64_t, VoxelActorCacheEntry> gVoxelActorCache;

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
		Changed,
	};

	struct VoxelActorCacheLookup
	{
		VoxelActorStability stability = VoxelActorStability::Uncacheable;
		uint64_t identityKey = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t materialSignature = 0;
		int32_t actorIndex = -1;
		uintptr_t actorPtr = 0;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
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

	MaterialRef MakeVoxelPaletteMaterialRef(FGameTexture* voxelTexture, int palette, int shade, float alpha, uint32_t extraFlags)
	{
		MaterialRef material = MakeMaterialRef(voxelTexture, palette, shade, alpha, extraFlags | MaterialFlag_PointSampled);
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

	uint64_t BuildVoxelActorIdentityKey(int32_t actorIndex, const DCoreActor* actor, const voxmodel_t* voxel)
	{
		if (actorIndex < 0 || actor == nullptr || voxel == nullptr || voxel->model == nullptr)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)actorIndex);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)actor);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)voxel);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)voxel->model);
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

		lookup.identityKey = BuildVoxelActorIdentityKey(actorIndex, sprite.Sprite->ownerActor, sprite.voxel);
		if (lookup.identityKey == 0)
		{
			return false;
		}

		lookup.actorIndex = actorIndex;
		lookup.actorPtr = (uintptr_t)sprite.Sprite->ownerActor;
		lookup.voxelPtr = (uintptr_t)sprite.voxel;
		lookup.voxelModelPtr = (uintptr_t)sprite.voxel->model;
		return true;
	}

	uint64_t BuildVoxelActorGeometrySignature(const HWSprite& sprite)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel->model);

		if (sprite.Sprite != nullptr)
		{
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sprite.Sprite->picnum);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sprite.Sprite->statnum);
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat);
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat2);
			hash = HashCombine64(hash, (uint64_t)(uint32_t)sprite.Sprite->spritetexture().GetIndex());
		}

		const FLOATTYPE* matrix = sprite.rotmat.get();
		for (int i = 0; i < 16; ++i)
		{
			hash = HashCombine64(hash, QuantizeSignatureFloat((double)matrix[i], 4096.0));
		}
		return hash;
	}

	uint64_t BuildVoxelActorMaterialSignature(FGameTexture* voxelTexture, const MaterialRef& material)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, voxelTexture != nullptr ? (uint64_t)(uint32_t)voxelTexture->GetID().GetIndex() : 0ull);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)material.palette);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)material.shade);
		hash = HashCombine64(hash, QuantizeSignatureFloat(material.alpha, 65535.0));
		hash = HashCombine64(hash, (uint64_t)material.flags);
		return hash;
	}

	uint64_t BuildVoxelActorSignature(uint64_t geometrySignature, uint64_t materialSignature)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, geometrySignature);
		hash = HashCombine64(hash, materialSignature);
		return hash;
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
		const uint64_t geometrySignature = BuildVoxelActorGeometrySignature(sprite);
		const uint64_t materialSignature = BuildVoxelActorMaterialSignature(voxelTexture, material);
		const uint64_t signature = BuildVoxelActorSignature(geometrySignature, materialSignature);
		lookup.signature = signature;
		lookup.geometrySignature = geometrySignature;
		lookup.materialSignature = materialSignature;
		auto found = gVoxelActorCache.find(lookup.identityKey);
		if (found == gVoxelActorCache.end())
		{
			stats.voxelStableSignatureMisses++;
			stats.voxelStableSplitLive++;
			lookup.stability = VoxelActorStability::New;
			return lookup;
		}

		lookup.entry = &found->second;
		lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
		lookup.entry->identityKey = lookup.identityKey;
		lookup.entry->actorIndex = lookup.actorIndex;
		lookup.entry->actorPtr = lookup.actorPtr;
		lookup.entry->voxelPtr = lookup.voxelPtr;
		lookup.entry->voxelModelPtr = lookup.voxelModelPtr;
		if (lookup.entry->signature == signature && lookup.entry->hasSurface)
		{
			if (!lookup.entry->persistentReady)
			{
				lookup.entry->persistentReady = true;
				++gVoxelActorCacheSerial;
			}
			stats.voxelStableSignatureHits++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			return lookup;
		}

		if (lookup.entry->geometrySignature == geometrySignature && lookup.entry->hasSurface)
		{
			lookup.entry->signature = signature;
			lookup.entry->materialSignature = materialSignature;
			lookup.entry->surface.material = material;
			lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
			if (!lookup.entry->persistentReady)
			{
				lookup.entry->persistentReady = true;
			}
			++gVoxelActorCacheSerial;
			stats.voxelStableSignatureChanges++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			return lookup;
		}

		stats.voxelStableSignatureChanges++;
		stats.voxelStableSplitLive++;
		lookup.stability = VoxelActorStability::Changed;
		return lookup;
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

	void StoreVoxelActorCacheSurface(const VoxelActorCacheLookup& lookup, const SurfaceRef& liveSurface, SceneDebugStats& stats)
	{
		if (lookup.identityKey == 0)
		{
			return;
		}

		VoxelActorCacheEntry& entry = gVoxelActorCache[lookup.identityKey];
		const bool hadSurface = entry.hasSurface;
		entry.signature = lookup.signature;
		entry.geometrySignature = lookup.geometrySignature;
		entry.materialSignature = lookup.materialSignature;
		entry.identityKey = lookup.identityKey;
		entry.actorIndex = lookup.actorIndex;
		entry.actorPtr = lookup.actorPtr;
		entry.voxelPtr = lookup.voxelPtr;
		entry.voxelModelPtr = lookup.voxelModelPtr;
		entry.surface = liveSurface;
		NormalizeCachedSurfacePreviousPositions(entry.surface);
		entry.lastSeenFrame = gVoxelActorCacheFrame;
		entry.primitiveCount = CountSurfacePrimitives(entry.surface);
		entry.persistentReady = false;
		entry.hasSurface = true;
		++gVoxelActorCacheSerial;

		if (lookup.stability == VoxelActorStability::New || !hadSurface)
		{
			stats.voxelCacheSurfaceStores++;
		}
		else if (lookup.stability == VoxelActorStability::Changed)
		{
			stats.voxelCacheSurfaceRebuilds++;
		}
	}

	void BuildLiveVoxelActorIdentityKeys(std::unordered_set<uint64_t>& outKeys)
	{
		outKeys.clear();
		if (!r_voxels)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (DCoreActor* actor = it.Next())
		{
			if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			const int32_t actorIndex = (int32_t)actor->GetIndex();
			if (actorIndex < 0)
			{
				continue;
			}

			const int voxelIndex = GetExtInfo(actor->spr.spritetexture()).tiletovox;
			if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr || voxmodels[voxelIndex]->model == nullptr)
			{
				continue;
			}

			const uint64_t identityKey = BuildVoxelActorIdentityKey(actorIndex, actor, voxmodels[voxelIndex]);
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

	void PruneVoxelActorCache(SceneDebugStats& stats)
	{
		std::unordered_set<uint64_t> liveActorKeys;
		BuildLiveVoxelActorIdentityKeys(liveActorKeys);

		for (auto it = gVoxelActorCache.begin(); it != gVoxelActorCache.end(); )
		{
			if (liveActorKeys.find(it->first) == liveActorKeys.end())
			{
				it = gVoxelActorCache.erase(it);
				stats.voxelCacheSurfaceRemoves++;
				++gVoxelActorCacheSerial;
				continue;
			}
			if (it->second.hasSurface && it->second.lastSeenFrame != gVoxelActorCacheFrame)
			{
				stats.voxelCacheNotCaptured++;
			}
			++it;
		}

		stats.voxelCacheEntries = (unsigned int)gVoxelActorCache.size();
		stats.voxelCachePrimitives = 0;
		for (const auto& pair : gVoxelActorCache)
		{
			if (pair.second.hasSurface)
			{
				stats.voxelCachePrimitives += pair.second.primitiveCount;
			}
		}
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

	bool BuildVoxelMeshSurface(const HWSprite& sprite, uint32_t drawListType, const FVoxelMeshData& mesh, const MaterialRef& voxelMaterial, SurfaceRef& outSurface)
	{
		const unsigned int indexCount = mesh.indices.Size();
		outSurface = {};
		outSurface.material = voxelMaterial;
		outSurface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, outSurface.material.flags);
		outSurface.vertices.reserve(mesh.vertices.Size());
		for (unsigned int i = 0; i < mesh.vertices.Size(); ++i)
		{
			outSurface.vertices.push_back(MakeCapturedModelVertex(sprite.rotmat, mesh.vertices[i]));
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

		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
		{
			ApplyActorPreviousTransform(outSurface, sprite.Sprite->ownerActor);
		}
		return true;
	}

	bool CaptureVoxelMeshSprite(const HWSprite& sprite, uint32_t drawListType, VoxelCaptureBudget& budget, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats, bool updatePersistentCache)
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

		const MaterialRef voxelMaterial = MakeVoxelPaletteMaterialRef(voxelTexture, sprite.palette, sprite.shade, sprite.alpha, MaterialFlag_Sprite);
		VoxelActorCacheLookup cacheLookup = {};
		if (updatePersistentCache)
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelClassifyMs);
			cacheLookup = TrackVoxelActorSignature(sprite, voxelTexture, voxelMaterial, stats);
		}
		if (cacheLookup.stability == VoxelActorStability::Stable && cacheLookup.entry != nullptr && cacheLookup.entry->hasSurface)
		{
			return true;
		}

		const bool cacheSurfaceUpdate = updatePersistentCache && cacheLookup.stability != VoxelActorStability::Stable;
		if (cacheSurfaceUpdate && !TrySpendVoxelCacheUpdateBudget(budget))
		{
			stats.voxelCacheNotCaptured++;
			stats.voxelCacheDeferred++;
			gDynamicCapturePerfStats.voxelCacheDeferred++;
			return true;
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
				stats.voxelCacheNotCaptured++;
				stats.voxelCacheDeferred++;
				gDynamicCapturePerfStats.voxelCacheDeferred++;
				return true;
			}

			CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			return true;
		}
		if (mesh == nullptr)
		{
			stats.voxelStableUncacheable++;
			return false;
		}

		const unsigned int indexCount = mesh->indices.Size();
		const uint32_t triangleCount = indexCount / 3u;
		if (!TrySpendVoxelTriangleBudget(triangleCount, budget))
		{
			if (cacheSurfaceUpdate)
			{
				stats.voxelCacheNotCaptured++;
				stats.voxelCacheDeferred++;
				gDynamicCapturePerfStats.voxelCacheDeferred++;
				return true;
			}

			CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			return true;
		}

		SurfaceRef exactSurface = {};
		bool hasExactSurface = false;
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSurfaceMs);
			hasExactSurface = BuildVoxelMeshSurface(sprite, drawListType, *mesh, voxelMaterial, exactSurface);
		}
		if (!hasExactSurface)
		{
			return false;
		}

		if (cacheSurfaceUpdate)
		{
			const unsigned int previousStores = stats.voxelCacheSurfaceStores;
			const unsigned int previousRebuilds = stats.voxelCacheSurfaceRebuilds;
			{
				ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelStoreMs);
				StoreVoxelActorCacheSurface(cacheLookup, exactSurface, stats);
			}
			gDynamicCapturePerfStats.voxelCacheStores += stats.voxelCacheSurfaceStores - previousStores;
			gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheSurfaceRebuilds - previousRebuilds;
			return true;
		}

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

	void CaptureModelSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats)
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

			if (CaptureVoxelMeshSprite(*sprite, drawListType, budget, outSprites, stats, true))
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

			if (CaptureVoxelMeshSprite(*sprite, drawListType, budget, outSprites, stats, false))
			{
				stats.voxelProxyDrawItems++;
			}
		}
		EndVoxelMeshCacheFrame(rootMeshCapture);
	}
}

namespace nri_scene
{
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

bool CaptureDynamicScene(HWDrawInfo& di, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
	gDynamicCapturePerfStats.calls++;
	const bool rootVoxelCacheFrame = [&]()
	{
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
		CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats);
	}
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
		view.materialSignature = entry.second->materialSignature;
		view.primitiveCount = entry.second->primitiveCount;
		view.surface = &entry.second->surface;
		outEntries.push_back(std::move(view));
	}

	return true;
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
	CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats);
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
