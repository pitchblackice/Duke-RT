#include "nri_renderer.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_resources.h"
#include "nri_material_policy.h"
#include "nri_pass_dispatch.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_upload.h"
#include "nri_static_scene_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_sky_environment.h"
#include "nri_upload_hash.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Bool, nri_ptslowdowntrace)
EXTERN_CVAR(Bool, nri_voxelstats)
EXTERN_CVAR(Bool, nri_ptbootstrap)
EXTERN_CVAR(Bool, nri_ptdirectscene)
EXTERN_CVAR(Float, nri_ptmirrordynamicdistance)
EXTERN_CVAR(Int, nri_ptbootstrapmode)
EXTERN_CVAR(Int, nri_ptdebug)
EXTERN_CVAR(Int, nri_ptloadingtrace)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)

namespace
{
	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}

	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
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

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	static void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			float normal[3] = {};
			if (!TryComputeCapturedSurfaceNormal(surface, normal))
			{
				continue;
			}

			for (nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				vertex.position[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.position[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.position[2] += normal[2] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[2] += normal[2] * kBlindSpotFlatDepthNudge;
			}
		}
	}

	static void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
	{
		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			return;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
		}
	}

	static uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	struct MirrorPlayerCaptureStats
	{
		int32_t viewpointActorIndex = -1;
		int32_t localPlayerActorIndex = -1;
		int32_t selectedMirrorWallIndex = -1;
		bool viewpointMatchesLocalPlayer = false;
		bool capturedScene = false;
		uint32_t mirrorPortalCandidates = 0;
		uint32_t rawFacingSprites = 0;
		uint32_t rawVoxelSprites = 0;
		uint32_t capturedSurfaceCount = 0;
		uint32_t capturedMatchingActorSurfaces = 0;
		uint32_t capturedOtherActorSurfaces = 0;
		uint32_t capturedActorlessSurfaces = 0;
		uint32_t filteredSurfaceCount = 0;
	};

	static bool MirrorPlayerCaptureStatsDiffer(const MirrorPlayerCaptureStats& a, const MirrorPlayerCaptureStats& b)
	{
		return
			a.viewpointActorIndex != b.viewpointActorIndex ||
			a.localPlayerActorIndex != b.localPlayerActorIndex ||
			a.selectedMirrorWallIndex != b.selectedMirrorWallIndex ||
			a.viewpointMatchesLocalPlayer != b.viewpointMatchesLocalPlayer ||
			a.capturedScene != b.capturedScene ||
			a.mirrorPortalCandidates != b.mirrorPortalCandidates ||
			a.rawFacingSprites != b.rawFacingSprites ||
			a.rawVoxelSprites != b.rawVoxelSprites ||
			a.capturedSurfaceCount != b.capturedSurfaceCount ||
			a.capturedMatchingActorSurfaces != b.capturedMatchingActorSurfaces ||
			a.capturedOtherActorSurfaces != b.capturedOtherActorSurfaces ||
			a.capturedActorlessSurfaces != b.capturedActorlessSurfaces ||
			a.filteredSurfaceCount != b.filteredSurfaceCount;
	}

	static uint32_t CountDrawListActorSprites(const HWDrawList& drawList, int32_t actorIndex, bool requireVoxel)
	{
		uint32_t count = 0;
		for (auto* sprite : drawList.sprites)
		{
			if (sprite == nullptr || sprite->Sprite == nullptr || sprite->Sprite->ownerActor == nullptr)
			{
				continue;
			}

			if ((int32_t)sprite->Sprite->ownerActor->GetIndex() != actorIndex)
			{
				continue;
			}

			const bool isVoxelSprite =
				sprite->modelframe < 0 &&
				sprite->voxel != nullptr &&
				sprite->voxel->model != nullptr;
			if (requireVoxel ? isVoxelSprite : !isVoxelSprite)
			{
				count++;
			}
		}
		return count;
	}

	static void AccumulateSceneViewActorSurfaceStats(
		const nri_scene::SceneView& sceneView,
		int32_t actorIndex,
		uint32_t& outMatching,
		uint32_t& outOther,
		uint32_t& outActorless)
	{
		auto visit = [&](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				if (surface.provenance.actorIndex < 0)
				{
					outActorless++;
				}
				else if (surface.provenance.actorIndex == actorIndex)
				{
					outMatching++;
				}
				else
				{
					outOther++;
				}
			}
		};

		visit(sceneView.opaqueWalls);
		visit(sceneView.opaqueFlats);
		visit(sceneView.opaqueSprites);
	}

	static void TraceMirrorPlayerCaptureStats(const MirrorPlayerCaptureStats& stats)
	{
		static bool hasPrevious = false;
		static MirrorPlayerCaptureStats previous = {};
		if (!nri_ptscenestats)
		{
			hasPrevious = false;
			previous = {};
			return;
		}

		if (hasPrevious && !MirrorPlayerCaptureStatsDiffer(previous, stats))
		{
			return;
		}

		Printf("NRI PT mirror player capture: view_actor=%d local_actor=%d mirror_candidates=%u mirror_wall=%d camera_match=%s raw_facing=%u raw_voxels=%u captured=%s surfaces=%u match=%u other=%u actorless=%u filtered=%u\n",
			stats.viewpointActorIndex,
			stats.localPlayerActorIndex,
			stats.mirrorPortalCandidates,
			stats.selectedMirrorWallIndex,
			stats.viewpointMatchesLocalPlayer ? "yes" : "no",
			stats.rawFacingSprites,
			stats.rawVoxelSprites,
			stats.capturedScene ? "yes" : "no",
			stats.capturedSurfaceCount,
			stats.capturedMatchingActorSurfaces,
			stats.capturedOtherActorSurfaces,
			stats.capturedActorlessSurfaces,
			stats.filteredSurfaceCount);
		hasPrevious = true;
		previous = stats;
	}

	class ScopedMirrorPlayerVisibilityCaptureOverride
	{
public:
		explicit ScopedMirrorPlayerVisibilityCaptureOverride(bool enabled)
		{
			if (!enabled || gi == nullptr) return;
			mEnabled = true;
			mPrevious = gi->GetMirrorPlayerVisibilityCaptureOverride();
			gi->SetMirrorPlayerVisibilityCaptureOverride(true);
		}

		~ScopedMirrorPlayerVisibilityCaptureOverride()
		{
			if (mEnabled && gi != nullptr)
			{
				gi->SetMirrorPlayerVisibilityCaptureOverride(mPrevious);
			}
		}

		ScopedMirrorPlayerVisibilityCaptureOverride(const ScopedMirrorPlayerVisibilityCaptureOverride&) = delete;
		ScopedMirrorPlayerVisibilityCaptureOverride& operator=(const ScopedMirrorPlayerVisibilityCaptureOverride&) = delete;

	private:
		bool mEnabled = false;
		bool mPrevious = false;
	};

	static void RebuildSceneViewStats(nri_scene::SceneView& sceneView)
	{
		const nri_scene::SceneDebugStats preservedStats = sceneView.stats;
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += !wall.indices.empty() ? (uint32_t)(wall.indices.size() / 3u) : (wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += !flat.indices.empty() ? (uint32_t)(flat.indices.size() / 3u) : (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += !sprite.indices.empty() ? (uint32_t)(sprite.indices.size() / 3u) : (sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (sprite.provenance.sourceType == nri_scene::SurfaceSourceType::VoxelProxySprite)
			{
				stats.modelDrawItems++;
				stats.voxelProxyDrawItems++;
			}
			else
			{
				stats.translucentDrawItems++;
			}
		}

		stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems;
		stats.voxelStableCandidates = preservedStats.voxelStableCandidates;
		stats.voxelStableUncacheable = preservedStats.voxelStableUncacheable;
		stats.voxelStableSignatureHits = preservedStats.voxelStableSignatureHits;
		stats.voxelStableSignatureMisses = preservedStats.voxelStableSignatureMisses;
		stats.voxelStableSignatureChanges = preservedStats.voxelStableSignatureChanges;
		stats.voxelStableSplitStable = preservedStats.voxelStableSplitStable;
		stats.voxelStableSplitLive = preservedStats.voxelStableSplitLive;
		stats.voxelCacheEntries = preservedStats.voxelCacheEntries;
		stats.voxelCacheSurfaceHits = preservedStats.voxelCacheSurfaceHits;
		stats.voxelCacheSurfaceStores = preservedStats.voxelCacheSurfaceStores;
		stats.voxelCacheSurfaceRebuilds = preservedStats.voxelCacheSurfaceRebuilds;
		stats.voxelCacheTransformRebakes = preservedStats.voxelCacheTransformRebakes;
		stats.voxelCacheSurfaceRemoves = preservedStats.voxelCacheSurfaceRemoves;
		stats.voxelCacheNotCaptured = preservedStats.voxelCacheNotCaptured;
		stats.voxelCachePrimitives = preservedStats.voxelCachePrimitives;
		sceneView.stats = stats;
	}

	static HWPortal* SelectPrimaryMirrorPortal(const HWDrawInfo& di, uint32_t& outCandidateCount, int32_t& outSelectedWallIndex, int32_t preferredWallIndex = -1)
	{
		outCandidateCount = 0;
		outSelectedWallIndex = -1;
		const walltype* wallData = wall.Size() > 0 ? wall.Data() : nullptr;
		const walltype* wallDataEnd = wallData != nullptr ? wallData + wall.Size() : nullptr;
		const DVector2 cameraPos(di.Viewpoint.Pos.X, -di.Viewpoint.Pos.Y);
		const DVector2 cameraPosRaw = di.Viewpoint.Pos.XY();
		const DVector2 cameraDir = di.Viewpoint.ViewVector;
		HWPortal* preferredPortal = nullptr;
		HWPortal* closestCenterHitPortal = nullptr;
		double closestCenterHitDistance = std::numeric_limits<double>::infinity();
		int32_t closestCenterHitWallIndex = -1;
		HWPortal* bestPortal = nullptr;
		double bestDistanceSquared = std::numeric_limits<double>::infinity();
		double bestFacing = -std::numeric_limits<double>::infinity();
		int32_t bestPortalWallIndex = -1;
		for (HWPortal* portal : di.Portals)
		{
			if (portal == nullptr || portal->GetType() != PORTAL_WALL_MIRROR)
			{
				continue;
			}

			outCandidateCount++;
			auto* mirrorLine = static_cast<walltype*>(portal->GetSource());
			if (mirrorLine == nullptr)
			{
				continue;
			}
			if (wallData == nullptr || mirrorLine < wallData || mirrorLine >= wallDataEnd)
			{
				continue;
			}
			if (!validWallIndex(mirrorLine->point2))
			{
				continue;
			}

			const int32_t mirrorWallIndex = wall.IndexOf(mirrorLine);
			if (preferredWallIndex >= 0 && mirrorWallIndex == preferredWallIndex)
			{
				preferredPortal = portal;
			}

			const walltype* next = mirrorLine->point2Wall();
			if (next == nullptr)
			{
				continue;
			}

			const DVector2 segmentStart(mirrorLine->pos.X, -mirrorLine->pos.Y);
			const DVector2 segmentDelta(next->pos.X - mirrorLine->pos.X, -next->pos.Y + mirrorLine->pos.Y);
			const double denominator = cameraDir.X * segmentDelta.Y - cameraDir.Y * segmentDelta.X;
			if (std::abs(denominator) > 1.0e-6)
			{
				const DVector2 fromCamera = segmentStart - cameraPos;
				const double rayDistance = (fromCamera.X * segmentDelta.Y - fromCamera.Y * segmentDelta.X) / denominator;
				const double segmentFraction = (fromCamera.X * cameraDir.Y - fromCamera.Y * cameraDir.X) / denominator;
				if (rayDistance >= 0.0 && segmentFraction >= 0.0 && segmentFraction <= 1.0 && rayDistance < closestCenterHitDistance)
				{
					closestCenterHitDistance = rayDistance;
					closestCenterHitPortal = portal;
					closestCenterHitWallIndex = mirrorWallIndex;
				}
			}

			double distanceSquared = std::numeric_limits<double>::infinity();
			DVector2 nearestVisiblePoint = cameraPosRaw;
			for (const HWWall& line : portal->lines)
			{
				const DVector2 lineStart(line.glseg.x1, line.glseg.y1);
				const DVector2 lineDelta(line.glseg.x2 - line.glseg.x1, line.glseg.y2 - line.glseg.y1);
				const double lineLengthSquared = lineDelta.LengthSquared();
				if (lineLengthSquared <= 1.0e-6)
				{
					continue;
				}

				const double t = clamp<double>(((cameraPosRaw - lineStart) | lineDelta) / lineLengthSquared, 0.0, 1.0);
				const DVector2 candidatePoint = lineStart + lineDelta * t;
				const double candidateDistanceSquared = (cameraPosRaw - candidatePoint).LengthSquared();
				if (candidateDistanceSquared < distanceSquared)
				{
					distanceSquared = candidateDistanceSquared;
					nearestVisiblePoint = candidatePoint;
				}
			}
			if (!std::isfinite(distanceSquared))
			{
				distanceSquared = SquareDistToWall(di.Viewpoint.Pos.X, di.Viewpoint.Pos.Y, mirrorLine);
			}
			if (distanceSquared <= 0.0001)
			{
				outSelectedWallIndex = mirrorWallIndex;
				return portal;
			}

			const DVector2 nearestVisiblePointFlipped(nearestVisiblePoint.X, -nearestVisiblePoint.Y);
			const DVector2 toMirror = nearestVisiblePointFlipped - cameraPos;
			const double visibleDistanceSquared = toMirror.LengthSquared();
			if (visibleDistanceSquared <= 0.0001)
			{
				outSelectedWallIndex = mirrorWallIndex;
				return portal;
			}

			const DVector2 toMirrorDir = toMirror / sqrt(visibleDistanceSquared);
			const double facing = cameraDir | toMirrorDir;
			if (distanceSquared < bestDistanceSquared - 0.0001 ||
				(std::abs(distanceSquared - bestDistanceSquared) <= 0.0001 && facing > bestFacing))
			{
				bestDistanceSquared = distanceSquared;
				bestFacing = facing;
				bestPortal = portal;
				bestPortalWallIndex = mirrorWallIndex;
			}
		}

		if (closestCenterHitPortal != nullptr)
		{
			outSelectedWallIndex = closestCenterHitWallIndex;
			return closestCenterHitPortal;
		}

		if (bestPortal != nullptr)
		{
			outSelectedWallIndex = bestPortalWallIndex;
			return bestPortal;
		}

		if (preferredPortal != nullptr)
		{
			outSelectedWallIndex = preferredWallIndex;
			return preferredPortal;
		}

		return nullptr;
	}

	struct MirrorBillboardLayout
	{
		nri_scene::CapturedVertex topLeft = {};
		nri_scene::CapturedVertex bottomLeft = {};
		nri_scene::CapturedVertex topRight = {};
		nri_scene::CapturedVertex bottomRight = {};
		float topCenter[3] = {};
		float bottomCenter[3] = {};
		float prevTopCenter[3] = {};
		float prevBottomCenter[3] = {};
		float halfWidth = 0.0f;
		float prevHalfWidth = 0.0f;
	};

	static void AverageCapturedVertexPair(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, float outPosition[3], float outPrevPosition[3])
	{
		for (int i = 0; i < 3; ++i)
		{
			outPosition[i] = (a.position[i] + b.position[i]) * 0.5f;
			outPrevPosition[i] = (a.prevPosition[i] + b.prevPosition[i]) * 0.5f;
		}
	}

	static float ComputeCapturedHorizontalDistance(const nri_scene::CapturedVertex& a, const float center[3], bool previous)
	{
		const float dx = (previous ? a.prevPosition[0] : a.position[0]) - center[0];
		const float dz = (previous ? a.prevPosition[2] : a.position[2]) - center[2];
		return sqrtf(dx * dx + dz * dz);
	}

	static bool ExtractMirrorBillboardLayout(const nri_scene::SurfaceRef& sourceSurface, MirrorBillboardLayout& outLayout)
	{
		if (sourceSurface.vertices.size() != 4 ||
			(sourceSurface.material.flags & nri_scene::MaterialFlag_FacingBillboard) == 0)
		{
			return false;
		}

		std::array<nri_scene::CapturedVertex, 4> vertices = {
			sourceSurface.vertices[0],
			sourceSurface.vertices[1],
			sourceSurface.vertices[2],
			sourceSurface.vertices[3]
		};

		std::sort(vertices.begin(), vertices.end(), [](const auto& a, const auto& b)
		{
			return a.position[1] > b.position[1];
		});

		std::array<nri_scene::CapturedVertex, 2> topPair = { vertices[0], vertices[1] };
		std::array<nri_scene::CapturedVertex, 2> bottomPair = { vertices[2], vertices[3] };

		const float axisX = topPair[1].position[0] - topPair[0].position[0];
		const float axisZ = topPair[1].position[2] - topPair[0].position[2];
		const float axisLength = sqrtf(axisX * axisX + axisZ * axisZ);
		if (axisLength <= 0.0001f)
		{
			return false;
		}

		const float invAxisLength = 1.0f / axisLength;
		const float normAxisX = axisX * invAxisLength;
		const float normAxisZ = axisZ * invAxisLength;

		auto horizontalProjection = [normAxisX, normAxisZ](const nri_scene::CapturedVertex& vertex)
		{
			return vertex.position[0] * normAxisX + vertex.position[2] * normAxisZ;
		};

		if (horizontalProjection(topPair[0]) > horizontalProjection(topPair[1]))
		{
			std::swap(topPair[0], topPair[1]);
		}
		if (horizontalProjection(bottomPair[0]) > horizontalProjection(bottomPair[1]))
		{
			std::swap(bottomPair[0], bottomPair[1]);
		}

		outLayout.topLeft = topPair[0];
		outLayout.topRight = topPair[1];
		outLayout.bottomLeft = bottomPair[0];
		outLayout.bottomRight = bottomPair[1];
		AverageCapturedVertexPair(outLayout.topLeft, outLayout.topRight, outLayout.topCenter, outLayout.prevTopCenter);
		AverageCapturedVertexPair(outLayout.bottomLeft, outLayout.bottomRight, outLayout.bottomCenter, outLayout.prevBottomCenter);
		outLayout.halfWidth =
			(ComputeCapturedHorizontalDistance(outLayout.topLeft, outLayout.topCenter, false) +
				ComputeCapturedHorizontalDistance(outLayout.bottomLeft, outLayout.bottomCenter, false)) * 0.5f;
		outLayout.prevHalfWidth =
			(ComputeCapturedHorizontalDistance(outLayout.topLeft, outLayout.prevTopCenter, true) +
				ComputeCapturedHorizontalDistance(outLayout.bottomLeft, outLayout.prevBottomCenter, true)) * 0.5f;
		return outLayout.halfWidth > 0.0001f;
	}

	static bool ComputeMirroredViewVector(const HWDrawInfo& di, const walltype& mirrorLine, float& outViewX, float& outViewY)
	{
		const walltype* next = mirrorLine.point2Wall();
		if (next == nullptr)
		{
			return false;
		}

		float lineX = (float)(next->pos.X - mirrorLine.pos.X);
		float lineY = (float)(-next->pos.Y + mirrorLine.pos.Y);
		const float lineLength = sqrtf(lineX * lineX + lineY * lineY);
		if (lineLength <= 0.0001f)
		{
			return false;
		}

		lineX /= lineLength;
		lineY /= lineLength;
		const float viewX = (float)di.Viewpoint.ViewVector.X;
		const float viewY = (float)di.Viewpoint.ViewVector.Y;
		const float projection = viewX * lineX + viewY * lineY;
		outViewX = lineX * (projection * 2.0f) - viewX;
		outViewY = lineY * (projection * 2.0f) - viewY;
		const float reflectedLength = sqrtf(outViewX * outViewX + outViewY * outViewY);
		if (reflectedLength <= 0.0001f)
		{
			return false;
		}

		outViewX /= reflectedLength;
		outViewY /= reflectedLength;
		return true;
	}

	static bool ApplyWallMirrorViewpoint(const walltype& mirrorLine, FRenderViewpoint& viewpoint)
	{
		const walltype* next = mirrorLine.point2Wall();
		if (next == nullptr)
		{
			return false;
		}

		const double x = mirrorLine.pos.X;
		const double y = mirrorLine.pos.Y;
		const double dx = next->pos.X - x;
		const double dy = next->pos.Y - y;
		const double lengthSq = dx * dx + dy * dy;
		if (lengthSq <= 0.0001)
		{
			return false;
		}

		const DVector2 viewPos = { viewpoint.Pos.X, -viewpoint.Pos.Y };
		const double projection = ((viewPos.X - x) * dx + (viewPos.Y - y) * dy) * 2.0;
		const double mirroredX = x * 2.0 + dx * projection / lengthSq - viewPos.X;
		const double mirroredY = y * 2.0 + dy * projection / lengthSq - viewPos.Y;

		const angle_t mirrorAngle = VecToAngle(dx, dy).BAMs();
		const angle_t mirroredRotAngle = mirrorAngle + mirrorAngle - viewpoint.RotAngle;

		viewpoint.Pos.X = mirroredX;
		viewpoint.Pos.Y = -mirroredY;
		viewpoint.RotAngle = mirroredRotAngle;
		viewpoint.SectNums = nullptr;
		viewpoint.SectCount = mirrorLine.sector;
		viewpoint.HWAngles.Yaw = FAngle::fromBam(-ANGLE_90 + mirroredRotAngle);

		const double focalTangent = tan(viewpoint.FieldOfView.Radians() / 2.0);
		const DAngle facingAngle = DAngle::fromDeg(270.0 - viewpoint.HWAngles.Yaw.Degrees());
		viewpoint.TanSin = focalTangent * facingAngle.Sin();
		viewpoint.TanCos = focalTangent * facingAngle.Cos();
		viewpoint.ViewVector = facingAngle.ToVector();
		return true;
	}

	static uint64_t BuildDynamicSurfaceMergeKey(const nri_scene::SurfaceRef& surface)
	{
		float center[3] = {};
		ComputeCapturedSurfaceCenter(surface, center);
		return SceneLightSystem::ComputeSurfaceIdentityKey(
			SceneLightRecordSource::DynamicScene,
			surface.provenance,
			center);
	}

	static float ComputeSurfaceDistanceSquaredToViewpoint(const FRenderViewpoint& viewpoint, const nri_scene::SurfaceRef& surface)
	{
		float center[3] = {};
		ComputeCapturedSurfaceCenter(surface, center);
		const float dx = center[0] - (float)viewpoint.Pos.X;
		const float dy = center[1] - (float)viewpoint.Pos.Z;
		const float dz = center[2] - (float)viewpoint.Pos.Y;
		return dx * dx + dy * dy + dz * dz;
	}

	static void SeedDynamicSurfaceMergeKeys(const nri_scene::SceneView& sceneView, std::unordered_set<uint64_t>& outKeys)
	{
		auto append = [&outKeys](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				outKeys.insert(BuildDynamicSurfaceMergeKey(surface));
			}
		};

		append(sceneView.opaqueWalls);
		append(sceneView.opaqueFlats);
		append(sceneView.opaqueSprites);
	}

	struct MirrorExtendedAppendStats
	{
		uint32_t source = 0;
		uint32_t accepted = 0;
		uint32_t rejectedDistance = 0;
		uint32_t rejectedDuplicate = 0;
	};

	struct MirrorExtendedDrawListStats
	{
		uint32_t modelSprites = 0;
		uint32_t voxelSprites = 0;
		uint32_t facingSprites = 0;
	};

	static bool ShouldTraceMirrorDynamicCapture();

	static MirrorExtendedDrawListStats GatherMirrorExtendedDrawListStats(HWDrawInfo& di, uint32_t frameIndex)
	{
		MirrorExtendedDrawListStats stats = {};
		stats.modelSprites = di.drawlists[GLDL_MODELS].Size();
		stats.facingSprites = di.drawlists[GLDL_TRANSLUCENT].Size();

		const bool trace = ShouldTraceMirrorDynamicCapture();
		uint32_t tracedVoxelSprites = 0;
		for (auto* sprite : di.drawlists[GLDL_MODELS].sprites)
		{
			if (sprite == nullptr ||
				sprite->modelframe >= 0 ||
				sprite->voxel == nullptr ||
				sprite->voxel->model == nullptr)
			{
				continue;
			}

			stats.voxelSprites++;
			if (trace && tracedVoxelSprites < 16u)
			{
				const DCoreActor* actor = sprite->Sprite != nullptr ? sprite->Sprite->ownerActor : nullptr;
				const int actorIndex = actor != nullptr ? actor->GetIndex() : -1;
				const int statnum = sprite->Sprite != nullptr ? sprite->Sprite->statnum : -1;
				const int picnum = sprite->Sprite != nullptr ? sprite->Sprite->picnum : -1;
				Printf("PERF pt mirror voxel drawlist NRI: frame=%u rank=%u actor=%d stat=%d pic=%d tex=%d pos=(%.2f,%.2f,%.2f) alpha=%.3f voxel=%p model=%p\n",
					frameIndex,
					tracedVoxelSprites + 1u,
					actorIndex,
					statnum,
					picnum,
					sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1,
					(double)sprite->x,
					(double)sprite->y,
					(double)sprite->z,
					(double)sprite->alpha,
					(void*)sprite->voxel,
					sprite->voxel != nullptr ? (void*)sprite->voxel->model : nullptr);
			}
			tracedVoxelSprites++;
		}

		return stats;
	}

	static void AppendMirrorExtendedSurfaceList(
		const std::vector<nri_scene::SurfaceRef>& source,
		const FRenderViewpoint& viewpoint,
		float maxDistance,
		std::unordered_set<uint64_t>& existingKeys,
		std::vector<nri_scene::SurfaceRef>& destination,
		MirrorExtendedAppendStats& stats)
	{
		(void)viewpoint;
		(void)maxDistance;
		for (const nri_scene::SurfaceRef& surface : source)
		{
			stats.source++;
			const uint64_t key = BuildDynamicSurfaceMergeKey(surface);
			if (!existingKeys.insert(key).second)
			{
				stats.rejectedDuplicate++;
				continue;
			}

			destination.push_back(surface);
			stats.accepted++;
		}
	}

	static nri_scene::CapturedVertex MakeMirrorBillboardVertex(const nri_scene::CapturedVertex& source, const float center[3], float widthAxisX, float widthAxisY, float halfWidth, bool rightSide, bool previous)
	{
		nri_scene::CapturedVertex result = source;
		const float side = rightSide ? 1.0f : -1.0f;
		float* destination = previous ? result.prevPosition : result.position;
		destination[0] = center[0] + widthAxisX * halfWidth * side;
		destination[1] = center[1];
		destination[2] = center[2] + widthAxisY * halfWidth * side;
		return result;
	}

	static bool ReorientFacingBillboardForMirror(const HWDrawInfo& di, const walltype& mirrorLine, const nri_scene::SurfaceRef& sourceSurface, nri_scene::SurfaceRef& outSurface)
	{
		MirrorBillboardLayout layout = {};
		if (!ExtractMirrorBillboardLayout(sourceSurface, layout))
		{
			return false;
		}

		float mirroredViewX = 0.0f;
		float mirroredViewY = 0.0f;
		if (!ComputeMirroredViewVector(di, mirrorLine, mirroredViewX, mirroredViewY))
		{
			return false;
		}

		const float widthAxisX = -mirroredViewY;
		const float widthAxisY = mirroredViewX;
		const float prevHalfWidth = layout.prevHalfWidth > 0.0001f ? layout.prevHalfWidth : layout.halfWidth;

		outSurface = sourceSurface;
		outSurface.vertices.resize(4);
		outSurface.vertices[0] = MakeMirrorBillboardVertex(layout.topLeft, layout.topCenter, widthAxisX, widthAxisY, layout.halfWidth, false, false);
		outSurface.vertices[1] = MakeMirrorBillboardVertex(layout.topRight, layout.topCenter, widthAxisX, widthAxisY, layout.halfWidth, true, false);
		outSurface.vertices[2] = MakeMirrorBillboardVertex(layout.bottomLeft, layout.bottomCenter, widthAxisX, widthAxisY, layout.halfWidth, false, false);
		outSurface.vertices[3] = MakeMirrorBillboardVertex(layout.bottomRight, layout.bottomCenter, widthAxisX, widthAxisY, layout.halfWidth, true, false);
		outSurface.vertices[0] = MakeMirrorBillboardVertex(outSurface.vertices[0], layout.prevTopCenter, widthAxisX, widthAxisY, prevHalfWidth, false, true);
		outSurface.vertices[1] = MakeMirrorBillboardVertex(outSurface.vertices[1], layout.prevTopCenter, widthAxisX, widthAxisY, prevHalfWidth, true, true);
		outSurface.vertices[2] = MakeMirrorBillboardVertex(outSurface.vertices[2], layout.prevBottomCenter, widthAxisX, widthAxisY, prevHalfWidth, false, true);
		outSurface.vertices[3] = MakeMirrorBillboardVertex(outSurface.vertices[3], layout.prevBottomCenter, widthAxisX, widthAxisY, prevHalfWidth, true, true);
		return true;
	}

	static bool AppendMirrorPlayerSurfaces(const HWDrawInfo& di, const nri_scene::SceneView& sourceView, nri_scene::SceneView& outView)
	{
		for (const nri_scene::SurfaceRef& sourceSurface : sourceView.opaqueSprites)
		{
			outView.opaqueSprites.push_back(sourceSurface);
		}

		return !outView.opaqueSprites.empty();
	}

	static bool CaptureMirrorExtendedDynamicScene(
		HWDrawInfo& di,
		HWPortal* mirrorPortal,
		int32_t selectedMirrorWallIndex,
		const nri_scene::SceneView* baseDynamicSceneView,
		uint32_t frameIndex,
		nri_scene::SceneView& outView)
	{
		outView = {};
		if (mirrorPortal == nullptr || nri_ptmirrordynamicdistance <= 0.0f)
		{
			return false;
		}

		auto* mirrorLine = static_cast<walltype*>(mirrorPortal->GetSource());
		if (mirrorLine == nullptr)
		{
			return false;
		}

		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;
		if (!ApplyWallMirrorViewpoint(*mirrorLine, captureDi->Viewpoint))
		{
			captureDi->EndDrawInfo();
			return false;
		}
		const FRenderViewpoint mirrorCaptureViewpoint = captureDi->Viewpoint;

		captureDi->CreateScene(false);
		const MirrorExtendedDrawListStats rawDrawListStats = GatherMirrorExtendedDrawListStats(*captureDi, frameIndex);
		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureDynamicScene(*captureDi, capturedView, nri_scene::DynamicVoxelCaptureMode::MirrorResidencyRequest);
		captureDi->EndDrawInfo();
		if (!hasCapture)
		{
			if (ShouldTraceMirrorDynamicCapture())
			{
				Printf("PERF pt mirror capture NRI: frame=%u result=empty wall=%d distance=%.1f raw_models=%u raw_voxels=%u raw_facing=%u captured_walls=0 captured_flats=0 captured_sprites=0 accepted_walls=0 accepted_flats=0 accepted_sprites=0 dist_rejects=0 duplicate_rejects=0 voxel_candidates=0 voxel_hits=0 voxel_misses=0 voxel_not_captured=0\n",
					frameIndex,
					selectedMirrorWallIndex,
					(double)nri_ptmirrordynamicdistance,
					rawDrawListStats.modelSprites,
					rawDrawListStats.voxelSprites,
					rawDrawListStats.facingSprites);
			}
			return false;
		}

		std::unordered_set<uint64_t> existingKeys;
		if (baseDynamicSceneView != nullptr)
		{
			SeedDynamicSurfaceMergeKeys(*baseDynamicSceneView, existingKeys);
		}

		outView.drawInfo = &di;
		MirrorExtendedAppendStats wallAppendStats = {};
		MirrorExtendedAppendStats flatAppendStats = {};
		MirrorExtendedAppendStats spriteAppendStats = {};
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueWalls,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueWalls,
			wallAppendStats);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueFlats,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueFlats,
			flatAppendStats);
		AppendMirrorExtendedSurfaceList(
			capturedView.opaqueSprites,
			mirrorCaptureViewpoint,
			nri_ptmirrordynamicdistance,
			existingKeys,
			outView.opaqueSprites,
			spriteAppendStats);
		if (ShouldTraceMirrorDynamicCapture())
		{
			Printf("PERF pt mirror capture NRI: frame=%u result=%s wall=%d distance=%.1f raw_models=%u raw_voxels=%u raw_facing=%u captured_walls=%u captured_flats=%u captured_sprites=%u accepted_walls=%u accepted_flats=%u accepted_sprites=%u dist_rejects=%u duplicate_rejects=%u voxel_candidates=%u voxel_hits=%u voxel_misses=%u voxel_not_captured=%u\n",
				frameIndex,
				(outView.opaqueWalls.empty() && outView.opaqueFlats.empty() && outView.opaqueSprites.empty()) ? "filtered" : "accepted",
				selectedMirrorWallIndex,
				(double)nri_ptmirrordynamicdistance,
				rawDrawListStats.modelSprites,
				rawDrawListStats.voxelSprites,
				rawDrawListStats.facingSprites,
				(uint32_t)capturedView.opaqueWalls.size(),
				(uint32_t)capturedView.opaqueFlats.size(),
				(uint32_t)capturedView.opaqueSprites.size(),
				wallAppendStats.accepted,
				flatAppendStats.accepted,
				spriteAppendStats.accepted,
				wallAppendStats.rejectedDistance + flatAppendStats.rejectedDistance + spriteAppendStats.rejectedDistance,
				wallAppendStats.rejectedDuplicate + flatAppendStats.rejectedDuplicate + spriteAppendStats.rejectedDuplicate,
				capturedView.stats.voxelStableCandidates,
				capturedView.stats.voxelCacheSurfaceHits,
				capturedView.stats.voxelStableSignatureMisses,
				capturedView.stats.voxelCacheNotCaptured);
		}
		if (outView.opaqueWalls.empty() && outView.opaqueFlats.empty() && outView.opaqueSprites.empty())
		{
			outView = {};
			return false;
		}

		outView.primitiveFlags = nri_scene::PrimitiveFlag_ReflectionOnly;
		RebuildSceneViewStats(outView);
		return true;
	}

	static bool CaptureMirrorPlayerDynamicScene(
		HWDrawInfo& di,
		HWPortal* mirrorPortal,
		int32_t selectedMirrorWallIndex,
		uint32_t mirrorPortalCandidates,
		nri_scene::SceneView& outView,
		MirrorPlayerCaptureStats* outStats = nullptr)
	{
		outView = {};
		MirrorPlayerCaptureStats captureStats = {};
		captureStats.viewpointActorIndex = di.Viewpoint.CameraActor != nullptr ? (int32_t)di.Viewpoint.CameraActor->GetIndex() : -1;
		const auto publishStats = [&]()
		{
			if (outStats != nullptr)
			{
				*outStats = captureStats;
			}
			TraceMirrorPlayerCaptureStats(captureStats);
		};
		if (gi == nullptr ||
			myconnectindex < 0 ||
			myconnectindex >= MAXPLAYERS)
		{
			publishStats();
			return false;
		}

		DCorePlayer* localPlayer = PlayerArray[myconnectindex];
		DCoreActor* localPlayerActor = localPlayer != nullptr ? localPlayer->GetActor() : nullptr;
		if (localPlayerActor == nullptr)
		{
			publishStats();
			return false;
		}

		const int32_t actorIndex = (int32_t)localPlayerActor->GetIndex();
		captureStats.localPlayerActorIndex = actorIndex;
		captureStats.viewpointMatchesLocalPlayer = di.Viewpoint.CameraActor == localPlayerActor;
		captureStats.mirrorPortalCandidates = mirrorPortalCandidates;
		captureStats.selectedMirrorWallIndex = selectedMirrorWallIndex;
		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;
		if (mirrorPortal != nullptr)
		{
			auto* mirrorLine = static_cast<walltype*>(mirrorPortal->GetSource());
			if (mirrorLine != nullptr)
			{
				ApplyWallMirrorViewpoint(*mirrorLine, captureDi->Viewpoint);
			}
		}

		const ScopedMirrorPlayerVisibilityCaptureOverride mirrorCaptureOverride(true);
		captureDi->CreateScene(false);
		captureStats.rawFacingSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_TRANSLUCENT], actorIndex, false);
		captureStats.rawVoxelSprites = CountDrawListActorSprites(captureDi->drawlists[GLDL_MODELS], actorIndex, true);

		nri_scene::SceneView capturedView;
		const bool hasCapture = nri_scene::CaptureActorSpriteScene(*captureDi, actorIndex, capturedView);
		captureDi->EndDrawInfo();
		if (!hasCapture || !AppendMirrorPlayerSurfaces(di, capturedView, outView))
		{
			publishStats();
			outView = {};
			return false;
		}

		outView.drawInfo = &di;
		RebuildSceneViewStats(outView);
		captureStats.capturedScene = true;
		captureStats.capturedSurfaceCount = CountSceneViewSurfaces(outView);
		AccumulateSceneViewActorSurfaceStats(
			outView,
			actorIndex,
			captureStats.capturedMatchingActorSurfaces,
			captureStats.capturedOtherActorSurfaces,
			captureStats.capturedActorlessSurfaces);

		outView.primitiveFlags = nri_scene::PrimitiveFlag_ReflectionOnly;
		captureStats.filteredSurfaceCount = captureStats.capturedSurfaceCount;
		publishStats();
		return true;
	}

	static bool IsMirrorPlayerPreviewCaptureEnabled()
	{
		// Phase 5 merges the captured local-player slice into the live PT dynamic
		// overlay and phase 4 marks it reflection-only, so the extra capture pass is
		// now consumed by the active mirror path instead of running as inert preview work.
		return true;
	}

	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	static bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceMirrorDynamicCapture()
	{
		return nri_voxelstats || (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0;
	}

	static constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	static MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	static void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

	static uint32_t CoherencyFloatBits(float value)
	{
		static_assert(sizeof(uint32_t) == sizeof(float), "unexpected float size");
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}

	static uint64_t HashMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return HashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	struct SceneViewUploadStampBuildResult
	{
		uint64_t vertexPayloadStamp = 0;
		uint64_t indexPayloadStamp = 0;
		uint64_t primitivePayloadStamp = 0;
		uint64_t primitiveProvenanceStamp = 0;
		uint64_t materialPayloadStamp = 0;
	};

	static uint64_t HashSurfaceProvenanceStamp(uint64_t hash, const nri_scene::SurfaceProvenance& provenance)
	{
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.drawListType);
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.cstat);
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.materialFlags);
		return hash;
	}

	static uint64_t HashCapturedVertexStamp(uint64_t hash, const nri_scene::CapturedVertex& vertex)
	{
		for (int i = 0; i < 3; ++i)
		{
			hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.position[i]));
		}
		for (int i = 0; i < 3; ++i)
		{
			hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.prevPosition[i]));
		}
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.uv[0]));
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(vertex.uv[1]));
		return hash;
	}

	static uint64_t HashMaterialRefStamp(uint64_t hash, const nri_scene::MaterialRef& material)
	{
		hash = nri_scene::HashCombine64(hash, material.texture != nullptr ? (uint64_t)(uint32_t)material.texture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, material.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)material.emissiveSourceTexture->GetID().GetIndex() + 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.palette + 1));
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(material.shade + 1));
		hash = nri_scene::HashCombine64(hash, CoherencyFloatBits(material.alpha));
		hash = nri_scene::HashCombine64(hash, (uint64_t)material.flags);
		return hash;
	}

	static uint32_t CountStampedSurfacePrimitives(const nri_scene::SurfaceRef& surface, bool triangleList)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		if (triangleList)
		{
			return (uint32_t)(surface.vertices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	static SceneViewUploadStampBuildResult BuildSceneViewUploadProducerStamp(const nri_scene::SceneView& sceneView, uint64_t mapWorldBuildSerial)
	{
		SceneViewUploadStampBuildResult result = {};
		result.vertexPayloadStamp = 1469598103934665603ull;
		result.indexPayloadStamp = 1469598103934665603ull;
		result.primitivePayloadStamp = 1469598103934665603ull;
		result.primitiveProvenanceStamp = 1469598103934665603ull;
		result.materialPayloadStamp = 1469598103934665603ull;
		auto appendSurface =
			[&](const nri_scene::SurfaceRef& surface, uint32_t surfaceKind, bool triangleList, uint32_t materialIndex)
		{
			const uint32_t primitiveCount = CountStampedSurfacePrimitives(surface, triangleList);
			const uint64_t surfaceHeader =
				nri_scene::HashCombine64(
					nri_scene::HashCombine64(
						nri_scene::HashCombine64(1469598103934665603ull, (uint64_t)surfaceKind),
						(uint64_t)materialIndex),
					(uint64_t)primitiveCount);
			result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, surfaceHeader);
			result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, surfaceHeader);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, surfaceHeader);
			result.primitiveProvenanceStamp = nri_scene::HashCombine64(result.primitiveProvenanceStamp, surfaceHeader);
			result.materialPayloadStamp = nri_scene::HashCombine64(result.materialPayloadStamp, surfaceHeader);
			result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, (uint64_t)surface.vertices.size());
			result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)surface.indices.size());
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)sceneView.primitiveFlags);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)surface.material.flags);
			result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, mapWorldBuildSerial);
			result.primitiveProvenanceStamp = HashSurfaceProvenanceStamp(result.primitiveProvenanceStamp, surface.provenance);
			result.materialPayloadStamp = HashMaterialRefStamp(result.materialPayloadStamp, surface.material);
			for (const nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				result.vertexPayloadStamp = HashCapturedVertexStamp(result.vertexPayloadStamp, vertex);
				result.primitivePayloadStamp = HashCapturedVertexStamp(result.primitivePayloadStamp, vertex);
			}
			for (uint32_t index : surface.indices)
			{
				result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)index);
				result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)index);
			}
		};

		uint32_t materialIndex = 0;
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			appendSurface(surface, 0u, false, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			appendSurface(surface, 1u, true, materialIndex++);
		}
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueSprites)
		{
			appendSurface(surface, 2u, false, materialIndex++);
		}
		result.vertexPayloadStamp = nri_scene::HashCombine64(result.vertexPayloadStamp, (uint64_t)materialIndex);
		result.indexPayloadStamp = nri_scene::HashCombine64(result.indexPayloadStamp, (uint64_t)materialIndex);
		result.primitivePayloadStamp = nri_scene::HashCombine64(result.primitivePayloadStamp, (uint64_t)materialIndex);
		result.primitiveProvenanceStamp = nri_scene::HashCombine64(result.primitiveProvenanceStamp, (uint64_t)materialIndex);
		result.materialPayloadStamp = nri_scene::HashCombine64(result.materialPayloadStamp, (uint64_t)materialIndex);
		result.vertexPayloadStamp = result.vertexPayloadStamp != 0 ? result.vertexPayloadStamp : 1;
		result.indexPayloadStamp = result.indexPayloadStamp != 0 ? result.indexPayloadStamp : 1;
		result.primitivePayloadStamp = result.primitivePayloadStamp != 0 ? result.primitivePayloadStamp : 1;
		result.primitiveProvenanceStamp = result.primitiveProvenanceStamp != 0 ? result.primitiveProvenanceStamp : 1;
		result.materialPayloadStamp = result.materialPayloadStamp != 0 ? result.materialPayloadStamp : 1;
		return result;
	}

	static uint64_t BuildConservativeMirrorPlayerPayloadStamp(
		uint64_t kind,
		uint64_t frameIndex,
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t mapWorldBuildSerial)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, kind);
		hash = nri_scene::HashCombine64(hash, frameIndex);
		hash = nri_scene::HashCombine64(hash, mapWorldBuildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.vertices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.indices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitives.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitiveProvenance.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.materials.size());
		return hash != 0 ? hash : 1;
	}

	static SceneViewUploadStampBuildResult BuildMirrorPlayerUploadProducerStamp(
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t frameIndex,
		uint64_t mapWorldBuildSerial)
	{
		SceneViewUploadStampBuildResult stamp = {};
		stamp.vertexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(1u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.indexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(2u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitivePayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(3u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitiveProvenanceStamp = BuildConservativeMirrorPlayerPayloadStamp(4u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.materialPayloadStamp = HashMaterialPayloadData(materials);
		return stamp;
	}

static void Copy3(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 3);
}

static void Copy2(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 2);
}

static const char* YesNo(bool value)
{
	return value ? "yes" : "no";
}

static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
{
	return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool ShouldTracePtPerf()
{
	return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
}

static bool ShouldCollectPtPerfTiming()
{
	return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace;
}

class ScopedPtPerfTimer
{
public:
	explicit ScopedPtPerfTimer(double& targetMs)
		: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
	{
		if (mTarget != nullptr)
		{
			mStart = std::chrono::steady_clock::now();
		}
	}

	~ScopedPtPerfTimer()
	{
		if (mTarget != nullptr)
		{
			*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
		}
	}

private:
	double* mTarget = nullptr;
	std::chrono::steady_clock::time_point mStart = {};
};

static bool MaterialDataEqual(const nri_scene::MaterialData& a, const nri_scene::MaterialData& b)
{
	return
		a.textureIndex == b.textureIndex &&
		a.paletteIndex == b.paletteIndex &&
		a.flags == b.flags &&
		a.materialClass == b.materialClass &&
		a.lightingFlags == b.lightingFlags &&
		a.normalTextureIndex == b.normalTextureIndex &&
		a.metallicTextureIndex == b.metallicTextureIndex &&
		a.roughnessTextureIndex == b.roughnessTextureIndex &&
		a.sectorIndex == b.sectorIndex &&
		a.emissiveTextureIndex == b.emissiveTextureIndex &&
		a.lightLevel == b.lightLevel &&
		a.alpha == b.alpha &&
		a.roughnessHint == b.roughnessHint &&
		a.metalnessHint == b.metalnessHint &&
		a.emissiveColor[0] == b.emissiveColor[0] &&
		a.emissiveColor[1] == b.emissiveColor[1] &&
		a.emissiveColor[2] == b.emissiveColor[2] &&
		a.emissiveIntensity == b.emissiveIntensity &&
		a.emissiveMaskScale == b.emissiveMaskScale &&
		a.emissiveMode == b.emissiveMode &&
		a.emissiveReserved == b.emissiveReserved;
}

static bool MaterialDataVectorEqual(const std::vector<nri_scene::MaterialData>& a, const std::vector<nri_scene::MaterialData>& b)
{
	if (a.size() != b.size())
	{
		return false;
	}

	for (size_t i = 0; i < a.size(); ++i)
	{
		if (!MaterialDataEqual(a[i], b[i]))
		{
			return false;
		}
	}

	return true;
}
}


NRIRenderer::RenderSceneHistorySnapshot NRIRenderer::CaptureRenderSceneHistorySnapshot(bool preserveHistory) const
{
	RenderSceneHistorySnapshot snapshot = {};
	snapshot.frameIndex = mFrameIndex;
	snapshot.currentTanHalfFovX = mCurrentTanHalfFovX;
	snapshot.currentTanHalfFovY = mCurrentTanHalfFovY;
	snapshot.previousTanHalfFovX = mPreviousTanHalfFovX;
	snapshot.previousTanHalfFovY = mPreviousTanHalfFovY;
	snapshot.hasPreviousCameraState = mHasPreviousCameraState;
	snapshot.resetHistory = mResetHistory;
	if (preserveHistory)
	{
		Copy3(mCurrentCameraPos, snapshot.currentCameraPos);
		Copy3(mCurrentCameraForward, snapshot.currentCameraForward);
		Copy3(mCurrentCameraRight, snapshot.currentCameraRight);
		Copy3(mCurrentCameraUp, snapshot.currentCameraUp);
		Copy3(mPreviousCameraPos, snapshot.previousCameraPos);
		Copy3(mPreviousCameraForward, snapshot.previousCameraForward);
		Copy3(mPreviousCameraRight, snapshot.previousCameraRight);
		Copy3(mPreviousCameraUp, snapshot.previousCameraUp);
		Copy2(mCurrentJitter, snapshot.currentJitter);
		Copy2(mPreviousJitter, snapshot.previousJitter);
		std::memcpy(snapshot.currentViewToClip, mCurrentViewToClip, sizeof(snapshot.currentViewToClip));
		std::memcpy(snapshot.previousViewToClip, mPreviousViewToClip, sizeof(snapshot.previousViewToClip));
		std::memcpy(snapshot.currentWorldToView, mCurrentWorldToView, sizeof(snapshot.currentWorldToView));
		std::memcpy(snapshot.previousWorldToView, mPreviousWorldToView, sizeof(snapshot.previousWorldToView));
	}
	return snapshot;
}

void NRIRenderer::RestoreRenderSceneHistorySnapshot(const RenderSceneHistorySnapshot& snapshot)
{
	mFrameIndex = snapshot.frameIndex;
	Copy3(snapshot.currentCameraPos, mCurrentCameraPos);
	Copy3(snapshot.currentCameraForward, mCurrentCameraForward);
	Copy3(snapshot.currentCameraRight, mCurrentCameraRight);
	Copy3(snapshot.currentCameraUp, mCurrentCameraUp);
	Copy3(snapshot.previousCameraPos, mPreviousCameraPos);
	Copy3(snapshot.previousCameraForward, mPreviousCameraForward);
	Copy3(snapshot.previousCameraRight, mPreviousCameraRight);
	Copy3(snapshot.previousCameraUp, mPreviousCameraUp);
	Copy2(snapshot.currentJitter, mCurrentJitter);
	Copy2(snapshot.previousJitter, mPreviousJitter);
	std::memcpy(mCurrentViewToClip, snapshot.currentViewToClip, sizeof(mCurrentViewToClip));
	std::memcpy(mPreviousViewToClip, snapshot.previousViewToClip, sizeof(mPreviousViewToClip));
	std::memcpy(mCurrentWorldToView, snapshot.currentWorldToView, sizeof(mCurrentWorldToView));
	std::memcpy(mPreviousWorldToView, snapshot.previousWorldToView, sizeof(mPreviousWorldToView));
	mCurrentTanHalfFovX = snapshot.currentTanHalfFovX;
	mCurrentTanHalfFovY = snapshot.currentTanHalfFovY;
	mPreviousTanHalfFovX = snapshot.previousTanHalfFovX;
	mPreviousTanHalfFovY = snapshot.previousTanHalfFovY;
	mHasPreviousCameraState = snapshot.hasPreviousCameraState;
	mResetHistory = snapshot.resetHistory;
}

bool NRIRenderer::EnsureRenderSceneFrameResources(const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	bool ready = false;
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		ready =
			Initialize() &&
			NRIFrameResources::EnsureFrameResources(
				*this,
				frameContext.outputWidth,
				frameContext.outputHeight,
				frameContext.targetWidth,
				frameContext.targetHeight);
	}
	if (!ready)
	{
		LogFallback("PT frame resources or pipelines failed to initialize.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	return true;
}

bool NRIRenderer::BeginRenderSceneFrame(HWDrawInfo& di, const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mHasVisibleMirrorPortalLastFrame = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;
	mDynamicSceneLastFrame = {};
	mRuntimeMutation.BeginFrameState();
	mRuntimeSpaceLinkLastFrame = {};
	if (!preserveHistory)
	{
		mPendingFrameGenerationTimestamp = std::chrono::steady_clock::now();
		mHasPendingFrameGenerationRealFrameTime = false;
		mPendingFrameGenerationRealFrameTimeMs = 0.0f;
		if (mHasFrameGenerationTimestamp)
		{
			const auto elapsed = mPendingFrameGenerationTimestamp - mLastFrameGenerationTimestamp;
			mPendingFrameGenerationRealFrameTimeMs = (float)std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();
			mHasPendingFrameGenerationRealFrameTime = true;
			if (mPendingFrameGenerationRealFrameTimeMs > 250.0f)
			{
				RequestHistoryReset("cadence-break");
			}
		}
	}
	UpdateFrameGenerationHistoryPolicy(frameContext.debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), frameContext.preserveHistory);

	RefreshMapWorld();
	if (!ApplyStartupMapWorldCorrectionIfNeeded("render-frame-start"))
	{
		LogFallback("PT startup world correction failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (mPendingStaticMapLightingInvalidation)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied++;
		}
		InvalidateStaticMapSceneForMaterialLighting();
		mPendingStaticMapLightingInvalidation = false;
	}
	UpdatePerFrameState(di);
	if (preserveHistory)
	{
		mResetHistory = true;
	}
	return true;
}

bool NRIRenderer::RenderSimpleBootstrapView(bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::Composed;
	mUseUpscaledInFinal = false;
	NRIPassDispatchContext passContext = BuildPassDispatchContext();
	if (!NRIPassDispatcher::DispatchBootstrapView(passContext))
	{
		LogFallback("PT bootstrap view dispatch failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	CopyFinalToActiveTarget();
	if (!preserveHistory)
	{
		NoteSuccessfulRealFrame();
		++mFrameIndex;
		mHasPreviousCameraState = true;
		mResetHistory = false;
	}
	else
	{
		RestoreRenderSceneHistorySnapshot(history);
	}
	return true;
}

bool NRIRenderer::DispatchSelectedRenderScene(const RenderSceneDispatchInputs& inputs)
{
	if (inputs.bootstrapCapturedView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		NRIPassDispatchContext passContext = BuildPassDispatchContext();
		return inputs.buffersReady && NRIPassDispatcher::DispatchBootstrapView(passContext);
	}

	NRIPassDispatchContext passContext = BuildPassDispatchContext();
	return inputs.accelerationReady &&
		inputs.drawInfo != nullptr &&
		inputs.activeGeometry != nullptr &&
		inputs.activeGpuMaterials != nullptr &&
		NRIPassDispatcher::DispatchFrameGraph(passContext, *inputs.drawInfo, *inputs.activeGeometry, *inputs.activeGpuMaterials, inputs.drawmode);
}

void NRIRenderer::LogRenderSceneFailureReasons(bool paletteReady, bool texturesReady, bool buffersReady, bool accelerationReady, bool dispatched, bool bootstrapCapturedView)
{
	if (!paletteReady)
	{
		LogFallback("PT palette texture upload failed.");
	}
	else if (!texturesReady)
	{
		LogFallback("PT material texture upload failed.");
	}
	else if (!buffersReady)
	{
		LogFallback("PT scene buffer upload failed.");
	}
	else if (!accelerationReady)
	{
		LogFallback("PT acceleration structure build failed.");
	}
	else if (!dispatched)
	{
		LogFallback(bootstrapCapturedView ? "PT bootstrap captured-scene dispatch failed." : "PT frame graph dispatch failed.");
	}
}

void NRIRenderer::CommitRenderSceneResult(const RenderSceneCompletionInputs& inputs, const RenderSceneHistorySnapshot& history)
{
	if (inputs.success)
	{
		mHasLoggedFallback = false;
		if (inputs.bootstrapCapturedView)
		{
			CopyFinalToActiveTarget();
		}

		if (!inputs.preserveHistory)
		{
			NoteSuccessfulRealFrame();
			mFrameIndex++;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
	}
	else if (inputs.preserveHistory)
	{
		RestoreRenderSceneHistorySnapshot(history);
	}

	if (inputs.success)
	{
		RecordRenderSceneSuccessStats(inputs);
		EmitSelfTestSummary(inputs.traceFrameIndex, inputs.drawmode, inputs.portal);
	}
	EmitRenderSceneTemporalTrace(inputs.traceFrameIndex);
}

void NRIRenderer::RecordRenderSceneSuccessStats(const RenderSceneCompletionInputs& inputs)
{
	if (inputs.activeGeometry == nullptr || inputs.activeGpuMaterials == nullptr)
	{
		return;
	}

	mLastPerfShellTraceStats.activePrimitiveCount = (uint32_t)inputs.activeGeometry->primitives.size();
	mLastPerfShellTraceStats.dynamicPrimitiveCount = inputs.activeDynamicGeometry != nullptr ? (uint32_t)inputs.activeDynamicGeometry->primitives.size() : 0u;
	mLastPerfShellTraceStats.activeMaterialCount = (uint32_t)inputs.activeGpuMaterials->size();
	mLastPerfShellTraceStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
	mLastPerfShellTraceStats.sceneInstanceStaticCount = 0;
	mLastPerfShellTraceStats.sceneInstanceDynamicCount = 0;
	mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount = 0;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneInstanceStatsMs);
		for (const SceneInstanceData& instance : mBoundSceneInstances)
		{
			if (instance.dataSource == nri_diag::SceneDataSourceStatic)
			{
				mLastPerfShellTraceStats.sceneInstanceStaticCount++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourceDynamic)
			{
				mLastPerfShellTraceStats.sceneInstanceDynamicCount++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
			{
				mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount++;
			}
		}
	}
	NRIPersistentVoxelStatusSnapshot persistentVoxelStatus = {};
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelResourceStatsMs);
		mPersistentVoxels.FillResourceStatusSnapshot(persistentVoxelStatus);
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelBatchStatsMs);
		mPersistentVoxels.FillBatchStatusSnapshot(persistentVoxelStatus);
	}
	mLastPerfShellTraceStats.persistentVoxelMeshVariantResourceCount = persistentVoxelStatus.meshVariantResourceCount;
	mLastPerfShellTraceStats.persistentVoxelMaterialVariantResourceCount = persistentVoxelStatus.materialVariantResourceCount;
	mLastPerfShellTraceStats.persistentVoxelBatchActorCount = persistentVoxelStatus.batchActorCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceRecordCount = persistentVoxelStatus.instanceRecordCount;
	mLastPerfShellTraceStats.persistentVoxelAdmissionQueueCount = persistentVoxelStatus.admissionQueueCount;
	mLastPerfShellTraceStats.persistentVoxelPendingInstanceCount = persistentVoxelStatus.pendingInstanceCount;
	mLastPerfShellTraceStats.persistentVoxelResidentResourceBytes = persistentVoxelStatus.residentResourceBytes;
	mLastPerfShellTraceStats.persistentVoxelZeroRefResourceBytes = persistentVoxelStatus.zeroRefResourceBytes;
	mLastPerfShellTraceStats.persistentVoxelZeroRefMeshResourceCount = persistentVoxelStatus.zeroRefMeshResourceCount;
	mLastPerfShellTraceStats.persistentVoxelZeroRefMaterialResourceCount = persistentVoxelStatus.zeroRefMaterialResourceCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceActiveCount = persistentVoxelStatus.activeInstanceCount;
	mLastPerfShellTraceStats.persistentVoxelInstancePrimitiveCount = persistentVoxelStatus.instancePrimitiveCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMaterialCount = persistentVoxelStatus.instanceMaterialCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMinPrimitiveCount = persistentVoxelStatus.instanceMinPrimitiveCount;
	mLastPerfShellTraceStats.persistentVoxelInstanceMaxPrimitiveCount = persistentVoxelStatus.instanceMaxPrimitiveCount;
	mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
	mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
	mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = inputs.usingPersistentDynamicEmissiveCache;
	const double accountedMs =
		mLastPerfShellTraceStats.initResourcesMs +
		mLastPerfShellTraceStats.mapWorldMs +
		mLastPerfShellTraceStats.updateStateMs +
		mLastPerfShellTraceStats.sceneSelectMs +
		mLastPerfShellTraceStats.sceneLightsMs +
		mLastPerfShellTraceStats.residentLightRefreshMs +
		mLastPerfShellTraceStats.emissiveUpdateMs +
		mLastPerfShellTraceStats.emissiveTlasMs +
		mLastPerfShellTraceStats.surfaceProbeMs +
		mLastPerfShellTraceStats.frameGraphMs;
	mLastPerfShellTraceStats.otherMs = std::max(0.0, mLastPerfShellTraceStats.totalMs - accountedMs);
}

void NRIRenderer::EmitRenderSceneTemporalTrace(uint32_t traceFrameIndex)
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
	Printf("NRI PT light trace: frame=%u analytic=%u topo=%s prop=%s added=%u removed=%u rebound=%u emissive=%u topo=%s prop=%s added=%u removed=%u rebound=%u reset=%s reason=%s\n",
		traceFrameIndex,
		(uint32_t)analyticLights.activeLights.size(),
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.activeSurfaces.size(),
		YesNo(emissiveSurfaces.lastBuildTopologyChanged),
		YesNo(emissiveSurfaces.lastBuildPropertiesChanged),
		(uint32_t)emissiveSurfaces.addedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.removedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.reboundTopologyKeys.size(),
		YesNo(mResetHistory),
		mResetHistory ? mLastHistoryResetReason.c_str() : "none");

	const nri_scene::SkyPerfStats sceneSkyPerf = nri_scene::ConsumeSkyPerfStats();
	Printf("NRI PT sky perf: frame=%u ensure_scene=%u preserve_scene=%u rebuild_scene=%u ensure_sky=%u preserve_hit=%u reuse_active=%u reuse_probe=%u probe=%u/%u face_probes=%u uploads=%u ensure_ms=%.3f probe_ms=%.3f face_ms=%.3f upload_ms=%.3f static_builds=%u overlay_builds=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls,
		gRendererSkyPerfTraceStats.ensureSkyCalls,
		gRendererSkyPerfTraceStats.preserveExistingHits,
		gRendererSkyPerfTraceStats.reuseActiveCubemapHits + gRendererSkyPerfTraceStats.solidReuseHits,
		gRendererSkyPerfTraceStats.reuseActiveProbeHits,
		gRendererSkyPerfTraceStats.probeSuccesses,
		gRendererSkyPerfTraceStats.probeAttempts,
		gRendererSkyPerfTraceStats.probeFaceCalls,
		gRendererSkyPerfTraceStats.buildCubemapUploadCalls,
		(double)gRendererSkyPerfTraceStats.ensureSkyTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeCubemapTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeFaceTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs / 1000.0,
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds,
		gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds);
	Printf("NRI PT sky scene: frame=%u updates=%u wall=%u flat=%u portal=%u inspects=%u cubemap_candidates=%u solid_candidates=%u inspect_faces=%u avg_base=%u avg_recursive=%u recursive_faces=%u avg_pixels=%llu update_ms=%.3f inspect_ms=%.3f avg_ms=%.3f\n",
		traceFrameIndex,
		sceneSkyPerf.updateCalls,
		sceneSkyPerf.wallUpdateCalls,
		sceneSkyPerf.flatUpdateCalls,
		sceneSkyPerf.portalUpdateCalls,
		sceneSkyPerf.inspectCalls,
		sceneSkyPerf.inspectCubemapCandidates,
		sceneSkyPerf.inspectSolidCandidates,
		sceneSkyPerf.inspectFaceWalks,
		sceneSkyPerf.averageColorBaseCalls,
		sceneSkyPerf.averageColorRecursiveCalls,
		sceneSkyPerf.recursiveSkyboxFaceSamples,
		(unsigned long long)sceneSkyPerf.averageColorPixels,
		(double)sceneSkyPerf.updateTimeUs / 1000.0,
		(double)sceneSkyPerf.inspectTimeUs / 1000.0,
		(double)sceneSkyPerf.averageColorTimeUs / 1000.0);
	Printf("NRI PT sky invalidation: frame=%u requests=%u applied=%u emissive_material_dirty=%u keep_last=%u hold_level=%u cached_cubemap=%u create_cubemap=%u cached_solid=%u create_solid=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.lightingInvalidationRequests,
		gRendererSkyPerfTraceStats.lightingInvalidationsApplied,
		gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents,
		gRendererSkyPerfTraceStats.keepLastCubemapHits,
		gRendererSkyPerfTraceStats.holdLevelCubemapHits,
		gRendererSkyPerfTraceStats.activateCachedCubemapHits,
		gRendererSkyPerfTraceStats.createCachedCubemapHits,
		gRendererSkyPerfTraceStats.solidActivateHits,
		gRendererSkyPerfTraceStats.solidCreateHits);
}

bool NRIRenderer::BuildRenderSceneFrame(HWDrawInfo& di, const RenderSceneFrameBuildInputs& inputs, const RenderSceneHistorySnapshot& history, RenderSceneFrameBuildResult& frame)
{
	const uint32_t bootstrapMode = inputs.bootstrapMode;
	const bool bootstrapCapturedView = inputs.bootstrapCapturedView;
	const bool bootstrapCapturedDiagnostics = inputs.bootstrapCapturedDiagnostics;
	const bool bootstrapCapturedFlat = inputs.bootstrapCapturedFlat;
	const bool bootstrapCapturedBaseColor = inputs.bootstrapCapturedBaseColor;
	const bool rawTraceDirectScene = inputs.rawTraceDirectScene;
	const bool preserveHistory = inputs.preserveHistory;
	const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
	const bool allowStaticMapScene = !bootstrapCapturedView && !rawTraceDirectScene && mMapWorld.valid;
	nri_scene::SceneView& capturedSceneView = frame.capturedSceneView;
	nri_scene::SceneView& dynamicSceneView = frame.dynamicSceneView;
	nri_scene::GeometryData& capturedGeometry = frame.capturedGeometry;
	NRIRuntimeMutationFrameOutput& runtimeMutationFrame = frame.runtimeMutationFrame;
	nri_scene::GeometryData& runtimeSpaceLinkGeometry = frame.runtimeSpaceLinkGeometry;
	nri_scene::GeometryData& dynamicGeometry = frame.dynamicGeometry;
	nri_scene::GeometryData& mirrorExtendedDynamicGeometry = frame.mirrorExtendedDynamicGeometry;
	nri_scene::GeometryData& mergedDynamicGeometry = frame.mergedDynamicGeometry;
	nri_scene::GeometryData& debugSphereGeometry = frame.debugSphereGeometry;
	nri_scene::GeometryData& surfaceLightGeometry = frame.surfaceLightGeometry;
	nri_scene::MaterialBridgeData& materialBridge = frame.materialBridge;
	nri_scene::MaterialBridgeData& runtimeSpaceLinkMaterialBridge = frame.runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData& dynamicMaterialBridge = frame.dynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mirrorExtendedDynamicMaterialBridge = frame.mirrorExtendedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mirrorPlayerMaterialBridge = frame.mirrorPlayerMaterialBridge;
	nri_scene::MaterialBridgeData& sceneLightMergedDynamicMaterialBridge = frame.sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& mergedDynamicMaterialBridge = frame.mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData& debugSphereMaterialBridge = frame.debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData& surfaceLightMaterialBridge = frame.surfaceLightMaterialBridge;
	nri_scene::GeometryData& overlayGeometry = mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData& overlayMaterialBridge = mSelectOverlayMaterialBridgeScratch;
	nri_scene::MaterialBridgeData& combinedMaterialBridge = frame.combinedMaterialBridge;
	auto& capturedGpuMaterials = mSelectCapturedGpuMaterialScratch;
	auto& dynamicGpuMaterials = mSelectDynamicGpuMaterialScratch;
	auto& persistentVoxelGpuMaterials = mSelectPersistentVoxelGpuMaterialScratch;
	auto& combinedGpuMaterials = mSelectCombinedGpuMaterialScratch;
	auto& refreshedCombinedGpuMaterials = mSelectRefreshedCombinedGpuMaterialScratch;
	capturedGpuMaterials.clear();
	dynamicGpuMaterials.clear();
	persistentVoxelGpuMaterials.clear();
	combinedGpuMaterials.clear();
	refreshedCombinedGpuMaterials.clear();
	nri_scene::ClearGeometryRetainingCapacity(mSelectMirrorPlayerGeometryScratch);
	nri_scene::ClearGeometryRetainingCapacity(mSelectOverlayGeometryScratch);
	nri_scene::ClearMaterialBridgeRetainingCapacity(mSelectOverlayMaterialBridgeScratch);
	mSelectTopLevelInstanceScratch.clear();
	mSelectSceneInstanceScratch.clear();
	mSelectCapturedTopLevelInstanceScratch.clear();
	mSelectCapturedSceneInstanceScratch.clear();
	const nri_scene::SceneView*& activeSceneView = frame.activeSceneView;
	const nri_scene::GeometryData*& activeGeometry = frame.activeGeometry;
	const std::vector<nri_scene::MaterialData>*& activeGpuMaterials = frame.activeGpuMaterials;
	const nri_scene::MaterialBridgeData*& activeMaterialBridge = frame.activeMaterialBridge;
	const nri_scene::SceneView* sceneLightCapturedView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightCapturedMaterials = nullptr;
	const nri_scene::SceneView* sceneLightDynamicView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightDynamicMaterials = nullptr;
	nri_scene::SceneView& mirrorExtendedDynamicSceneView = frame.mirrorExtendedDynamicSceneView;
	nri_scene::SceneView& mirrorPlayerSceneView = frame.mirrorPlayerSceneView;
	nri_scene::SceneView& sceneLightMergedDynamicSceneView = frame.sceneLightMergedDynamicSceneView;
	nri_scene::SceneView& mergedDynamicSceneView = frame.mergedDynamicSceneView;
	const nri_scene::SceneView*& activeDynamicSceneView = frame.activeDynamicSceneView;
	const nri_scene::GeometryData*& activeDynamicGeometry = frame.activeDynamicGeometry;
	const nri_scene::MaterialBridgeData*& activeDynamicMaterials = frame.activeDynamicMaterials;
	nri_scene::GeometryData& mirrorPlayerGeometry = mSelectMirrorPlayerGeometryScratch;
	MirrorPlayerCaptureStats mirrorPlayerCaptureStats = {};
	nri_scene::GeometryBuildTraceStats mirrorPlayerGeometryTraceStats = {};
	std::vector<SceneBufferUploadDomainSpan> sceneUploadDomainSpans;
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats& activeStats = frame.activeStats;
	bool& paletteReady = frame.paletteReady;
	bool& texturesReady = frame.texturesReady;
	bool& buffersReady = frame.buffersReady;
	bool& accelerationReady = frame.accelerationReady;
	uint32_t combinedOverlayMaterialOffset = 0;
	bool& usingPersistentDynamicEmissiveCache = frame.usingPersistentDynamicEmissiveCache;
	bool liveDynamicHasEmissive = false;
	bool hasPersistentVoxelBatch = false;
	bool appendPersistentVoxelSceneLights = false;
	uint32_t selectedStaticSceneInstanceCount = 0;
	uint32_t selectedDynamicSceneInstanceCount = 0;
	uint32_t selectedPersistentVoxelSceneInstanceCount = 0;
	uint32_t selectedSceneInstanceCount = 0;
	uint32_t selectedTlasInstanceCount = 0;
	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		const bool hasStaticMapScene = allowStaticMapScene && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticMapMs);
			return EnsureStaticMapScene();
		}();
		if (hasStaticMapScene)
		{
			sceneLightUsesStaticMapScene = true;
			emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
			activeStats = mStaticMapScene.sceneView.stats;

			bool residentStaticWorldGeometryChanged = false;
			const bool deferOverlayThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
			const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
				return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
			}();
			mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
			mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
			const bool hasRuntimeMutationOverlay = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
				const bool hasOverlay = mRuntimeMutation.BuildFrameOverlay(
					NRIRuntimeMutationSystem::BuildOverlayServices(*this),
					runtimeMutationFrame);
				residentStaticWorldGeometryChanged = runtimeMutationFrame.residentStaticSceneChanged;
				return hasOverlay;
			}();
			const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
				(void)nri_scene::ConsumeDynamicCapturePerfStats();
				const bool captured = nri_scene::CaptureDynamicScene(di, dynamicSceneView);
				const nri_scene::DynamicCapturePerfStats captureStats = nri_scene::ConsumeDynamicCapturePerfStats();
				mLastPerfShellTraceStats.dynamicCaptureCalls += captureStats.calls;
				mLastPerfShellTraceStats.dynamicCaptureWallSurfaces += captureStats.wallSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureFlatSurfaces += captureStats.flatSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureSpriteSurfaces += captureStats.spriteSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelProxySurfaces += captureStats.voxelProxySurfaces;
				mLastPerfShellTraceStats.dynamicCaptureUnsupportedModelSurfaces += captureStats.unsupportedModelSurfaces;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheStores += captureStats.voxelCacheStores;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheRebuilds += captureStats.voxelCacheRebuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCacheDeferred += captureStats.voxelCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshBuilds += captureStats.voxelMeshCacheBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshDeferred += captureStats.voxelMeshCacheDeferred;
				mLastPerfShellTraceStats.dynamicCaptureVoxelMeshInvalid += captureStats.voxelMeshCacheInvalid;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceBuilds += captureStats.voxelCanonicalSurfaceBuilds;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceHits += captureStats.voxelCanonicalSurfaceHits;
				mLastPerfShellTraceStats.dynamicCaptureVoxelCanonicalSurfaceInvalid += captureStats.voxelCanonicalSurfaceInvalid;
				mLastPerfShellTraceStats.voxelCacheActorEntries = dynamicSceneView.stats.voxelCacheEntries;
				mLastPerfShellTraceStats.voxelCacheActorSurfaces = dynamicSceneView.stats.voxelCacheActorSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueMeshKeys = dynamicSceneView.stats.voxelCacheUniqueMeshKeys;
				mLastPerfShellTraceStats.voxelCacheUniqueMaterialKeys = dynamicSceneView.stats.voxelCacheUniqueMaterialKeys;
				mLastPerfShellTraceStats.voxelCacheLocalSpaceSurfaces = dynamicSceneView.stats.voxelCacheLocalSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheBakedTransformSurfaces = dynamicSceneView.stats.voxelCacheBakedTransformSurfaces;
				mLastPerfShellTraceStats.voxelCacheUnknownSpaceSurfaces = dynamicSceneView.stats.voxelCacheUnknownSpaceSurfaces;
				mLastPerfShellTraceStats.voxelCacheTransformKeyedSurfaces = dynamicSceneView.stats.voxelCacheTransformKeyedSurfaces;
				mLastPerfShellTraceStats.voxelCacheUniqueTransformBases = dynamicSceneView.stats.voxelCacheUniqueTransformBases;
				mLastPerfShellTraceStats.voxelCacheInvariantWarnings = dynamicSceneView.stats.voxelCacheInvariantWarnings;
				mLastPerfShellTraceStats.voxelCacheActorPrimitives = dynamicSceneView.stats.voxelCachePrimitives;
				mLastPerfShellTraceStats.voxelCacheDuplicatedVertexBytes = dynamicSceneView.stats.voxelCacheDuplicatedVertexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedIndexBytes = dynamicSceneView.stats.voxelCacheDuplicatedIndexBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedPrimitiveBytes = dynamicSceneView.stats.voxelCacheDuplicatedPrimitiveBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicatedTotalBytes = dynamicSceneView.stats.voxelCacheDuplicatedTotalBytes;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopCount = dynamicSceneView.stats.voxelCacheDuplicateTopCount;
				mLastPerfShellTraceStats.voxelCacheDuplicateTopEntries = dynamicSceneView.stats.voxelCacheDuplicateTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelEscapeActorCount = dynamicSceneView.stats.dynamicVoxelEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeEligibleActorCount = dynamicSceneView.stats.dynamicVoxelEscapeEligibleActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeForcedActorCount = dynamicSceneView.stats.dynamicVoxelEscapeForcedActorCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeVertexBytes = dynamicSceneView.stats.dynamicVoxelEscapeVertexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeIndexBytes = dynamicSceneView.stats.dynamicVoxelEscapeIndexBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapePrimitiveBytes = dynamicSceneView.stats.dynamicVoxelEscapePrimitiveBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeMaterialBytes = dynamicSceneView.stats.dynamicVoxelEscapeMaterialBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeActorCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeActorCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelExpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapePrimitiveCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapePrimitiveCount;
				mLastPerfShellTraceStats.dynamicVoxelExpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelExpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTotalBytes = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTotalBytes;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopCount = dynamicSceneView.stats.dynamicVoxelEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopCount = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopCount;
				mLastPerfShellTraceStats.dynamicVoxelUnexpectedEscapeTopEntries = dynamicSceneView.stats.dynamicVoxelUnexpectedEscapeTopEntries;
				mLastPerfShellTraceStats.dynamicCaptureCountMs += captureStats.countMs;
				mLastPerfShellTraceStats.dynamicCaptureWallsMs += captureStats.wallsMs;
				mLastPerfShellTraceStats.dynamicCaptureFlatsMs += captureStats.flatsMs;
				mLastPerfShellTraceStats.dynamicCaptureFacingSpritesMs += captureStats.facingSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSpritesMs += captureStats.modelSpritesMs;
				mLastPerfShellTraceStats.dynamicCaptureModelClassifyMs += captureStats.modelClassifyMs;
				mLastPerfShellTraceStats.dynamicCaptureModelMeshMs += captureStats.modelMeshMs;
				mLastPerfShellTraceStats.dynamicCaptureModelSurfaceMs += captureStats.modelSurfaceMs;
				mLastPerfShellTraceStats.dynamicCaptureModelStoreMs += captureStats.modelStoreMs;
				mLastPerfShellTraceStats.dynamicCaptureVoxelFrameMs += captureStats.voxelFrameMs;
				mLastPerfShellTraceStats.dynamicCaptureStatsMs += captureStats.statsMs;
				return captured;
			}();
			const int32_t preferredMirrorWallIndex =
			mLastSurfaceProbe.valid &&
			mLastSurfaceProbe.hit &&
			(mLastSurfaceProbe.primitiveFlags & nri_scene::MaterialFlag_Mirror) != 0 &&
			mLastSurfaceProbe.provenance.wallIndex >= 0 ?
				mLastSurfaceProbe.provenance.wallIndex :
				-1;
		uint32_t visibleMirrorPortalCandidates = 0;
		int32_t selectedVisibleMirrorWallIndex = -1;
		HWPortal* const visibleMirrorPortal = !deferOverlayThisFrame ? [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorPortalMs);
				return SelectPrimaryMirrorPortal(di, visibleMirrorPortalCandidates, selectedVisibleMirrorWallIndex, preferredMirrorWallIndex);
			}() :
			nullptr;
		mHasVisibleMirrorPortalLastFrame = visibleMirrorPortal != nullptr;
		const bool hasMirrorExtendedDynamicScene = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			return CaptureMirrorExtendedDynamicScene(
				di,
				visibleMirrorPortal,
				selectedVisibleMirrorWallIndex,
				hasDynamicScene ? &dynamicSceneView : nullptr,
				mFrameIndex,
				mirrorExtendedDynamicSceneView);
		}();
		const bool hasMirrorPlayerScene = !deferOverlayThisFrame && IsMirrorPlayerPreviewCaptureEnabled() && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMirrorCaptureMs);
			ScopedPtPerfTimer mirrorPlayerTimer(mLastPerfShellTraceStats.mirrorPlayerCaptureMs);
			return CaptureMirrorPlayerDynamicScene(
				di,
				visibleMirrorPortal,
				selectedVisibleMirrorWallIndex,
				visibleMirrorPortalCandidates,
				mirrorPlayerSceneView,
				&mirrorPlayerCaptureStats);
		}();
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildDynamicLiveMs);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}
			mLastPerfShellTraceStats.geometryBuildDynamicLivePrimitives += (uint32_t)dynamicGeometry.primitives.size();

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(dynamicSceneView, dynamicMaterialBridge, "dynamic_live");
				}
			}

			sceneLightDynamicView = &dynamicSceneView;
			sceneLightDynamicMaterials = &dynamicMaterialBridge;
			activeDynamicSceneView = &dynamicSceneView;
			activeDynamicGeometry = &dynamicGeometry;
			activeDynamicMaterials = &dynamicMaterialBridge;
			liveDynamicHasEmissive = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentDynamicMs);
				return mSceneLights.RebuildPersistentDynamicEmissiveCache(
					dynamicSceneView,
					dynamicMaterialBridge,
					BuildPersistentDynamicEmissiveCacheServices());
			}();
		}
		if (hasMirrorExtendedDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorExtendedMs);
				nri_scene::BuildGeometry(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, mirrorExtendedDynamicGeometry);
			}

			if (!mirrorExtendedDynamicGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				BuildMaterialsWithActorOverrides(mirrorExtendedDynamicSceneView, mirrorExtendedDynamicMaterialBridge, "mirror_extended");
			}

			if (hasDynamicScene)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
				sceneLightMergedDynamicSceneView = dynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_dynamic");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}
		if (hasMirrorPlayerScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMirrorPlayerMs);
				{
					ScopedPtPerfTimer buildTimer(mLastPerfShellTraceStats.mirrorPlayerGeometryBuildMs);
					nri_scene::BuildGeometry(mirrorPlayerSceneView, mirrorPlayerGeometry, &mirrorPlayerGeometryTraceStats, true);
				}
				{
					ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.mirrorPlayerPortalAssignMs);
					AssignGeometryPortalIndices(mMapWorld, mirrorPlayerGeometry);
				}
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildWallMs = mirrorPlayerGeometryTraceStats.wallMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildFlatMs = mirrorPlayerGeometryTraceStats.flatMs;
				mLastPerfShellTraceStats.mirrorPlayerGeometryBuildSpriteMs = mirrorPlayerGeometryTraceStats.spriteMs;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawFacingSprites = mirrorPlayerCaptureStats.rawFacingSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureRawVoxelSprites = mirrorPlayerCaptureStats.rawVoxelSprites;
				mLastPerfShellTraceStats.mirrorPlayerCaptureSurfaces = mirrorPlayerCaptureStats.capturedSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerCaptureMatchingActorSurfaces = mirrorPlayerCaptureStats.capturedMatchingActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureOtherActorSurfaces = mirrorPlayerCaptureStats.capturedOtherActorSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureActorlessSurfaces = mirrorPlayerCaptureStats.capturedActorlessSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerCaptureFilteredSurfaces = mirrorPlayerCaptureStats.filteredSurfaceCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryWallSurfaces = mirrorPlayerGeometryTraceStats.wallSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryFlatSurfaces = mirrorPlayerGeometryTraceStats.flatSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteSurfaces = mirrorPlayerGeometryTraceStats.spriteSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexedSurfaces = mirrorPlayerGeometryTraceStats.indexedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometryTriangleFanSurfaces = mirrorPlayerGeometryTraceStats.triangleFanSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySpriteStripSurfaces = mirrorPlayerGeometryTraceStats.spriteStripSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySkippedSurfaces = mirrorPlayerGeometryTraceStats.skippedSurfaces;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceVertices = mirrorPlayerGeometryTraceStats.sourceVertexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometrySourceIndices = mirrorPlayerGeometryTraceStats.sourceIndexCount;
				mLastPerfShellTraceStats.mirrorPlayerGeometryVertexGrowths = mirrorPlayerGeometryTraceStats.vertexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryIndexGrowths = mirrorPlayerGeometryTraceStats.indexCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryPrimitiveGrowths = mirrorPlayerGeometryTraceStats.primitiveCapacityGrowths;
				mLastPerfShellTraceStats.mirrorPlayerGeometryProvenanceGrowths = mirrorPlayerGeometryTraceStats.provenanceCapacityGrowths;
			}

			if (!mirrorPlayerGeometry.primitives.empty())
			{
				Clocker clock(NriPTMaterialBuild);
				ScopedPtPerfTimer materialTimer(mLastPerfShellTraceStats.mirrorPlayerMaterialBuildMs);
				BuildMaterialsWithActorOverrides(mirrorPlayerSceneView, mirrorPlayerMaterialBridge, "mirror_player");
			}
		}

		hasPersistentVoxelBatch = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentVoxelBatchMs);
			const MemoryTelemetry telemetry = GetMemoryTelemetry();
			mPersistentVoxels.PumpAdmissionQueue(
				"runtime",
				mMapWorld.buildSerial,
				mFrameIndex,
				persistentVoxelSettings,
				telemetry.totalTrackedBytes,
				mFrameBuffer != nullptr ? mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildPersistentVoxelResetServices(),
				BuildPersistentVoxelAdmissionServices());
			return EnsurePersistentVoxelBatch();
		}();

		PersistentDynamicSurfaceStats persistentDynamicStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPersistentEmissiveMs);
			mSceneLights.PrunePersistentDynamicEmissiveCacheToLiveActors(BuildPersistentDynamicEmissiveCacheServices());
			persistentDynamicStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
			mSceneLights.UpdatePersistentDynamicEmissiveHighWaterStats(persistentDynamicStats);
		}
		mLastPerfShellTraceStats.persistentDynamicActorSurfaceCount = persistentDynamicStats.actorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicNonActorSurfaceCount = persistentDynamicStats.nonActorSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicWallSurfaceCount = persistentDynamicStats.wallSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicFlatSurfaceCount = persistentDynamicStats.flatSurfaceCount;
		mLastPerfShellTraceStats.persistentDynamicSpriteSurfaceCount = persistentDynamicStats.spriteSurfaceCount;

		const PersistentDynamicEmissiveCache& persistentDynamicCache = mSceneLights.GetPersistentDynamicEmissiveCache();
		const bool shouldUsePersistentDynamicEmissive = persistentDynamicCache.valid;
		if (shouldUsePersistentDynamicEmissive)
		{
			usingPersistentDynamicEmissiveCache = true;
			if (hasDynamicScene)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMs);
				mergedDynamicSceneView = dynamicSceneView;
				mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(mergedDynamicSceneView);
				RebuildSceneViewStats(mergedDynamicSceneView);

				{
					Clocker clock(NriPTGeometryBuild);
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildMergedDynamicMs);
					nri_scene::BuildGeometry(mergedDynamicSceneView, mergedDynamicGeometry);
					AssignGeometryPortalIndices(mMapWorld, mergedDynamicGeometry);
				}
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(mergedDynamicSceneView, mergedDynamicMaterialBridge, "dynamic_with_persistent_emissive");
				}

				if (!mergedDynamicGeometry.primitives.empty())
				{
					activeDynamicSceneView = &mergedDynamicSceneView;
					activeDynamicGeometry = &mergedDynamicGeometry;
					activeDynamicMaterials = &mergedDynamicMaterialBridge;
				}
			}
			else
			{
				activeDynamicSceneView = &persistentDynamicCache.sceneView;
				activeDynamicGeometry = &persistentDynamicCache.geometry;
				activeDynamicMaterials = &persistentDynamicCache.materialBridge;
			}

			if (hasMirrorExtendedDynamicScene && activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				ScopedPtPerfTimer mergePerfTimer(mLastPerfShellTraceStats.sceneSelectLightMergeMs);
				sceneLightMergedDynamicSceneView = *activeDynamicSceneView;
				sceneLightMergedDynamicSceneView.opaqueWalls.insert(
					sceneLightMergedDynamicSceneView.opaqueWalls.end(),
					mirrorExtendedDynamicSceneView.opaqueWalls.begin(),
					mirrorExtendedDynamicSceneView.opaqueWalls.end());
				sceneLightMergedDynamicSceneView.opaqueFlats.insert(
					sceneLightMergedDynamicSceneView.opaqueFlats.end(),
					mirrorExtendedDynamicSceneView.opaqueFlats.begin(),
					mirrorExtendedDynamicSceneView.opaqueFlats.end());
				sceneLightMergedDynamicSceneView.opaqueSprites.insert(
					sceneLightMergedDynamicSceneView.opaqueSprites.end(),
					mirrorExtendedDynamicSceneView.opaqueSprites.begin(),
					mirrorExtendedDynamicSceneView.opaqueSprites.end());
				RebuildSceneViewStats(sceneLightMergedDynamicSceneView);
				BuildMaterialsWithActorOverrides(sceneLightMergedDynamicSceneView, sceneLightMergedDynamicMaterialBridge, "scene_light_merged_persistent");
				sceneLightDynamicView = &sceneLightMergedDynamicSceneView;
				sceneLightDynamicMaterials = &sceneLightMergedDynamicMaterialBridge;
			}
			else if (activeDynamicSceneView != nullptr && activeDynamicMaterials != nullptr)
			{
				sceneLightDynamicView = activeDynamicSceneView;
				sceneLightDynamicMaterials = activeDynamicMaterials;
			}
			else if (hasMirrorExtendedDynamicScene)
			{
				sceneLightDynamicView = &mirrorExtendedDynamicSceneView;
				sceneLightDynamicMaterials = &mirrorExtendedDynamicMaterialBridge;
			}
		}

		if (hasPersistentVoxelBatch && mPersistentVoxels.HasValidBatch())
		{
			appendPersistentVoxelSceneLights = true;
		}

		const bool hasActiveDynamicOverlay =
			activeDynamicGeometry != nullptr &&
			!activeDynamicGeometry->primitives.empty() &&
			activeDynamicMaterials != nullptr;
		const bool hasPersistentVoxelOverlay =
			hasPersistentVoxelBatch &&
			mPersistentVoxels.HasRenderableOverlay();
		const bool hasMirrorExtendedDynamicOverlay =
			hasMirrorExtendedDynamicScene &&
			!mirrorExtendedDynamicGeometry.primitives.empty() &&
			!mirrorExtendedDynamicMaterialBridge.materials.empty();
		const bool hasMirrorPlayerOverlay =
			hasMirrorPlayerScene &&
			!mirrorPlayerGeometry.primitives.empty() &&
			!mirrorPlayerMaterialBridge.materials.empty();
		const bool hasRuntimeDebugSphereOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMs);
			NRIDebugOverlayBuildTelemetry debugOverlayTelemetry = {};
			const bool built = mDebugOverlays.BuildRuntimeDebugSphereOverlay(
				debugSphereGeometry,
				debugSphereMaterialBridge,
				debugOverlayTelemetry,
				ShouldCollectPtPerfTiming());
			mLastPerfShellTraceStats.runtimeDebugSphereViewMs += debugOverlayTelemetry.runtimeDebugSphereViewMs;
			mLastPerfShellTraceStats.runtimeDebugSphereGeoMs += debugOverlayTelemetry.runtimeDebugSphereGeoMs;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialMs += debugOverlayTelemetry.runtimeDebugSphereMaterialMs;
			mLastPerfShellTraceStats.geometryBuildDebugSphereMs += debugOverlayTelemetry.geometryBuildDebugSphereMs;
			mLastPerfShellTraceStats.runtimeDebugSphereCount = debugOverlayTelemetry.runtimeDebugSphereCount;
			mLastPerfShellTraceStats.runtimeDebugSphereLongitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLongitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSphereLatitudeSegments = debugOverlayTelemetry.runtimeDebugSphereLatitudeSegments;
			mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = debugOverlayTelemetry.runtimeDebugSpherePrimitiveCount;
			mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = debugOverlayTelemetry.runtimeDebugSphereMaterialCount;
			return built;
		}();
		const bool hasSurfaceLightOverlay = !deferOverlayThisFrame &&
			BuildSurfaceLightOverlay(surfaceLightGeometry, surfaceLightMaterialBridge);

		if (hasPersistentVoxelOverlay || hasRuntimeSpaceLinkOverlay || hasRuntimeMutationOverlay || hasActiveDynamicOverlay || hasMirrorExtendedDynamicOverlay || hasMirrorPlayerOverlay || hasRuntimeDebugSphereOverlay || hasSurfaceLightOverlay)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.overlayAssembleMs);

			{
				ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.overlayAppendMs);
				{
					ScopedPtPerfTimer resetTimer(mLastPerfShellTraceStats.overlayAppendResetMs);
					nri_scene::ClearGeometryRetainingCapacity(overlayGeometry);
					nri_scene::ClearMaterialBridgeRetainingCapacity(overlayMaterialBridge);
				}

				auto appendOverlaySource =
					[&](
						const nri_scene::GeometryData* geometry,
						const SceneBufferUploadProducerStamp* producerStamp,
						const nri_scene::MaterialBridgeData& materials,
						double& totalMs,
						double& geometryMs,
						double& materialMs,
						uint32_t& primitiveCount,
						uint32_t& materialCount,
						PerfShellTraceStats::OverlayAppendSourceTraceEntry& sourceTrace,
						SceneBufferUploadDomain uploadDomain)
				{
					NRISceneContribution contribution = {};
					contribution.geometry = geometry;
					contribution.producerStamp = producerStamp;
					contribution.materials = &materials;
					contribution.uploadDomain = uploadDomain;
					NRISceneContributionAppendStats appendStats = {};
					appendStats.totalMs = &totalMs;
					appendStats.geometryMs = &geometryMs;
					appendStats.materialMs = &materialMs;
					appendStats.primitiveCount = &primitiveCount;
					appendStats.materialCount = &materialCount;
					appendStats.sourceTrace = &sourceTrace;
					AppendNRISceneContribution(contribution, appendStats, overlayGeometry, overlayMaterialBridge, sceneUploadDomainSpans);
				};

				{
					ScopedPtPerfTimer sourceAggregateTimer(mLastPerfShellTraceStats.overlayAppendSourcesMs);

					const auto buildProducerStamp =
						[&](const nri_scene::SceneView& sceneView, double& timerMs) -> SceneBufferUploadProducerStamp
					{
						ScopedPtPerfTimer aggregateTimer(mLastPerfShellTraceStats.overlayAppendProducerStampMs);
						ScopedPtPerfTimer sourceTimer(timerMs);
						const SceneViewUploadStampBuildResult built = BuildSceneViewUploadProducerStamp(sceneView, mMapWorld.buildSerial);
						SceneBufferUploadProducerStamp stamp = {};
						stamp.vertexPayloadStamp = built.vertexPayloadStamp;
						stamp.indexPayloadStamp = built.indexPayloadStamp;
						stamp.primitivePayloadStamp = built.primitivePayloadStamp;
						stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
						stamp.materialPayloadStamp = built.materialPayloadStamp;
						return stamp;
					};
					const auto buildMirrorPlayerProducerStamp =
						[&]() -> SceneBufferUploadProducerStamp
					{
						ScopedPtPerfTimer aggregateTimer(mLastPerfShellTraceStats.overlayAppendProducerStampMs);
						ScopedPtPerfTimer sourceTimer(mLastPerfShellTraceStats.overlayAppendMirrorPlayerStampMs);
						const SceneViewUploadStampBuildResult built = BuildMirrorPlayerUploadProducerStamp(
							mirrorPlayerGeometry,
							mirrorPlayerMaterialBridge,
							mFrameIndex,
							mMapWorld.buildSerial);
						SceneBufferUploadProducerStamp stamp = {};
						stamp.vertexPayloadStamp = built.vertexPayloadStamp;
						stamp.indexPayloadStamp = built.indexPayloadStamp;
						stamp.primitivePayloadStamp = built.primitivePayloadStamp;
						stamp.primitiveProvenanceStamp = built.primitiveProvenanceStamp;
						stamp.materialPayloadStamp = built.materialPayloadStamp;
						return stamp;
					};
					const SceneBufferUploadProducerStamp dynamicStamp =
						hasActiveDynamicOverlay && activeDynamicSceneView != nullptr ? buildProducerStamp(*activeDynamicSceneView, mLastPerfShellTraceStats.overlayAppendDynamicStampMs) : SceneBufferUploadProducerStamp {};
					const SceneBufferUploadProducerStamp mirrorExtendedStamp =
						hasMirrorExtendedDynamicOverlay ? buildProducerStamp(mirrorExtendedDynamicSceneView, mLastPerfShellTraceStats.overlayAppendMirrorExtendedStampMs) : SceneBufferUploadProducerStamp {};
					const SceneBufferUploadProducerStamp mirrorPlayerStamp =
						hasMirrorPlayerOverlay ? buildMirrorPlayerProducerStamp() : SceneBufferUploadProducerStamp {};

					NRISceneContributionReserve overlayReserve = {};
					auto addOverlayReserve =
						[&](const nri_scene::GeometryData* geometry, const nri_scene::MaterialBridgeData& materials)
					{
						NRISceneContribution contribution = {};
						contribution.geometry = geometry;
						contribution.materials = &materials;
						AccumulateNRISceneContributionReserve(contribution, overlayReserve);
					};

					if (hasRuntimeSpaceLinkOverlay)
					{
						addOverlayReserve(&runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
					}
					if (hasRuntimeMutationOverlay)
					{
						addOverlayReserve(&runtimeMutationFrame.geometry, runtimeMutationFrame.materialBridge);
					}
					if (hasActiveDynamicOverlay)
					{
						addOverlayReserve(activeDynamicGeometry, *activeDynamicMaterials);
					}
					if (hasMirrorExtendedDynamicOverlay)
					{
						addOverlayReserve(&mirrorExtendedDynamicGeometry, mirrorExtendedDynamicMaterialBridge);
					}
					if (hasMirrorPlayerOverlay)
					{
						addOverlayReserve(&mirrorPlayerGeometry, mirrorPlayerMaterialBridge);
					}
					if (hasRuntimeDebugSphereOverlay)
					{
						addOverlayReserve(&debugSphereGeometry, debugSphereMaterialBridge);
					}
					if (hasSurfaceLightOverlay)
					{
						addOverlayReserve(&surfaceLightGeometry, surfaceLightMaterialBridge);
					}
					ReserveNRISceneContributionCapacity(overlayReserve, overlayGeometry, overlayMaterialBridge);

					if (hasRuntimeSpaceLinkOverlay)
					{
						appendOverlaySource(
							&runtimeSpaceLinkGeometry,
							nullptr,
							runtimeSpaceLinkMaterialBridge,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkGeometryMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialMs,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkPrimitiveCount,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkMaterialCount,
							mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}

					if (hasRuntimeMutationOverlay)
					{
						appendOverlaySource(
							&runtimeMutationFrame.geometry,
							nullptr,
							runtimeMutationFrame.materialBridge,
							mLastPerfShellTraceStats.overlayRuntimeMutationMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationGeometryMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationMaterialMs,
							mLastPerfShellTraceStats.overlayRuntimeMutationPrimitiveCount,
							mLastPerfShellTraceStats.overlayRuntimeMutationMaterialCount,
							mLastPerfShellTraceStats.overlayRuntimeMutationAppend,
							SceneBufferUploadDomain::RuntimeMutation);
					}

					if (hasActiveDynamicOverlay)
					{
						appendOverlaySource(
							activeDynamicGeometry,
							&dynamicStamp,
							*activeDynamicMaterials,
							mLastPerfShellTraceStats.overlayDynamicMs,
							mLastPerfShellTraceStats.overlayDynamicGeometryMs,
							mLastPerfShellTraceStats.overlayDynamicMaterialMs,
							mLastPerfShellTraceStats.overlayDynamicPrimitiveCount,
							mLastPerfShellTraceStats.overlayDynamicMaterialCount,
							mLastPerfShellTraceStats.overlayDynamicAppend,
							SceneBufferUploadDomain::Dynamic);
					}

					if (hasMirrorExtendedDynamicOverlay)
					{
						appendOverlaySource(
							&mirrorExtendedDynamicGeometry,
							&mirrorExtendedStamp,
							mirrorExtendedDynamicMaterialBridge,
							mLastPerfShellTraceStats.overlayMirrorExtendedMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedGeometryMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedMaterialMs,
							mLastPerfShellTraceStats.overlayMirrorExtendedPrimitiveCount,
							mLastPerfShellTraceStats.overlayMirrorExtendedMaterialCount,
							mLastPerfShellTraceStats.overlayMirrorExtendedAppend,
							SceneBufferUploadDomain::MirrorExtended);
					}

					if (hasMirrorPlayerOverlay)
					{
						appendOverlaySource(
							&mirrorPlayerGeometry,
							&mirrorPlayerStamp,
							mirrorPlayerMaterialBridge,
							mLastPerfShellTraceStats.overlayMirrorPlayerMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerGeometryMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerMaterialMs,
							mLastPerfShellTraceStats.overlayMirrorPlayerPrimitiveCount,
							mLastPerfShellTraceStats.overlayMirrorPlayerMaterialCount,
							mLastPerfShellTraceStats.overlayMirrorPlayerAppend,
							SceneBufferUploadDomain::MirrorPlayer);
					}

					if (hasRuntimeDebugSphereOverlay)
					{
						appendOverlaySource(
							&debugSphereGeometry,
							nullptr,
							debugSphereMaterialBridge,
							mLastPerfShellTraceStats.overlayDebugSphereMs,
							mLastPerfShellTraceStats.overlayDebugSphereGeometryMs,
							mLastPerfShellTraceStats.overlayDebugSphereMaterialMs,
							mLastPerfShellTraceStats.overlayDebugSpherePrimitiveCount,
							mLastPerfShellTraceStats.overlayDebugSphereMaterialCount,
							mLastPerfShellTraceStats.overlayDebugSphereAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}

					if (hasSurfaceLightOverlay)
					{
						double surfaceLightOverlayMs = 0.0;
						double surfaceLightGeometryMs = 0.0;
						double surfaceLightMaterialMs = 0.0;
						uint32_t surfaceLightPrimitiveCount = 0;
						uint32_t surfaceLightMaterialCount = 0;
						PerfShellTraceStats::OverlayAppendSourceTraceEntry surfaceLightAppend = {};
						appendOverlaySource(
							&surfaceLightGeometry,
							nullptr,
							surfaceLightMaterialBridge,
							surfaceLightOverlayMs,
							surfaceLightGeometryMs,
							surfaceLightMaterialMs,
							surfaceLightPrimitiveCount,
							surfaceLightMaterialCount,
							surfaceLightAppend,
							SceneBufferUploadDomain::StaticOverlay);
					}
				}

				{
					ScopedPtPerfTimer bookkeepingTimer(mLastPerfShellTraceStats.overlayAppendBookkeepingMs);
					if (hasPersistentVoxelOverlay)
					{
						const NRIPersistentVoxelOverlayStats persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayStats();
						mLastPerfShellTraceStats.overlayPersistentVoxelActorCount = persistentVoxelOverlayStats.actorCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelPrimitiveCount = persistentVoxelOverlayStats.primitiveCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelMaterialCount = persistentVoxelOverlayStats.materialCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.primitiveCount = persistentVoxelOverlayStats.primitiveCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.materialCount = persistentVoxelOverlayStats.materialCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.indexCount = persistentVoxelOverlayStats.indexCount;
						mLastPerfShellTraceStats.overlayPersistentVoxelAppend.byteCount = persistentVoxelOverlayStats.byteCount;
					}
					mLastPerfShellTraceStats.overlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
					mLastPerfShellTraceStats.overlayMaterialCount = (uint32_t)overlayMaterialBridge.materials.size();
				}
			}

			auto& instances = mSelectTopLevelInstanceScratch;
			auto& sceneInstances = mSelectSceneInstanceScratch;
			instances.clear();
			sceneInstances.clear();
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStaticInstancesMs);
				BuildStaticMapInstances(instances, sceneInstances);
			}
			const uint32_t staticSceneInstanceBaselineCount = (uint32_t)sceneInstances.size();
			selectedStaticSceneInstanceCount = staticSceneInstanceBaselineCount;
			selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
			selectedTlasInstanceCount = (uint32_t)instances.size();
			bool selectedSceneHasDynamicOverlay = false;

			if (overlayGeometry.primitives.empty() && !hasPersistentVoxelOverlay)
			{
				accelerationReady =
					BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
					NRISceneUploadManager::UpdateSceneDataSet(*this,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						sceneInstances,
						(uint32_t)mStaticMapScene.geometry.primitives.size(),
						0u,
						(uint32_t)mStaticMapScene.gpuMaterials.size(),
						0u,
						"static_only_scene");
				if (accelerationReady && hasRuntimeMutationOverlay)
				{
					mBuiltStaticMapSceneASLastFrame = false;
				}
			}
			else
			{
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialBridgeMs);
					combinedMaterialBridge = mStaticMapScene.materialBridge;
					combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					if (hasPersistentVoxelOverlay)
					{
						mPersistentVoxels.AppendMaterialBridgeTo(combinedMaterialBridge);
						combinedOverlayMaterialOffset = (uint32_t)combinedMaterialBridge.materials.size();
					}
					nri_scene::AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				}
				paletteReady = [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectPaletteMs);
					return EnsurePaletteTexture(combinedMaterialBridge);
				}();
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
				texturesReady = paletteReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturesMs);
					return EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false, "static_map_overlay_combined");
				}();
				dynamicGpuMaterials.clear();
				persistentVoxelGpuMaterials.clear();
				if (texturesReady)
				{
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectMaterialSplitMs);
						const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
						const size_t persistentVoxelMaterialCount = hasPersistentVoxelOverlay ? mPersistentVoxels.OverlayMaterialCount() : 0u;
						if (combinedGpuMaterials.size() < staticMaterialCount + persistentVoxelMaterialCount)
						{
							texturesReady = false;
						}
						else
						{
							persistentVoxelGpuMaterials.assign(
								combinedGpuMaterials.begin() + staticMaterialCount,
								combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount);
							dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount + persistentVoxelMaterialCount, combinedGpuMaterials.end());
						}
					}
				}
				buffersReady = texturesReady && [&]()
				{
					ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadMs);
					return UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials, &sceneUploadDomainSpans) &&
						(!hasPersistentVoxelOverlay || UploadPersistentVoxelArenaMaterialBuffers(persistentVoxelGpuMaterials));
				}();
				accelerationReady = false;
				const uint32_t liveOverlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
				const uint32_t liveOverlayIndexOffset = 0u;
				const uint32_t liveOverlayIndexCount = (uint32_t)overlayGeometry.indices.size();
				NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
				if (buffersReady)
				{
					bool persistentVoxelAsReady = true;
					bool dynamicAsReady = true;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelAsMs);
						NRIPersistentVoxelAccelerationBuildStats persistentVoxelAsStats = {};
						persistentVoxelAsReady = mPersistentVoxels.BuildAccelerationStructures(
							mFrameIndex,
							(bool)nri_voxelstats,
							BuildPersistentVoxelResetServices(),
							BuildPersistentVoxelAccelerationServices(),
							persistentVoxelAsStats);
						mLastPerfShellTraceStats.persistentVoxelAsCalls += persistentVoxelAsStats.calls;
						mLastPerfShellTraceStats.persistentVoxelAsBuilds += persistentVoxelAsStats.builds;
						mLastPerfShellTraceStats.persistentVoxelAsUniqueMeshBuilds += persistentVoxelAsStats.uniqueMeshBuilds;
						mLastPerfShellTraceStats.persistentVoxelAsInstances += persistentVoxelAsStats.instances;
					}
					if (liveOverlayPrimitiveCount > 0)
					{
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkPrimitives = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationPrimitives = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDynamicPrimitives = mLastPerfShellTraceStats.overlayDynamicAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedPrimitives = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerPrimitives = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsDebugSpherePrimitives = mLastPerfShellTraceStats.overlayDebugSphereAppend.primitiveCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeSpaceLinkBytes = mLastPerfShellTraceStats.overlayRuntimeSpaceLinkAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsRuntimeMutationBytes = mLastPerfShellTraceStats.overlayRuntimeMutationAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDynamicBytes = mLastPerfShellTraceStats.overlayDynamicAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorExtendedBytes = mLastPerfShellTraceStats.overlayMirrorExtendedAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsMirrorPlayerBytes = mLastPerfShellTraceStats.overlayMirrorPlayerAppend.byteCount;
						mLastPerfShellTraceStats.dynamicAsDebugSphereBytes = mLastPerfShellTraceStats.overlayDebugSphereAppend.byteCount;
						dynamicAsReady =
							BuildDynamicAccelerationStructure(
								overlayGeometry,
								liveOverlayIndexOffset,
								liveOverlayIndexCount,
								liveOverlayPrimitiveCount,
								dynamicBottomLevelAS,
								true) &&
							dynamicBottomLevelAS.accelerationStructure != nullptr;
					}
					else
					{
						mLastPerfShellTraceStats.dynamicAsPrimitiveCount = 0;
						mLastPerfShellTraceStats.dynamicAsVertexCount = 0;
						mLastPerfShellTraceStats.dynamicAsIndexCount = 0;
					}
					accelerationReady = persistentVoxelAsReady && dynamicAsReady;
				}
				emissiveSamplingContext.runtimeMutationGeometry = hasRuntimeMutationOverlay ? &runtimeMutationFrame.geometry : nullptr;
				emissiveSamplingContext.runtimeMutationPrimitiveBaseOffset = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationFrame.geometry.primitives.size());
				if (accelerationReady)
				{
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						ScopedPtPerfTimer persistentVoxelTlasTimer(mLastPerfShellTraceStats.persistentVoxelTlasInstanceMs);
						NRIPersistentVoxelTlasServices persistentVoxelTlasServices = {};
						persistentVoxelTlasServices.user = this;
						persistentVoxelTlasServices.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& resource) -> uint64_t
						{
							NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
							return resource.accelerationStructure != nullptr ?
								renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*resource.accelerationStructure) :
								0ull;
						};
						NRIPersistentVoxelTlasBuildStats persistentVoxelTlasStats = {};
						if (!mPersistentVoxels.AppendTlasInstances(
							instances,
							sceneInstances,
							mFrameIndex,
							persistentVoxelSettings,
							(bool)nri_voxelstats,
							persistentVoxelTlasServices,
							persistentVoxelTlasStats))
						{
							accelerationReady = false;
						}
						mLastPerfShellTraceStats.persistentVoxelSharedMeshResources = persistentVoxelTlasStats.sharedMeshResourceCount;
						mLastPerfShellTraceStats.persistentVoxelTlasInstances += persistentVoxelTlasStats.instanceCount;
						mLastPerfShellTraceStats.persistentVoxelBakedFallbackInstances += persistentVoxelTlasStats.bakedFallbackInstanceCount;
					}

					if (liveOverlayPrimitiveCount > 0 && dynamicBottomLevelAS.accelerationStructure != nullptr)
					{
						ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectInstanceHandlesMs);
						nri::TopLevelInstance dynamicInstance = {};
						dynamicInstance.transform[0][0] = 1.0f;
						dynamicInstance.transform[1][1] = 1.0f;
						dynamicInstance.transform[2][2] = 1.0f;
						dynamicInstance.instanceId = (uint32_t)sceneInstances.size();
						dynamicInstance.mask = 0xFF;
						dynamicInstance.shaderBindingTableLocalOffset = 0;
						dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
						dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);
						instances.push_back(dynamicInstance);
						sceneInstances.push_back({ 0u, nri_diag::SceneDataSourceDynamic, 0u, UINT32_MAX });
					}

					selectedStaticSceneInstanceCount = 0;
					selectedDynamicSceneInstanceCount = 0;
					selectedPersistentVoxelSceneInstanceCount = 0;
					for (const SceneInstanceData& sceneInstance : sceneInstances)
					{
						if (sceneInstance.dataSource == nri_diag::SceneDataSourceStatic)
						{
							selectedStaticSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourceDynamic)
						{
							selectedDynamicSceneInstanceCount++;
						}
						else if (sceneInstance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
						{
							selectedPersistentVoxelSceneInstanceCount++;
						}
					}
					selectedSceneInstanceCount = (uint32_t)sceneInstances.size();
					selectedTlasInstanceCount = (uint32_t)instances.size();
					const bool hasEffectiveOverlayInstances = sceneInstances.size() > staticSceneInstanceBaselineCount;
					selectedSceneHasDynamicOverlay =
						liveOverlayPrimitiveCount > 0 ||
						selectedDynamicSceneInstanceCount > 0 ||
						selectedPersistentVoxelSceneInstanceCount > 0 ||
						hasEffectiveOverlayInstances;
					if (selectedSceneHasDynamicOverlay)
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								GetCurrentDynamicVertexBuffer(),
								GetCurrentDynamicIndexBuffer(),
								GetCurrentDynamicPrimitiveBuffer(),
								GetCurrentDynamicMaterialBuffer(),
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								(uint32_t)overlayGeometry.primitives.size(),
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								(uint32_t)dynamicGpuMaterials.size(),
								"static_plus_overlay_scene");
					}
					else
					{
						accelerationReady =
							BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
							NRISceneUploadManager::UpdateSceneDataSet(*this,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								mStaticVertexBuffer,
								mStaticIndexBuffer,
								mStaticPrimitiveBuffer,
								mStaticMaterialBuffer,
								sceneInstances,
								(uint32_t)mStaticMapScene.geometry.primitives.size(),
								0u,
								(uint32_t)mStaticMapScene.gpuMaterials.size(),
								0u,
								"static_only_effective_scene");
					}
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectTexturePrepMs);
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectStateCommitMs);
				{
					ScopedPtPerfTimer stateFlagsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitFlagsMs);
					mUsedDynamicSceneLastFrame = selectedSceneHasDynamicOverlay;
					mGpuSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					mLastPerfShellTraceStats.sceneSelectStateCommitSelectedDynamic = selectedSceneHasDynamicOverlay ? 1u : 0u;
				}
				{
					NRISceneFrameDynamicStateInputs dynamicStateInputs = {};
					dynamicStateInputs.activeDynamicSceneView = activeDynamicSceneView;
					dynamicStateInputs.activeDynamicGeometry = activeDynamicGeometry;
					dynamicStateInputs.activeDynamicMaterials = activeDynamicMaterials;
					dynamicStateInputs.mirrorExtendedSceneView = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicSceneView : nullptr;
					dynamicStateInputs.mirrorExtendedGeometry = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicGeometry : nullptr;
					dynamicStateInputs.mirrorExtendedMaterials = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicMaterialBridge : nullptr;
					dynamicStateInputs.mirrorPlayerSceneView = hasMirrorPlayerScene ? &mirrorPlayerSceneView : nullptr;
					dynamicStateInputs.mirrorPlayerGeometry = hasMirrorPlayerScene ? &mirrorPlayerGeometry : nullptr;
					dynamicStateInputs.mirrorPlayerMaterials = hasMirrorPlayerScene ? &mirrorPlayerMaterialBridge : nullptr;
					dynamicStateInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicStateMs;
					dynamicStateInputs.dynamicCoreMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicCoreMs;
					dynamicStateInputs.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorExtendedMs;
					dynamicStateInputs.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitDynamicMirrorPlayerMs;
					mDynamicSceneLastFrame = BuildNRISceneFrameDynamicState(dynamicStateInputs, mDynamicSceneLastFrame, mLastPerfShellTraceStats);
				}
				{
					NRISceneFrameGeometrySelectionInputs geometrySelectionInputs = {};
					geometrySelectionInputs.staticBuildSerial = mStaticMapScene.buildSerial;
					geometrySelectionInputs.staticGeometry = &mStaticMapScene.geometry;
					geometrySelectionInputs.staticMaterialBridge = &mStaticMapScene.materialBridge;
					geometrySelectionInputs.staticGpuMaterials = &mStaticMapScene.gpuMaterials;
					geometrySelectionInputs.overlayGeometry = &overlayGeometry;
					geometrySelectionInputs.overlayMaterialOffset = combinedOverlayMaterialOffset;
					geometrySelectionInputs.combinedMaterialBridge = &combinedMaterialBridge;
					geometrySelectionInputs.combinedGpuMaterials = &combinedGpuMaterials;
					geometrySelectionInputs.totalMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStateMs;
					geometrySelectionInputs.staticCopyMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticCopyMs;
					geometrySelectionInputs.overlayAppendMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometryAppendMs;
					geometrySelectionInputs.selectMs = &mLastPerfShellTraceStats.sceneSelectStateCommitGeometrySelectMs;
					const NRISceneFrameGeometrySelection geometrySelection = mSceneFrameGeometry.SelectActiveGeometry(geometrySelectionInputs);
					if (geometrySelection.usedCombinedGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryCombined = 1;
					}
					if (geometrySelection.usedStaticOnlyGeometry)
					{
						mLastPerfShellTraceStats.sceneSelectStateCommitGeometryStaticOnly = 1;
					}
					activeStaticProbePrimitiveCount = geometrySelection.staticProbePrimitiveCount;
					activeGeometry = geometrySelection.geometry;
					activeGpuMaterials = geometrySelection.gpuMaterials;
					activeMaterialBridge = geometrySelection.materialBridge;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedPrimitiveCount = geometrySelection.combinedPrimitiveCount;
					mLastPerfShellTraceStats.sceneSelectStateCommitCombinedMaterialCount = geometrySelection.combinedMaterialCount;
				}

				{
					ScopedPtPerfTimer statsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsMs);
					nri_scene::SceneDebugStats persistentVoxelOverlayStats;
					if (hasPersistentVoxelOverlay)
					{
						ScopedPtPerfTimer persistentVoxelStatsTimer(mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs);
						persistentVoxelOverlayStats = mPersistentVoxels.BuildOverlayDebugStats();
					}
					NRISceneFrameDebugStatsInputs debugStatsInputs = {};
					debugStatsInputs.staticMapStats = &mStaticMapScene.sceneView.stats;
					debugStatsInputs.deferredDynamicSceneView = !deferOverlayThisFrame ? &dynamicSceneView : nullptr;
					debugStatsInputs.activeDynamicSceneView = activeDynamicSceneView;
					debugStatsInputs.persistentVoxelStats = hasPersistentVoxelOverlay ? &persistentVoxelOverlayStats : nullptr;
					debugStatsInputs.mirrorExtendedSceneView = hasMirrorExtendedDynamicScene ? &mirrorExtendedDynamicSceneView : nullptr;
					debugStatsInputs.mirrorPlayerSceneView = hasMirrorPlayerScene ? &mirrorPlayerSceneView : nullptr;
					debugStatsInputs.baseMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsBaseMs;
					debugStatsInputs.persistentVoxelMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsPersistentVoxelMs;
					debugStatsInputs.mirrorExtendedMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorExtendedMs;
					debugStatsInputs.mirrorPlayerMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMirrorPlayerMs;
					debugStatsInputs.mergeMs = &mLastPerfShellTraceStats.sceneSelectStateCommitStatsMergeMs;
					activeStats = BuildNRISceneFrameDebugStats(debugStatsInputs, mLastPerfShellTraceStats);
				}

				{
					NRISceneFrameGenerationInputs generationInputs = {};
					generationInputs.staticMapBuildSerial = mStaticMapScene.buildSerial;
					generationInputs.runtimeMutationGeneration = mRuntimeMutation.BuildFrameGenerationHash(hasRuntimeMutationOverlay);
					generationInputs.persistentVoxelGeneration = hasPersistentVoxelOverlay ? mPersistentVoxels.BuildSceneGenerationHash() : 0ull;
					generationInputs.frameIndex = mFrameIndex;
					generationInputs.staticAccelerationBuildSerial = mStaticAccelerationBuildSerial;
					generationInputs.renderWidth = mRenderWidth;
					generationInputs.renderHeight = mRenderHeight;
					generationInputs.currentCameraPos = mCurrentCameraPos;
					generationInputs.currentCameraForward = mCurrentCameraForward;
					generationInputs.currentCameraRight = mCurrentCameraRight;
					generationInputs.currentCameraUp = mCurrentCameraUp;
					generationInputs.currentTanHalfFovX = mCurrentTanHalfFovX;
					generationInputs.currentTanHalfFovY = mCurrentTanHalfFovY;
					generationInputs.selectedSceneHasDynamicOverlay = selectedSceneHasDynamicOverlay;
					generationInputs.activeDynamicSceneView = activeDynamicSceneView;
					generationInputs.activeDynamicGeometry = activeDynamicGeometry;
					generationInputs.activeDynamicMaterials = activeDynamicMaterials;
					generationInputs.hasMirrorPlayerScene = hasMirrorPlayerScene;
					generationInputs.mirrorPlayerGeometry = &mirrorPlayerGeometry;
					generationInputs.mirrorPlayerMaterials = &mirrorPlayerMaterialBridge;
					generationInputs.activeMaterialBridge = activeMaterialBridge;
					generationInputs.activeGpuMaterials = activeGpuMaterials;
					generationInputs.sceneTextureCacheCount = mSceneTextures.CacheCount();
					generationInputs.selectedTlasInstanceCount = selectedTlasInstanceCount;
					generationInputs.selectedSceneInstanceCount = selectedSceneInstanceCount;
					generationInputs.selectedStaticSceneInstanceCount = selectedStaticSceneInstanceCount;
					generationInputs.selectedDynamicSceneInstanceCount = selectedDynamicSceneInstanceCount;
					generationInputs.selectedPersistentVoxelSceneInstanceCount = selectedPersistentVoxelSceneInstanceCount;
					const NRISceneFrameGenerationResult generationResult =
						BuildNRISceneFrameGenerationResult(generationInputs, mLastStateCommitDomainGenerations, mHasLastStateCommitDomainGenerations);
					WriteNRISceneFrameGenerationTraceStats(generationResult, mLastPerfShellTraceStats);
					mLastStateCommitDomainGenerations = generationResult.current;
					mHasLastStateCommitDomainGenerations = true;
				}
			}
			else
			{
				LogFallback("PT runtime/dynamic overlay update failed; tracing the resident static world only.");
				if (mGpuSceneHasDynamicOverlay)
				{
					RestoreStaticTopLevelScene();
				}
				paletteReady = true;
				texturesReady = true;
				buffersReady = true;
				accelerationReady = true;
			}
		}
		else if (mGpuSceneHasDynamicOverlay || residentStaticWorldGeometryChanged)
		{
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay or resident chunk rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			mGpuSceneHasDynamicOverlay = false;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping non-map dynamic overlay on the same frame that rebuilt resident static map assets.\n");
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
		ResetPersistentDynamicEmissiveCache();
		Clocker clock(NriPTSceneCapture);
		if (!nri_scene::CaptureScene(di, capturedSceneView))
		{
			LogFallback("PT scene capture failed.");
			if (preserveHistory)
			{
				RestoreRenderSceneHistorySnapshot(history);
			}
			return false;
		}

		activeSceneView = &capturedSceneView;
		activeMaterialBridge = &materialBridge;
		sceneLightCapturedView = &capturedSceneView;
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildCapturedMs);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
			AssignGeometryPortalIndices(mMapWorld, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(capturedSceneView, materialBridge, "captured_scene");
		}
		sceneLightCapturedMaterials = &materialBridge;

		const bool needsFallbackMaterials = bootstrapCapturedDiagnostics || bootstrapCapturedFlat;
		const bool needsRealTextures = !nri_ptbootstrap || bootstrapCapturedBaseColor || bootstrapMode >= 13u;
		paletteReady = needsRealTextures ? EnsurePaletteTexture(materialBridge) : true;
		texturesReady = needsFallbackMaterials ? UseFallbackSceneTextures(preserveHistory, "captured_scene_fallback") : (needsRealTextures ? (paletteReady && EnsureSceneTextures(capturedSceneView, materialBridge, capturedGpuMaterials, preserveHistory, "captured_scene")) : EnsureSkyTexture(capturedSceneView, preserveHistory));
		if (needsFallbackMaterials)
		{
			capturedGpuMaterials = materialBridge.materials;
			for (auto& material : capturedGpuMaterials)
			{
				material.textureIndex = 0;
				material.paletteIndex = 0;
				material.flags = 0;
				material.normalTextureIndex = UINT32_MAX;
				material.metallicTextureIndex = UINT32_MAX;
				material.roughnessTextureIndex = UINT32_MAX;
				material.emissiveTextureIndex = UINT32_MAX;
				material.lightLevel = 1.0f;
				material.alpha = 1.0f;
			}
		}
		else if (!needsRealTextures)
		{
			capturedGpuMaterials = materialBridge.materials;
		}

		buffersReady = texturesReady && UploadSceneBuffers(capturedGeometry, capturedGpuMaterials);
		auto& sceneInstances = mSelectCapturedSceneInstanceScratch;
		sceneInstances.clear();
		if (buffersReady)
		{
			sceneInstances.push_back({ 0u, nri_diag::SceneDataSourceDynamic, 0u, UINT32_MAX });
			buffersReady = NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				sceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene");
		}
		if (texturesReady)
		{
			PrepareSceneTextureInputsForCompute();
		}
		if (bootstrapCapturedView || rawTraceDirectScene)
		{
			accelerationReady = true;
		}
		else if (buffersReady)
		{
			NRIAccelerationStructureResource& dynamicBottomLevelAS = GetCurrentDynamicBottomLevelAS();
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				dynamicBottomLevelAS.accelerationStructure != nullptr;
			if (accelerationReady)
			{
				nri::TopLevelInstance instance = {};
				instance.transform[0][0] = 1.0f;
				instance.transform[1][1] = 1.0f;
				instance.transform[2][2] = 1.0f;
				instance.instanceId = 0;
				instance.mask = 0xFF;
				instance.shaderBindingTableLocalOffset = 0;
				instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS.accelerationStructure);

				auto& instances = mSelectCapturedTopLevelInstanceScratch;
				instances.clear();
				instances.push_back(instance);
				accelerationReady = BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Dynamic);
			}
		}
		else
		{
			accelerationReady = false;
		}
		activeGeometry = &capturedGeometry;
		activeGpuMaterials = &capturedGpuMaterials;
		emissiveSamplingContext.capturedGeometry = &capturedGeometry;
		}
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr || activeMaterialBridge == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	RefreshSceneLightSystem(
		sceneLightUsesStaticMapScene,
		sceneLightCapturedView,
		sceneLightCapturedMaterials,
		sceneLightDynamicView,
		sceneLightDynamicMaterials,
		appendPersistentVoxelSceneLights);

	bool refreshedSceneDataAfterLightRebuild = false;
	if (mGpuSceneHasDynamicOverlay &&
		activeMaterialBridge == &combinedMaterialBridge &&
		!overlayGeometry.primitives.empty())
	{
		refreshedCombinedGpuMaterials = combinedMaterialBridge.materials;
		ApplyEmissiveMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		ApplyActorShadowMaterialOverrides(combinedMaterialBridge, refreshedCombinedGpuMaterials);
		if (!MaterialDataVectorEqual(refreshedCombinedGpuMaterials, combinedGpuMaterials))
		{
			const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
			if (refreshedCombinedGpuMaterials.size() < staticMaterialCount)
			{
				LogFallback("PT runtime overlay material refresh produced an invalid material slice.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			combinedGpuMaterials.swap(refreshedCombinedGpuMaterials);
			dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
			if (!UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials) ||
				!NRISceneUploadManager::UpdateSceneDataSet(*this,
					mStaticVertexBuffer,
					mStaticIndexBuffer,
					mStaticPrimitiveBuffer,
					mStaticMaterialBuffer,
					GetCurrentDynamicVertexBuffer(),
					GetCurrentDynamicIndexBuffer(),
					GetCurrentDynamicPrimitiveBuffer(),
					GetCurrentDynamicMaterialBuffer(),
					mBoundSceneInstances,
					(uint32_t)mStaticMapScene.geometry.primitives.size(),
					(uint32_t)overlayGeometry.primitives.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size(),
					(uint32_t)dynamicGpuMaterials.size(),
					"resident_overlay_material_refresh"))
			{
				LogFallback("PT runtime overlay material refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}

			activeGpuMaterials = &combinedGpuMaterials;
			refreshedSceneDataAfterLightRebuild = true;
		}
	}

	if (mRuntimeLightSceneDataDirty && !refreshedSceneDataAfterLightRebuild)
	{
		if (mGpuSceneHasDynamicOverlay)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				mStaticVertexBuffer,
				mStaticIndexBuffer,
				mStaticPrimitiveBuffer,
				mStaticMaterialBuffer,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				(uint32_t)mStaticMapScene.geometry.primitives.size(),
				(uint32_t)overlayGeometry.primitives.size(),
				(uint32_t)mStaticMapScene.gpuMaterials.size(),
				(uint32_t)dynamicGpuMaterials.size(),
				"runtime_overlay_light_refresh"))
			{
				LogFallback("PT runtime overlay light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
		else if (!sceneLightUsesStaticMapScene)
		{
			if (!NRISceneUploadManager::UpdateSceneDataSet(*this,
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				GetCurrentDynamicVertexBuffer(),
				GetCurrentDynamicIndexBuffer(),
				GetCurrentDynamicPrimitiveBuffer(),
				GetCurrentDynamicMaterialBuffer(),
				mBoundSceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size(),
				"captured_scene_light_refresh"))
			{
				LogFallback("PT captured scene light refresh failed after scene-light rebuild.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			mRuntimeLightSceneDataDirty ||
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh)
		{
			if (!RefreshResidentStaticSceneDataSet())
			{
				LogFallback("PT static scene light refresh failed.");
				if (preserveHistory)
				{
					RestoreRenderSceneHistorySnapshot(history);
				}
				return false;
			}
		}
	}

	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT emissive primitive update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT emissive TLAS update failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	TraceRuntimeLinkEvents(di);
	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);
	NRISceneSurfaceProbeFrameInputs surfaceProbeFrameInputs = {};
	surfaceProbeFrameInputs.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	surfaceProbeFrameInputs.activeStaticProbePrimitiveCount = activeStaticProbePrimitiveCount;
	surfaceProbeFrameInputs.runtimeSpaceLinkGeometry = &runtimeSpaceLinkGeometry;
	surfaceProbeFrameInputs.runtimeMutationGeometry = &runtimeMutationFrame.geometry;
	surfaceProbeFrameInputs.overlayGeometry = &overlayGeometry;
	surfaceProbeFrameInputs.activeDynamicGeometry = activeDynamicGeometry;
	mSurfaceProbeFrame = BuildNRISceneSurfaceProbeFrameState(surfaceProbeFrameInputs);

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, activeMaterialBridge, true);
	}
	if (activeGeometry->primitives.empty())
	{
		LogFallback("PT scene path produced no supported opaque geometry.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	if (mUsedStaticMapSceneLastFrame)
	{
		PrepareSceneTextureInputsForCompute();
	}


	return true;
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if ((drawmode != DM_MAINVIEW && drawmode != DM_OFFSCREEN) || portal || mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!mPathTracingSupported)
	{
		LogFallback(GetAvailabilityReason());
		return false;
	}

	ResetPerfTraceStats();
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.totalMs);
	Clocker totalClock(NriPTAll);

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	const NRIRendererFrameContext frameContext = BuildFrameContext(drawmode, portal, debugMode, preserveHistory);
	const uint32_t traceFrameIndex = frameContext.frameIndex;
	const RenderSceneHistorySnapshot history = CaptureRenderSceneHistorySnapshot(preserveHistory);
	if (!EnsureRenderSceneFrameResources(frameContext, preserveHistory, history) ||
		!BeginRenderSceneFrame(di, frameContext, preserveHistory, history))
	{
		return false;
	}

	if (bootstrapSimpleView)
	{
		return RenderSimpleBootstrapView(preserveHistory, history);
	}

	RenderSceneFrameBuildInputs sceneFrameInputs = {};
	sceneFrameInputs.bootstrapMode = bootstrapMode;
	sceneFrameInputs.bootstrapCapturedView = bootstrapCapturedView;
	sceneFrameInputs.bootstrapCapturedDiagnostics = bootstrapCapturedDiagnostics;
	sceneFrameInputs.bootstrapCapturedFlat = bootstrapCapturedFlat;
	sceneFrameInputs.bootstrapCapturedBaseColor = bootstrapCapturedBaseColor;
	sceneFrameInputs.rawTraceDirectScene = rawTraceDirectScene;
	sceneFrameInputs.preserveHistory = preserveHistory;
	RenderSceneFrameBuildResult sceneFrame;
	if (!BuildRenderSceneFrame(di, sceneFrameInputs, history, sceneFrame))
	{
		return false;
	}
	RenderSceneDispatchInputs dispatchInputs = {};
	dispatchInputs.bootstrapCapturedView = bootstrapCapturedView;
	dispatchInputs.buffersReady = sceneFrame.buffersReady;
	dispatchInputs.accelerationReady = sceneFrame.accelerationReady;
	dispatchInputs.drawInfo = &di;
	dispatchInputs.activeGeometry = sceneFrame.activeGeometry;
	dispatchInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	dispatchInputs.drawmode = drawmode;
	const bool dispatched = DispatchSelectedRenderScene(dispatchInputs);
	const bool success = sceneFrame.paletteReady && sceneFrame.texturesReady && sceneFrame.buffersReady && sceneFrame.accelerationReady && dispatched;
	LogRenderSceneFailureReasons(sceneFrame.paletteReady, sceneFrame.texturesReady, sceneFrame.buffersReady, sceneFrame.accelerationReady, dispatched, bootstrapCapturedView);

	RenderSceneCompletionInputs completionInputs = {};
	completionInputs.success = success;
	completionInputs.preserveHistory = preserveHistory;
	completionInputs.bootstrapCapturedView = bootstrapCapturedView;
	completionInputs.traceFrameIndex = traceFrameIndex;
	completionInputs.drawmode = drawmode;
	completionInputs.portal = portal;
	completionInputs.activeGeometry = sceneFrame.activeGeometry;
	completionInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	completionInputs.activeDynamicGeometry = sceneFrame.activeDynamicGeometry;
	completionInputs.usingPersistentDynamicEmissiveCache = sceneFrame.usingPersistentDynamicEmissiveCache;
	CommitRenderSceneResult(completionInputs, history);

	return success;
}
