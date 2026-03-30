#include "nri_scene_bridge.h"

#include "nri_portal_bridge.h"
#include "nri_texture_signature.h"

#include "c_cvars.h"
#include "hw_portal.h"
#include "hw_voxels.h"
#include "image.h"
#include "model_kvx.h"
#include "skyboxtexture.h"
#include "texturemanager.h"
#include "textures.h"
#include "v_video.h"
#include <chrono>
#include <unordered_map>
#include <windows.h>

EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	using namespace nri_scene;

	constexpr float kAttachedWallSpriteDepthNudge = 0.01f;

	SkyPerfStats gSkyPerfStats = {};
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

	void NudgeAttachedWallSpriteSurface(const HWWall& wall, SurfaceRef& surface)
	{
		if (wall.Sprite == nullptr || wall.walldist == nullptr || surface.vertices.empty())
		{
			return;
		}

		// The raster path uses depth bias for wall-attached sprites so coplanar walls do not overdraw them.
		// PT needs a small geometric equivalent or the structural wall can win the closest-hit test.
		const DVector2 normal = wall.Sprite->Angles.Yaw.ToVector();
		const float offsetX = (float)(normal.X * kAttachedWallSpriteDepthNudge);
		const float offsetZ = (float)(-normal.Y * kAttachedWallSpriteDepthNudge);

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += offsetX;
			vertex.position[2] += offsetZ;
			vertex.prevPosition[0] += offsetX;
			vertex.prevPosition[2] += offsetZ;
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
			const uint32_t extraFlags = wall->Sprite != nullptr ? MaterialFlag_Sprite : MaterialFlag_None;
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
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat);
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
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat | MaterialFlag_Sprite);
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
			surface.material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, MaterialFlag_Sprite);
			surface.provenance = MakeSpriteProvenance(*sprite, SurfaceSourceType::FacingSprite, drawListType, surface.material.flags);
			surface.vertices.reserve(4);
			for (uint32_t i = 0; i < 4; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
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

	void AddVoxelFace(const VSMatrix& matrix, const float* extents, const int* indices, SurfaceRef& outSurface)
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

	void CaptureModelSprites(HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats)
	{
		static const int faces[6][4] = {
			{ 0, 1, 2, 3 },
			{ 4, 5, 6, 7 },
			{ 0, 4, 7, 3 },
			{ 1, 5, 6, 2 },
			{ 3, 2, 6, 7 },
			{ 0, 1, 5, 4 },
		};

		for (auto* sprite : list.sprites)
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

			FGameTexture* voxelTexture = TexMan.GetGameTexture(sprite->voxel->model->GetPaletteTexture());
			if (voxelTexture == nullptr || !voxelTexture->isValid())
			{
				continue;
			}

			const float extents[3] = {
				(float)sprite->voxel->siz.X,
				(float)sprite->voxel->siz.Z,
				(float)sprite->voxel->siz.Y
			};

			for (const auto& face : faces)
			{
				SurfaceRef surface = {};
				surface.material = MakeMaterialRef(voxelTexture, sprite->palette, sprite->shade, sprite->alpha, MaterialFlag_Sprite);
				surface.provenance = MakeSpriteProvenance(*sprite, SurfaceSourceType::VoxelProxySprite, drawListType, surface.material.flags);
				surface.vertices.reserve(4);
				AddVoxelFace(sprite->rotmat, extents, face, surface);
				if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
				{
					ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				}
				outSprites.push_back(std::move(surface));
			}

			stats.voxelProxyDrawItems++;
		}
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
	outView.stats.totalDrawItems = outView.stats.wallDrawItems + outView.stats.flatDrawItems + outView.stats.spriteDrawItems;

	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], GLDL_MASKEDWALLSS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], GLDL_MASKEDWALLSD, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], GLDL_MASKEDWALLSV, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], GLDL_MASKEDWALLSH, outView.opaqueWalls, outView.stats, outView);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats);
	CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, outView.opaqueSprites);
	CaptureModelSprites(di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += wall.vertices.size() >= 3 ? (unsigned int)wall.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += (unsigned int)(flat.vertices.size() / 3);
		outView.stats.materialRefs++;
	}

	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += sprite.vertices.size() >= 3 ? (unsigned int)sprite.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}

bool CaptureScene(HWDrawInfo& di, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
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
	CaptureModelSprites(di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats);
	CapturePortalViews(di, outView);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += wall.vertices.size() >= 3 ? (unsigned int)wall.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += (unsigned int)(flat.vertices.size() / 3);
		outView.stats.materialRefs++;
	}

	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += sprite.vertices.size() >= 3 ? (unsigned int)sprite.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}
}
