#include "nri_scene_frame_mirrors.h"
#include "nri_cvars.h"
#include "nri_scene_lights.h"
#include "nri_upload_hash.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_bridge.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "gamecontrol.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "hw_sections.h"
#include "hw_voxels.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace
{
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

	static uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	static bool MirrorPlayerCaptureStatsDiffer(const NRIMirrorPlayerCaptureStats& a, const NRIMirrorPlayerCaptureStats& b)
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

	static void TraceMirrorPlayerCaptureStats(const NRIMirrorPlayerCaptureStats& stats)
	{
		static bool hasPrevious = false;
		static NRIMirrorPlayerCaptureStats previous = {};
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
		nri_scene::SceneView& outView,
		NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats)
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
		if (rebuildSceneViewStats != nullptr)
		{
			rebuildSceneViewStats(outView);
		}
		return true;
	}

	static bool CaptureMirrorPlayerDynamicScene(
		HWDrawInfo& di,
		HWPortal* mirrorPortal,
		int32_t selectedMirrorWallIndex,
		uint32_t mirrorPortalCandidates,
		nri_scene::SceneView& outView,
		NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats,
		NRIMirrorPlayerCaptureStats* outStats = nullptr)
	{
		outView = {};
		NRIMirrorPlayerCaptureStats captureStats = {};
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
		if (rebuildSceneViewStats != nullptr)
		{
			rebuildSceneViewStats(outView);
		}
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

	static bool ShouldTraceMirrorDynamicCapture()
	{
		return nri_voxelstats || (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0;
	}

	static uint64_t HashMirrorMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return NRIHashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
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

	static NRIMirrorPlayerUploadStamp BuildMirrorPlayerUploadProducerStamp(
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t frameIndex,
		uint64_t mapWorldBuildSerial)
	{
		NRIMirrorPlayerUploadStamp stamp = {};
		stamp.vertexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(1u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.indexPayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(2u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitivePayloadStamp = BuildConservativeMirrorPlayerPayloadStamp(3u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitiveProvenanceStamp = BuildConservativeMirrorPlayerPayloadStamp(4u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.materialPayloadStamp = HashMirrorMaterialPayloadData(materials);
		return stamp;
	}
}

NRIMirrorPortalSelectionResult SelectNRIPrimaryMirrorPortal(const NRIMirrorPortalSelectionRequest& request)
{
	NRIMirrorPortalSelectionResult result = {};
	if (request.drawInfo == nullptr)
	{
		return result;
	}
	result.portal = SelectPrimaryMirrorPortal(*request.drawInfo, result.candidateCount, result.selectedWallIndex, request.preferredWallIndex);
	return result;
}

NRIMirrorExtendedCaptureResult CaptureNRIMirrorExtendedDynamicScene(const NRIMirrorExtendedCaptureRequest& request, nri_scene::SceneView& outView)
{
	NRIMirrorExtendedCaptureResult result = {};
	if (request.drawInfo == nullptr)
	{
		outView = {};
		return result;
	}
	result.captured = CaptureMirrorExtendedDynamicScene(
		*request.drawInfo,
		request.mirrorPortal,
		request.selectedMirrorWallIndex,
		request.baseDynamicSceneView,
		request.frameIndex,
		outView,
		request.rebuildSceneViewStats);
	return result;
}

NRIMirrorPlayerCaptureResult CaptureNRIMirrorPlayerDynamicScene(const NRIMirrorPlayerCaptureRequest& request, nri_scene::SceneView& outView)
{
	NRIMirrorPlayerCaptureResult result = {};
	if (request.drawInfo == nullptr)
	{
		outView = {};
		return result;
	}
	result.captured = CaptureMirrorPlayerDynamicScene(
		*request.drawInfo,
		request.mirrorPortal,
		request.selectedMirrorWallIndex,
		request.mirrorPortalCandidates,
		outView,
		request.rebuildSceneViewStats,
		&result.stats);
	return result;
}

bool IsNRIMirrorPlayerPreviewCaptureEnabled()
{
	return IsMirrorPlayerPreviewCaptureEnabled();
}

NRIMirrorPlayerUploadStamp BuildNRIMirrorPlayerUploadProducerStamp(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials,
	uint64_t frameIndex,
	uint64_t mapWorldBuildSerial)
{
	return BuildMirrorPlayerUploadProducerStamp(geometry, materials, frameIndex, mapWorldBuildSerial);
}
