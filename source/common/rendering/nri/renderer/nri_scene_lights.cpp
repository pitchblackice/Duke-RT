#include "nri_scene_lights.h"

#include "c_cvars.h"
#include "gamefuncs.h"
#include "maptypes.h"
#include "palette.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

EXTERN_CVAR(Bool, nri_ptemissiveheuristics)
EXTERN_CVAR(Float, nri_ptemissiveminpower)
EXTERN_CVAR(Float, nri_ptemissiveminsurface)
EXTERN_CVAR(Float, nri_ptglowscale)
EXTERN_CVAR(Float, nri_ptglowreach)
EXTERN_CVAR(Float, nri_ptglowfalloff)
EXTERN_CVAR(Bool, nri_ptsectorlighting)
EXTERN_CVAR(Float, nri_ptsectorambientscale)
EXTERN_CVAR(Float, nri_ptsectorhemiscale)
EXTERN_CVAR(Float, nri_ptsectorfogscale)
EXTERN_CVAR(Float, nri_ptsectorclamp)
EXTERN_CVAR(Int, nri_ptsectorfilterpal)
EXTERN_CVAR(Int, nri_ptsectorfilterminshade)
EXTERN_CVAR(Int, nri_ptsectorfiltermaxshade)
EXTERN_CVAR(Int, nri_ptsectorfilterlotag)
EXTERN_CVAR(Int, nri_ptsectorpulseframes)
EXTERN_CVAR(Float, nri_ptsectorpulseamount)
EXTERN_CVAR(Int, nri_ptnudgetrace)

namespace
{
	constexpr float TwoPi = 6.28318530717958647692f;

	DVector3 PathTracingToWorldPosition(const DVector3& source)
	{
		return { source.X, -source.Z, -source.Y };
	}

	DVector3 WorldToPathTracingPosition(const DVector3& source)
	{
		return { source.X, -source.Z, -source.Y };
	}

	const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall: return "draw_list_wall";
		case nri_scene::SurfaceSourceType::MirrorWall: return "mirror_wall";
		case nri_scene::SurfaceSourceType::FloorFlat: return "floor_flat";
		case nri_scene::SurfaceSourceType::CeilingFlat: return "ceiling_flat";
		case nri_scene::SurfaceSourceType::FacingSprite: return "facing_sprite";
		case nri_scene::SurfaceSourceType::VoxelProxySprite: return "voxel_proxy_sprite";
		case nri_scene::SurfaceSourceType::MapWallBand: return "map_wall_band";
		case nri_scene::SurfaceSourceType::MapFloorSection: return "map_floor_section";
		case nri_scene::SurfaceSourceType::MapCeilingSection: return "map_ceiling_section";
		case nri_scene::SurfaceSourceType::MapPortalSurface: return "map_portal_surface";
		case nri_scene::SurfaceSourceType::DebugSphere: return "debug_sphere";
		default: return "unknown";
		}
	}

	void TraceSurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule,
		const char* pathName,
		const DVector3& sourcePosition,
		const DVector3& nudgedPosition,
		float displacement)
	{
		if (nri_ptnudgetrace <= 0)
		{
			return;
		}

		const DVector3 delta = nudgedPosition - sourcePosition;
		Printf(
			"NRI PT surface nudge: rule=%u path=%s source=%s sector=%d wall=%d nextsector=%d cstat=0x%x nudge=%.3f disp=%.3f from=(%.2f, %.2f, %.2f) to=(%.2f, %.2f, %.2f) delta=(%.2f, %.2f, %.2f)\n",
			rule.ruleId,
			pathName != nullptr ? pathName : "unknown",
			GetSurfaceSourceTypeName(record.provenance.sourceType),
			record.provenance.sectorIndex,
			record.provenance.wallIndex,
			record.provenance.nextSectorIndex,
			record.provenance.cstat,
			rule.nudgeFromSurfaceDistance,
			displacement,
			sourcePosition.X,
			sourcePosition.Y,
			sourcePosition.Z,
			nudgedPosition.X,
			nudgedPosition.Y,
			nudgedPosition.Z,
			delta.X,
			delta.Y,
			delta.Z);
	}

	void Copy3f(const float* source, float* destination)
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	void ComputeSurfaceBounds(const nri_scene::SurfaceRef& surface, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;

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

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			const float dx = vertex.position[0] - outCenter[0];
			const float dy = vertex.position[1] - outCenter[1];
			const float dz = vertex.position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}

	float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	float ComputeSurfaceArea(const nri_scene::SurfaceRef& surface)
	{
		if (surface.vertices.size() < 3)
		{
			return 0.0f;
		}

		float area = 0.0f;
		if ((surface.material.flags & nri_scene::MaterialFlag_Flat) != 0)
		{
			for (uint32_t i = 0; i + 2 < surface.vertices.size(); i += 3)
			{
				area += ComputeTriangleArea(surface.vertices[i], surface.vertices[i + 1], surface.vertices[i + 2]);
			}
		}
		else
		{
			const nri_scene::CapturedVertex& root = surface.vertices[0];
			for (uint32_t i = 1; i + 1 < surface.vertices.size(); ++i)
			{
				area += ComputeTriangleArea(root, surface.vertices[i], surface.vertices[i + 1]);
			}
		}

		return area;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint64_t QuantizePositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, (uint64_t)x);
		key = HashCombine64(key, (uint64_t)y);
		key = HashCombine64(key, (uint64_t)z);
		return key;
	}

	uint64_t HashTaggedSignedValue(uint64_t hash, uint64_t tag, int32_t value)
	{
		hash = HashCombine64(hash, tag);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)(value + 1));
		return hash;
	}

	uint64_t BuildSurfaceIdentityKey(const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, (uint64_t)(uint32_t)record.source);
		key = HashCombine64(key, (uint64_t)(uint32_t)record.provenance.sourceType);
		key = HashCombine64(key, (uint64_t)record.provenance.drawListType);

		bool hasAuthoritativeOwnership = false;
		if (record.provenance.actorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xA11C700000000001ull, record.provenance.actorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.sectorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x5EC70B5E00000001ull, record.provenance.sectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.wallIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xAA11000000000001ull, record.provenance.wallIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.sectionIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x5EC7100000000001ull, record.provenance.sectionIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.mapChunkIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xC4C0000000000001ull, record.provenance.mapChunkIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.nextSectorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x9E57000000000001ull, record.provenance.nextSectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (!hasAuthoritativeOwnership)
		{
			key = HashCombine64(key, 0xCE173E0000000001ull);
			key = HashCombine64(key, QuantizePositionKey(record.center));
		}

		return key;
	}

	uint64_t BuildAnalyticTopologyKey(uint32_t sourceFlags, uint32_t ruleId, const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombine64(key, (uint64_t)sourceFlags);
		key = HashCombine64(key, (uint64_t)ruleId);
		key = HashCombine64(key, record.identityKey);
		return key;
	}

	uint64_t HashQuantizedFloat(uint64_t hash, float value, float scale)
	{
		const int64_t quantized = (int64_t)std::llround((double)value * (double)scale);
		return HashCombine64(hash, (uint64_t)quantized);
	}

	uint64_t BuildAnalyticPropertyHash(const SceneLightSystem::SceneAnalyticLight& light)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)light.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)light.actorIndex);
		hash = HashCombine64(hash, (uint64_t)light.textureId);
		hash = HashQuantizedFloat(hash, light.position[0], 16.0f);
		hash = HashQuantizedFloat(hash, light.position[1], 16.0f);
		hash = HashQuantizedFloat(hash, light.position[2], 16.0f);
		hash = HashQuantizedFloat(hash, light.color[0], 4096.0f);
		hash = HashQuantizedFloat(hash, light.color[1], 4096.0f);
		hash = HashQuantizedFloat(hash, light.color[2], 4096.0f);
		hash = HashQuantizedFloat(hash, light.intensity, 4096.0f);
		hash = HashQuantizedFloat(hash, light.radius, 16.0f);
		return hash;
	}

	uint64_t BuildAnalyticBindingHash(const SceneLightSystem::SceneAnalyticLight& light)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)light.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)light.sourceRuleId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)light.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)light.actorIndex);
		hash = HashCombine64(hash, (uint64_t)light.textureId);
		return hash;
	}

	float EvaluateFlickerScale(uint64_t stableKey, uint32_t frameIndex, uint32_t flickerFrames)
	{
		if (flickerFrames <= 1)
		{
			return 1.0f;
		}

		const uint32_t seed = (uint32_t)(stableKey ^ (stableKey >> 32));
		const uint32_t phaseFrame = (frameIndex + seed) % flickerFrames;
		const float phase = ((float)phaseFrame / (float)flickerFrames) * TwoPi;
		return 0.35f + 0.65f * (0.5f + 0.5f * std::cos(phase));
	}

	float EvaluatePulseScale(uint64_t stableKey, uint32_t frameIndex, uint32_t pulseFrames, float pulseAmount)
	{
		if (pulseFrames <= 1 || pulseAmount <= 0.0f)
		{
			return 1.0f;
		}

		const float clampedAmount = std::clamp(pulseAmount, 0.0f, 0.95f);
		const float baseScale = 1.0f - clampedAmount;
		return baseScale + clampedAmount * EvaluateFlickerScale(stableKey ^ 0x5EC70B5E00000000ull, frameIndex, pulseFrames);
	}

	uint64_t AdvanceOverlayRandomState(uint64_t& state)
	{
		state += 0x9e3779b97f4a7c15ull;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	float NextOverlayUnitRandom(uint64_t& state)
	{
		const uint64_t bits = AdvanceOverlayRandomState(state);
		return (float)((bits >> 40) & 0xFFFFFFu) * (1.0f / 16777215.0f);
	}

	float EvaluateOverlayRandomIntensityOffset(uint64_t stableKey, uint32_t renderFrameIndex, float minValue, float maxValue)
	{
		if (!std::isfinite(minValue) || !std::isfinite(maxValue))
		{
			return 0.0f;
		}

		if (minValue > maxValue)
		{
			std::swap(minValue, maxValue);
		}

		if (minValue == maxValue)
		{
			return minValue;
		}

		uint64_t randomState = stableKey ^ 0xA17F4D6300000000ull ^ ((uint64_t)renderFrameIndex * 0x9e3779b97f4a7c15ull);
		return minValue + (maxValue - minValue) * NextOverlayUnitRandom(randomState);
	}

	float ResolveOverlayLightIntensity(
		float baseIntensity,
		uint64_t stableKey,
		uint32_t flickerTimeIndex,
		uint32_t renderFrameIndex,
		uint32_t flickerFrames,
		bool hasRandomIntensity,
		const float randomIntensityRange[2])
	{
		if (hasRandomIntensity)
		{
			const float intensityOffset = EvaluateOverlayRandomIntensityOffset(
				stableKey,
				renderFrameIndex,
				randomIntensityRange[0],
				randomIntensityRange[1]);
			return std::max(baseIntensity + intensityOffset, 0.0f);
		}

		return baseIntensity * EvaluateFlickerScale(stableKey, flickerTimeIndex, flickerFrames);
	}

	bool IsValidSurfaceNudgeDistance(float distance)
	{
		return std::isfinite(distance) && distance > 0.0f;
	}

	bool TryResolveSurfaceNudgeSector(
		const SceneLightSystem::SurfaceRecord& record,
		const DVector3& position,
		sectortype*& outSector)
	{
		outSector =
			record.provenance.sectorIndex >= 0 && (unsigned)record.provenance.sectorIndex < sector.Size() ?
			&sector[(unsigned)record.provenance.sectorIndex] :
			nullptr;

		sectortype* candidate = outSector;
		updatesectorz(position, &candidate);
		if (candidate != nullptr)
		{
			outSector = candidate;
			return true;
		}

		candidate = outSector;
		updatesector(position, &candidate);
		if (candidate != nullptr)
		{
			outSector = candidate;
			return true;
		}

		const int bestSectorIndex = FindBestSector(position);
		if (bestSectorIndex >= 0 && (unsigned)bestSectorIndex < sector.Size())
		{
			outSector = &sector[(unsigned)bestSectorIndex];
			return true;
		}

		outSector = nullptr;
		return false;
	}

	bool IsWallLikeSurfaceSource(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::MirrorWall:
		case nri_scene::SurfaceSourceType::MapWallBand:
			return true;
		default:
			return false;
		}
	}

	DVector2 ComputeSectorCenter2D(const sectortype* sec)
	{
		if (sec == nullptr || sec->walls.Size() == 0)
		{
			return {};
		}

		DVector2 center = {};
		for (const auto& wal : sec->walls)
		{
			center += wal.pos;
		}
		return center / (double)sec->walls.Size();
	}

	bool TryProjectAwayFromWall(
		const DVector3& sourcePosition,
		const walltype& sourceWall,
		float nudgeDistance,
		const sectortype* preferredSideSector,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (!IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		DVector2 nearestPoint = {};
		const double maxDistanceSquared = (double)nudgeDistance * (double)nudgeDistance;
		const double distanceSquared = SquareDistToWall(sourcePosition.X, sourcePosition.Y, &sourceWall, &nearestPoint);
		if (distanceSquared > maxDistanceSquared)
		{
			return false;
		}

		DVector2 wallNormal = sourceWall.delta().Rotated90CCW().Unit();
		const DVector2 nearestToSource = sourcePosition.XY() - nearestPoint;
		if (nearestToSource.LengthSquared() > 1e-12)
		{
			if (nearestToSource.dot(wallNormal) < 0.0)
			{
				wallNormal = -wallNormal;
			}
		}
		else if (preferredSideSector != nullptr)
		{
			const DVector2 sectorDelta = ComputeSectorCenter2D(preferredSideSector) - nearestPoint;
			if (sectorDelta.dot(wallNormal) < 0.0)
			{
				wallNormal = -wallNormal;
			}
		}

		const DVector2 nudgedXY = nearestPoint + wallNormal * nudgeDistance;
		const DVector2 displacement = nudgedXY - sourcePosition.XY();
		const double displacementSquared = displacement.LengthSquared();
		if (displacementSquared <= 1e-12)
		{
			return false;
		}

		outPosition = sourcePosition;
		outPosition.X = nudgedXY.X;
		outPosition.Y = nudgedXY.Y;
		outDisplacement = (float)std::sqrt(displacementSquared);
		return true;
	}

	bool TryApplyProvenanceWallSurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const DVector3& sourcePosition,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (!IsWallLikeSurfaceSource(record.provenance.sourceType) ||
			record.provenance.wallIndex < 0 ||
			(unsigned)record.provenance.wallIndex >= wall.Size())
		{
			return false;
		}

		const walltype& sourceWall = wall[(unsigned)record.provenance.wallIndex];
		const sectortype* preferredSideSector =
			record.provenance.sectorIndex >= 0 && (unsigned)record.provenance.sectorIndex < sector.Size() ?
			&sector[(unsigned)record.provenance.sectorIndex] :
			nullptr;
		return TryProjectAwayFromWall(sourcePosition, sourceWall, nudgeDistance, preferredSideSector, outPosition, outDisplacement);
	}

	bool TryApplyWallSurfaceNudge(
		const DVector3& sourcePosition,
		sectortype* startSector,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (startSector == nullptr || !IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		walltype* bestWall = nullptr;
		double bestDistanceSquared = DBL_MAX;
		const double maxDistanceSquared = (double)nudgeDistance * (double)nudgeDistance;

		BFSSectorSearch search(startSector);
		while (auto sec = search.GetNext())
		{
			for (auto& wal : sec->walls)
			{
				DVector2 nearestPoint = {};
				const double distanceSquared = SquareDistToWall(sourcePosition.X, sourcePosition.Y, &wal, &nearestPoint);
				if (distanceSquared > maxDistanceSquared)
				{
					continue;
				}

				bool blocked = false;
				if (!wal.twoSided())
				{
					blocked = true;
				}
				else
				{
					const DVector2 nearestPoint = NearestPointOnWall(sourcePosition.X, sourcePosition.Y, &wal);
					blocked = checkOpening(nearestPoint, sourcePosition.Z, sec, wal.nextSector(), 0.0, 0.0);
					if (!blocked)
					{
						search.Add(wal.nextSector());
					}
				}

				if (!blocked || distanceSquared >= bestDistanceSquared)
				{
					continue;
				}

				bestWall = &wal;
				bestDistanceSquared = distanceSquared;
			}
		}

		if (bestWall == nullptr)
		{
			return false;
		}

		return TryProjectAwayFromWall(sourcePosition, *bestWall, nudgeDistance, nullptr, outPosition, outDisplacement);
	}

	bool TryApplyPlaneSurfaceNudge(
		const DVector3& sourcePosition,
		sectortype* startSector,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (startSector == nullptr || !IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		double ceilingZ = -FLT_MAX;
		double floorZ = FLT_MAX;
		CollisionBase ceilingHit = {};
		CollisionBase floorHit = {};
		getzrange(sourcePosition, startSector, &ceilingZ, ceilingHit, &floorZ, floorHit, nudgeDistance, 0u);

		double bestTargetZ = sourcePosition.Z;
		double bestDisplacement = DBL_MAX;
		bool foundCandidate = false;

		const double ceilingDistance = sourcePosition.Z - ceilingZ;
		if (ceilingHit.type == kHitSector &&
			ceilingDistance >= 0.0 &&
			ceilingDistance < nudgeDistance)
		{
			const double targetZ = ceilingZ + nudgeDistance;
			const double displacement = targetZ - sourcePosition.Z;
			if (displacement > 0.0 && displacement < bestDisplacement)
			{
				bestTargetZ = targetZ;
				bestDisplacement = displacement;
				foundCandidate = true;
			}
		}

		const double floorDistance = floorZ - sourcePosition.Z;
		if (floorHit.type == kHitSector &&
			floorDistance >= 0.0 &&
			floorDistance < nudgeDistance)
		{
			const double targetZ = floorZ - nudgeDistance;
			const double displacement = sourcePosition.Z - targetZ;
			if (displacement > 0.0 && displacement < bestDisplacement)
			{
				bestTargetZ = targetZ;
				bestDisplacement = displacement;
				foundCandidate = true;
			}
		}

		if (!foundCandidate)
		{
			return false;
		}

		outPosition = sourcePosition;
		outPosition.Z = bestTargetZ;
		outDisplacement = (float)bestDisplacement;
		return true;
	}

	void ApplyActorOverlaySurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule,
		float position[3])
	{
		if (!rule.hasNudgeFromSurface || !IsValidSurfaceNudgeDistance(rule.nudgeFromSurfaceDistance) || position == nullptr)
		{
			return;
		}

		const DVector3 sourceRenderPosition(position[0], position[1], position[2]);
		const DVector3 sourceWorldPosition = PathTracingToWorldPosition(sourceRenderPosition);
		sectortype* startSector = nullptr;
		if (!TryResolveSurfaceNudgeSector(record, sourceWorldPosition, startSector) || startSector == nullptr)
		{
			return;
		}

		DVector3 provenanceWallPosition = sourceWorldPosition;
		float provenanceWallDisplacement = 0.0f;
		if (TryApplyProvenanceWallSurfaceNudge(record, sourceWorldPosition, rule.nudgeFromSurfaceDistance, provenanceWallPosition, provenanceWallDisplacement))
		{
			const DVector3 provenanceWallRenderPosition = WorldToPathTracingPosition(provenanceWallPosition);
			TraceSurfaceNudge(record, rule, "source_wall", sourceRenderPosition, provenanceWallRenderPosition, provenanceWallDisplacement);
			position[0] = (float)provenanceWallRenderPosition.X;
			position[1] = (float)provenanceWallRenderPosition.Y;
			position[2] = (float)provenanceWallRenderPosition.Z;
			return;
		}

		DVector3 bestPosition = sourceWorldPosition;
		float bestDisplacement = FLT_MAX;

		DVector3 wallPosition = sourceWorldPosition;
		float wallDisplacement = 0.0f;
		if (TryApplyWallSurfaceNudge(sourceWorldPosition, startSector, rule.nudgeFromSurfaceDistance, wallPosition, wallDisplacement) &&
			wallDisplacement < bestDisplacement)
		{
			bestPosition = wallPosition;
			bestDisplacement = wallDisplacement;
		}

		DVector3 planePosition = sourceWorldPosition;
		float planeDisplacement = 0.0f;
		if (TryApplyPlaneSurfaceNudge(sourceWorldPosition, startSector, rule.nudgeFromSurfaceDistance, planePosition, planeDisplacement) &&
			planeDisplacement < bestDisplacement)
		{
			bestPosition = planePosition;
			bestDisplacement = planeDisplacement;
		}

		if (bestDisplacement == FLT_MAX)
		{
			return;
		}

		const DVector3 bestRenderPosition = WorldToPathTracingPosition(bestPosition);
		TraceSurfaceNudge(record, rule, bestPosition.Z != sourceWorldPosition.Z ? "plane" : "wall_fallback", sourceRenderPosition, bestRenderPosition, bestDisplacement);

		position[0] = (float)bestRenderPosition.X;
		position[1] = (float)bestRenderPosition.Y;
		position[2] = (float)bestRenderPosition.Z;
	}

	float ComputeColorLuminance(const float color[3])
	{
		return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
	}

	float ComputeBuildLightLevel(int shade, int paletteIndex)
	{
		const int clampedPalette = clamp(paletteIndex, 0, MAXPALOOKUPS - 1);
		const float shadeDiv = lookups.tables[clampedPalette].ShadeFactor;
		const bool fullbright = shadeDiv < 1.0f / 1000.0f || shade < -numshades;
		if (fullbright)
		{
			return 1.0f;
		}

		float inverseLight = (float)shade * 255.0f / (float)numshades;
		inverseLight /= shadeDiv;
		const float lightLevel = clamp(255.0f - inverseLight, 0.0f, 255.0f);
		return lightLevel / 255.0f;
	}

	bool IsGlowDrivenEmissive(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		if (emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			return true;
		}

		return (sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	float ResolveGlowStrengthScale(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max((float)nri_ptglowscale, 0.0f) : 1.0f;
	}

	float ResolveGlowSamplingScale(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max((float)nri_ptglowreach, 0.0f) : 1.0f;
	}

	float ResolveGlowFalloffScale(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::clamp((float)nri_ptglowfalloff, 0.25f, 4.0f) : 1.0f;
	}

	bool HasPalEntryColor(const PalEntry& color)
	{
		return color.r != 0 || color.g != 0 || color.b != 0;
	}

	void ResolveSectorTint(const sectortype& sec, int paletteIndex, float outTint[3], float& outFogStrength)
	{
		(void)paletteIndex;

		outTint[0] = 1.0f;
		outTint[1] = 1.0f;
		outTint[2] = 1.0f;
		outFogStrength = 0.0f;

		const float visibilityStrength = std::clamp((float)sec.visibility / 128.0f, 0.0f, 1.0f);
		const bool hasExplicitFogPalette = sec.fogpal > 0;
		outFogStrength = hasExplicitFogPalette ? std::max(visibilityStrength, 0.35f) : visibilityStrength;
		if (!hasExplicitFogPalette)
		{
			return;
		}

		PalEntry fade = {};
		fade = lookups.getFade(clamp((int)sec.fogpal, 0, MAXPALOOKUPS - 1));

		const bool hasFogTint = HasPalEntryColor(fade);
		if (!hasFogTint)
		{
			return;
		}

		const float tint[3] = {
			(float)fade.r / 255.0f,
			(float)fade.g / 255.0f,
			(float)fade.b / 255.0f,
		};
		const float tintWeight = std::clamp((hasExplicitFogPalette ? 0.20f : 0.10f) + outFogStrength * 0.35f, 0.0f, 0.65f);
		outTint[0] = 1.0f + (tint[0] - 1.0f) * tintWeight;
		outTint[1] = 1.0f + (tint[1] - 1.0f) * tintWeight;
		outTint[2] = 1.0f + (tint[2] - 1.0f) * tintWeight;
	}

	uint64_t BuildEmissiveTopologyKey(const SceneLightSystem::SurfaceRecord& record)
	{
		return record.identityKey;
	}

	uint64_t BuildEmissivePropertyHash(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)emissive.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)emissive.sourceRuleId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.textureId);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveTextureIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveMode);
		hash = HashQuantizedFloat(hash, emissive.center[0], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.center[1], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.center[2], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.boundsRadius, 16.0f);
		hash = HashQuantizedFloat(hash, emissive.surfaceArea, 16.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[0], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[1], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[2], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveIntensity, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.powerEstimate, 256.0f);
		return hash;
	}

	uint64_t BuildEmissiveBindingHash(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)emissive.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)emissive.sourceRuleId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.textureId);
		hash = HashCombine64(hash, (uint64_t)emissive.materialIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveMode);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveTextureIndex);
		return hash;
	}

	bool EvaluateEmissiveMaterial(
		const SceneLightSystem::EmissiveSurfaceRegistry& registry,
		const nri_scene::MaterialLightingMetadata& metadata,
		uint32_t& outSourceFlags,
		uint32_t& outRuleId,
		float outColor[3],
		float& outIntensity,
		uint32_t& outMode,
		uint32_t& outTextureIndex,
		float& outSamplingScale,
		float& outFalloffScale)
	{
		outSourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		outRuleId = 0;
		outColor[0] = 0.0f;
		outColor[1] = 0.0f;
		outColor[2] = 0.0f;
		outIntensity = 0.0f;
		outMode = nri_scene::MaterialEmissiveMode_None;
		outTextureIndex = UINT32_MAX;
		outSamplingScale = 1.0f;
		outFalloffScale = 1.0f;

		if (nri_ptemissiveheuristics)
		{
			if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_MaterialFullbright | nri_scene::MaterialLightingFlag_TextureFullbright)) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoFullbright;
			}
			if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_TextureGlowing | nri_scene::MaterialLightingFlag_TextureAutoGlowing)) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoTextureGlow;
			}
			if ((metadata.lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0)
			{
				outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoGlowmap;
			}

			if (metadata.emissiveMode != nri_scene::MaterialEmissiveMode_None && metadata.emissiveIntensity > 0.0f)
			{
				outMode = metadata.emissiveMode;
				outTextureIndex = metadata.emissiveTextureIndex;
				outIntensity = metadata.emissiveIntensity;
				Copy3f(metadata.emissiveColor, outColor);
			}
		}

		for (const auto& rule : registry.textureRules)
		{
			if (metadata.textureId != rule.textureId)
			{
				continue;
			}

			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule;
			outRuleId = rule.ruleId;
			const float baseIntensity = outIntensity > 0.0f ? outIntensity : 1.0f;
			switch (rule.emissiveMode)
			{
			case nri_scene::MaterialEmissiveMode_UseBaseTexture:
				outMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
				outTextureIndex = metadata.textureIndex;
				outColor[0] = metadata.averageColor[0];
				outColor[1] = metadata.averageColor[1];
				outColor[2] = metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseGlowmapTexture:
				if (metadata.glowmapTextureIndex != UINT32_MAX)
				{
					outMode = nri_scene::MaterialEmissiveMode_UseGlowmapTexture;
					outTextureIndex = metadata.glowmapTextureIndex;
				}
				else
				{
					outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				}
				outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
				outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
				outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseConstantColor:
				outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				if (rule.hasExplicitColor)
				{
					Copy3f(rule.emissiveColor, outColor);
				}
				else
				{
					outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
					outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
					outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				}
				break;
			default:
				break;
			}
			outIntensity = baseIntensity * std::max(rule.intensityScale, 0.0f);
			break;
		}

		if (outMode == nri_scene::MaterialEmissiveMode_None || outIntensity <= 0.0f)
		{
			return false;
		}

		outIntensity *= ResolveGlowStrengthScale(outSourceFlags, outMode);
		outSamplingScale = ResolveGlowSamplingScale(outSourceFlags, outMode);
		outFalloffScale = ResolveGlowFalloffScale(outSourceFlags, outMode);
		return outIntensity > 0.0f;
	}
}

void SceneLightSystem::Reset()
{
	mAnalyticLights = {};
	mEmissiveSurfaces = {};
	mSectorLighting = {};
	mEnvironmentLighting = {};
	mSurfaceRecords.clear();
	mFrameAppendStats = {};
	mFrameSerial = 0;
}

void SceneLightSystem::ResetLevelState()
{
	mAnalyticLights.manualLights.clear();
	mAnalyticLights.transientLights.clear();
	mAnalyticLights.activeLights.clear();
	mAnalyticLights.activeTopologyKeys.clear();
	mAnalyticLights.activePropertyHashes.clear();
	mAnalyticLights.activeBindingHashes.clear();
	mAnalyticLights.activeDiagnosticFlags.clear();
	mAnalyticLights.addedTopologyKeys.clear();
	mAnalyticLights.removedTopologyKeys.clear();
	mAnalyticLights.reboundTopologyKeys.clear();
	mAnalyticLights.matchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayRuleCount = 0;
	mAnalyticLights.actorOverlayMatchedSurfaceCount = 0;
	mAnalyticLights.mapOverlayRuleCount = 0;
	mAnalyticLights.transientMuzzleSlotCount = 0;
	mAnalyticLights.transientMuzzleActiveCount = 0;
	mAnalyticLights.dedupedMatchCount = 0;
	mAnalyticLights.truncatedLightCount = 0;
	mAnalyticLights.topologyChanged = false;
	mAnalyticLights.propertiesChanged = false;
	mAnalyticLights.lastBuildTopologyChanged = false;
	mAnalyticLights.lastBuildPropertiesChanged = false;

	mEmissiveSurfaces.activeSurfaces.clear();
	mEmissiveSurfaces.activeTopologyKeys.clear();
	mEmissiveSurfaces.activePropertyHashes.clear();
	mEmissiveSurfaces.activeBindingHashes.clear();
	mEmissiveSurfaces.activeDiagnosticFlags.clear();
	mEmissiveSurfaces.addedTopologyKeys.clear();
	mEmissiveSurfaces.removedTopologyKeys.clear();
	mEmissiveSurfaces.reboundTopologyKeys.clear();
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;
	mEmissiveSurfaces.topologyChanged = false;
	mEmissiveSurfaces.propertiesChanged = false;
	mEmissiveSurfaces.materialBindingChanged = false;
	mEmissiveSurfaces.materialPropertiesChanged = false;
	mEmissiveSurfaces.lastBuildTopologyChanged = false;
	mEmissiveSurfaces.lastBuildPropertiesChanged = false;

	mSectorLighting = {};
	mEnvironmentLighting = {};
	mSurfaceRecords.clear();
	mFrameAppendStats = {};
	mFrameSerial = 0;
}

void SceneLightSystem::BeginFrame(uint64_t frameSerial)
{
	mFrameSerial = frameSerial;
	mSurfaceRecords.clear();
	mFrameAppendStats = {};
	mAnalyticLights.matchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayRuleCount = 0;
	mAnalyticLights.actorOverlayMatchedSurfaceCount = 0;
	mAnalyticLights.mapOverlayRuleCount = 0;
	mAnalyticLights.dedupedMatchCount = 0;
	mAnalyticLights.truncatedLightCount = 0;
	mAnalyticLights.topologyChanged = false;
	mAnalyticLights.propertiesChanged = false;
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;
	mEmissiveSurfaces.topologyChanged = false;
	mEmissiveSurfaces.propertiesChanged = false;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.topologyChanged = false;
}

void SceneLightSystem::AppendSceneView(
	const nri_scene::SceneView& sceneView,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	const SurfaceIdentityOverrides* identityOverrides)
{
	uint32_t localMaterialIndex = 0;
	AppendSurfaceList(
		sceneView.opaqueWalls,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueWalls : nullptr);
	AppendSurfaceList(
		sceneView.opaqueFlats,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueFlats : nullptr);
	AppendSurfaceList(
		sceneView.opaqueSprites,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueSprites : nullptr);
}

void SceneLightSystem::AppendSpriteSurfaces(
	const std::vector<nri_scene::SurfaceRef>& surfaces,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	const std::vector<uint64_t>* identityOverrides)
{
	uint32_t localMaterialIndex = 0;
	AppendSurfaceList(
		surfaces,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides);
}

uint64_t SceneLightSystem::ComputeSurfaceIdentityKey(
	SceneLightRecordSource source,
	const nri_scene::SurfaceProvenance& provenance,
	const float center[3])
{
	SurfaceRecord record = {};
	record.source = source;
	record.provenance = provenance;
	Copy3f(center, record.center);
	return BuildSurfaceIdentityKey(record);
}

void SceneLightSystem::RebuildAnalyticLights(
	uint32_t flickerTimeIndex,
	uint32_t renderFrameIndex,
	uint32_t maxActiveLights,
	const std::unordered_map<int32_t, std::vector<AnalyticLightRegistry::ActorOverlayRule>>* actorOverlayRules,
	const std::vector<AnalyticLightRegistry::MapOverlayRule>* mapOverlayRules)
{
	std::vector<SceneAnalyticLight> nextLights;
	size_t overlayRuleCount = 0;
	if (actorOverlayRules != nullptr)
	{
		for (const auto& entry : *actorOverlayRules)
		{
			overlayRuleCount += entry.second.size();
		}
	}
	const size_t mapOverlayRuleCount = mapOverlayRules != nullptr ? mapOverlayRules->size() : 0u;
	mAnalyticLights.actorOverlayRuleCount = (uint32_t)overlayRuleCount;
	mAnalyticLights.mapOverlayRuleCount = (uint32_t)mapOverlayRuleCount;
	nextLights.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.transientLights.size() + mAnalyticLights.spriteTileRules.size() + overlayRuleCount + mapOverlayRuleCount);
	std::unordered_map<uint64_t, size_t> keyToLightIndex;
	keyToLightIndex.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.transientLights.size() + mAnalyticLights.spriteTileRules.size() * 4u + overlayRuleCount * 2u + mapOverlayRuleCount);

	auto tryAppendLight = [this, &nextLights, &keyToLightIndex, maxActiveLights](const SceneAnalyticLight& light)
	{
		if (keyToLightIndex.find(light.stableKey) != keyToLightIndex.end())
		{
			mAnalyticLights.dedupedMatchCount++;
			return;
		}

		if (nextLights.size() >= maxActiveLights)
		{
			mAnalyticLights.truncatedLightCount++;
			return;
		}

		keyToLightIndex.emplace(light.stableKey, nextLights.size());
		nextLights.push_back(light);
	};

	for (const SceneAnalyticLight& manualLight : mAnalyticLights.manualLights)
	{
		tryAppendLight(manualLight);
	}

	for (const SceneAnalyticLight& transientLight : mAnalyticLights.transientLights)
	{
		tryAppendLight(transientLight);
	}

	for (const AnalyticLightHeuristicRule& rule : mAnalyticLights.spriteTileRules)
	{
		for (const SurfaceRecord& record : mSurfaceRecords)
		{
			if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) == 0)
			{
				continue;
			}
			if (record.material.textureId != rule.textureId)
			{
				continue;
			}

			mAnalyticLights.matchedSurfaceCount++;

			SceneAnalyticLight light = {};
			light.stableKey = BuildAnalyticTopologyKey(SceneAnalyticLightSourceFlag_SpriteTileHeuristic, rule.ruleId, record);
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_SpriteTileHeuristic;
			light.sourceRuleId = rule.ruleId;
			light.source = record.source;
			light.actorIndex = record.provenance.actorIndex;
			light.textureId = record.material.textureId;
			Copy3f(record.center, light.position);
			Copy3f(rule.color, light.color);
			light.intensity = rule.intensity * EvaluateFlickerScale(light.stableKey, flickerTimeIndex, rule.flickerFrames);
			light.radius = rule.radius;
			tryAppendLight(light);
		}
	}

	if (actorOverlayRules != nullptr && !actorOverlayRules->empty())
	{
		for (const SurfaceRecord& record : mSurfaceRecords)
		{
			if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) == 0 ||
				record.provenance.actorIndex < 0)
			{
				continue;
			}

			const auto actorRuleIt = actorOverlayRules->find(record.provenance.actorIndex);
			if (actorRuleIt == actorOverlayRules->end())
			{
				continue;
			}

			for (const AnalyticLightRegistry::ActorOverlayRule& rule : actorRuleIt->second)
			{
				if (rule.hasTileFilter && record.material.textureId != rule.tileFilter)
				{
					continue;
				}

				mAnalyticLights.matchedSurfaceCount++;
				mAnalyticLights.actorOverlayMatchedSurfaceCount++;

				SceneAnalyticLight light = {};
				light.stableKey = BuildAnalyticTopologyKey(SceneAnalyticLightSourceFlag_ActorOverlay, rule.ruleId, record);
				light.id = 0;
				light.sourceFlags = SceneAnalyticLightSourceFlag_ActorOverlay;
				light.sourceRuleId = rule.ruleId;
				light.source = record.source;
				light.actorIndex = record.provenance.actorIndex;
				light.textureId = record.material.textureId;
				light.position[0] = record.center[0] + rule.offset[0];
				light.position[1] = record.center[1] + rule.offset[1];
				light.position[2] = record.center[2] + rule.offset[2];
				ApplyActorOverlaySurfaceNudge(record, rule, light.position);
				Copy3f(rule.color, light.color);
				light.intensity = ResolveOverlayLightIntensity(
					rule.intensity,
					light.stableKey,
					flickerTimeIndex,
					renderFrameIndex,
					rule.flickerFrames,
					rule.hasRandomIntensity,
					rule.randomIntensityRange);
				light.radius = rule.radius;
				tryAppendLight(light);
			}
		}
	}

	if (mapOverlayRules != nullptr)
	{
		for (const AnalyticLightRegistry::MapOverlayRule& rule : *mapOverlayRules)
		{
			SceneAnalyticLight light = {};
			light.stableKey = rule.stableKey;
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_MapOverlay;
			light.sourceRuleId = rule.ruleId;
			light.source = rule.source;
			light.actorIndex = -1;
			light.textureId = 0;
			Copy3f(rule.position, light.position);
			Copy3f(rule.color, light.color);
			light.intensity = rule.intensity * EvaluateFlickerScale(light.stableKey, flickerTimeIndex, rule.flickerFrames);
			light.radius = rule.radius;
			tryAppendLight(light);
		}
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextLights.size());
	std::unordered_map<uint64_t, uint64_t> nextPropertyHashes;
	std::unordered_map<uint64_t, uint64_t> nextBindingHashes;
	std::unordered_map<uint64_t, uint32_t> nextDiagnosticFlags;
	nextPropertyHashes.reserve(nextLights.size());
	nextBindingHashes.reserve(nextLights.size());
	nextDiagnosticFlags.reserve(nextLights.size());
	for (const SceneAnalyticLight& light : nextLights)
	{
		nextTopologyKeys.push_back(light.stableKey);
		const uint64_t propertyHash = BuildAnalyticPropertyHash(light);
		const uint64_t bindingHash = BuildAnalyticBindingHash(light);
		uint32_t diagnosticFlags = SceneLightDiagnosticFlag_None;
		const auto previousPropertyIt = mAnalyticLights.activePropertyHashes.find(light.stableKey);
		if (previousPropertyIt != mAnalyticLights.activePropertyHashes.end())
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_PreviousMatch;
			if (previousPropertyIt->second != propertyHash)
			{
				diagnosticFlags |= SceneLightDiagnosticFlag_PropertyChanged;
			}
		}
		else
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Added;
		}

		const auto previousBindingIt = mAnalyticLights.activeBindingHashes.find(light.stableKey);
		if (previousBindingIt != mAnalyticLights.activeBindingHashes.end() && previousBindingIt->second != bindingHash)
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Rebound;
		}

		nextPropertyHashes.emplace(light.stableKey, propertyHash);
		nextBindingHashes.emplace(light.stableKey, bindingHash);
		nextDiagnosticFlags.emplace(light.stableKey, diagnosticFlags);
	}
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mAnalyticLights.topologyChanged = nextTopologyKeys != mAnalyticLights.activeTopologyKeys;
	mAnalyticLights.propertiesChanged = false;
	mAnalyticLights.addedTopologyKeys.clear();
	mAnalyticLights.removedTopologyKeys.clear();
	mAnalyticLights.reboundTopologyKeys.clear();
	for (const auto& entry : nextPropertyHashes)
	{
		const auto previousIt = mAnalyticLights.activePropertyHashes.find(entry.first);
		if (previousIt != mAnalyticLights.activePropertyHashes.end() && previousIt->second != entry.second)
		{
			mAnalyticLights.propertiesChanged = true;
			break;
		}
	}
	for (const auto& key : nextTopologyKeys)
	{
		if (mAnalyticLights.activePropertyHashes.find(key) == mAnalyticLights.activePropertyHashes.end())
		{
			mAnalyticLights.addedTopologyKeys.push_back(key);
		}
	}
	for (const auto& key : mAnalyticLights.activeTopologyKeys)
	{
		if (nextPropertyHashes.find(key) == nextPropertyHashes.end())
		{
			mAnalyticLights.removedTopologyKeys.push_back(key);
		}
	}
	for (const auto& entry : nextDiagnosticFlags)
	{
		if ((entry.second & SceneLightDiagnosticFlag_Rebound) != 0)
		{
			mAnalyticLights.reboundTopologyKeys.push_back(entry.first);
		}
	}
	mAnalyticLights.lastBuildTopologyChanged = mAnalyticLights.topologyChanged;
	mAnalyticLights.lastBuildPropertiesChanged = mAnalyticLights.propertiesChanged;
	mAnalyticLights.activeTopologyKeys = std::move(nextTopologyKeys);
	mAnalyticLights.activePropertyHashes = std::move(nextPropertyHashes);
	mAnalyticLights.activeBindingHashes = std::move(nextBindingHashes);
	mAnalyticLights.activeDiagnosticFlags = std::move(nextDiagnosticFlags);
	mAnalyticLights.activeLights = std::move(nextLights);
}

void SceneLightSystem::RebuildEmissiveSurfaces(uint32_t maxActiveSurfaces)
{
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;

	std::vector<EmissiveSurfaceRegistry::EmissiveSurfaceRecord> nextSurfaces;
	nextSurfaces.reserve(std::min<uint32_t>((uint32_t)mSurfaceRecords.size(), maxActiveSurfaces));

	const float minSurfaceArea = std::max((float)nri_ptemissiveminsurface, 0.0f);
	const float minPower = std::max((float)nri_ptemissiveminpower, 0.0f);

	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		uint32_t sourceRuleId = 0;
		float emissiveColor[3] = {};
		float emissiveIntensity = 0.0f;
		uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		float emissiveSamplingScale = 1.0f;
		float emissiveFalloffScale = 1.0f;
		if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, record.material, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
		{
			continue;
		}

		if (record.surfaceArea < minSurfaceArea)
		{
			continue;
		}

		const float resolvedLuminance = emissiveMode == nri_scene::MaterialEmissiveMode_UseBaseTexture ?
			ComputeColorLuminance(record.material.averageColor) :
			ComputeColorLuminance(emissiveColor);
		const float powerEstimate = record.surfaceArea * resolvedLuminance * emissiveIntensity;
		if (powerEstimate < minPower)
		{
			continue;
		}

		if (nextSurfaces.size() >= maxActiveSurfaces)
		{
			mEmissiveSurfaces.truncatedSurfaceCount++;
			continue;
		}

		EmissiveSurfaceRegistry::EmissiveSurfaceRecord emissive = {};
		emissive.stableKey = BuildEmissiveTopologyKey(record);
		emissive.sourceFlags = sourceFlags;
		emissive.sourceRuleId = sourceRuleId;
		emissive.source = record.source;
		emissive.actorIndex = record.provenance.actorIndex;
		emissive.textureId = record.material.textureId;
		emissive.emissiveTextureIndex = emissiveTextureIndex;
		emissive.materialIndex = record.materialIndex;
		emissive.emissiveMode = emissiveMode;
		emissive.surfaceArea = record.surfaceArea;
		emissive.boundsRadius = record.boundsRadius;
		emissive.powerEstimate = powerEstimate;
		Copy3f(record.center, emissive.center);
		Copy3f(emissiveColor, emissive.emissiveColor);
		emissive.emissiveIntensity = emissiveIntensity;
		nextSurfaces.push_back(emissive);

		if ((sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoFullbright | SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0)
		{
			mEmissiveSurfaces.autoTaggedCount++;
		}
		if ((sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) != 0)
		{
			mEmissiveSurfaces.explicitRuleMatchCount++;
		}
		mEmissiveSurfaces.totalPowerEstimate += powerEstimate;
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextSurfaces.size());
	std::unordered_map<uint64_t, uint64_t> nextPropertyHashes;
	std::unordered_map<uint64_t, uint64_t> nextBindingHashes;
	std::unordered_map<uint64_t, uint32_t> nextDiagnosticFlags;
	nextPropertyHashes.reserve(nextSurfaces.size());
	nextBindingHashes.reserve(nextSurfaces.size());
	nextDiagnosticFlags.reserve(nextSurfaces.size());
	for (const auto& emissive : nextSurfaces)
	{
		nextTopologyKeys.push_back(emissive.stableKey);
		const uint64_t propertyHash = BuildEmissivePropertyHash(emissive);
		const uint64_t bindingHash = BuildEmissiveBindingHash(emissive);
		uint32_t diagnosticFlags = SceneLightDiagnosticFlag_None;
		const auto previousPropertyIt = mEmissiveSurfaces.activePropertyHashes.find(emissive.stableKey);
		if (previousPropertyIt != mEmissiveSurfaces.activePropertyHashes.end())
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_PreviousMatch;
			if (previousPropertyIt->second != propertyHash)
			{
				diagnosticFlags |= SceneLightDiagnosticFlag_PropertyChanged;
			}
		}
		else
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Added;
		}

		const auto previousBindingIt = mEmissiveSurfaces.activeBindingHashes.find(emissive.stableKey);
		if (previousBindingIt != mEmissiveSurfaces.activeBindingHashes.end() && previousBindingIt->second != bindingHash)
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Rebound;
		}

		nextPropertyHashes.emplace(emissive.stableKey, propertyHash);
		nextBindingHashes.emplace(emissive.stableKey, bindingHash);
		nextDiagnosticFlags.emplace(emissive.stableKey, diagnosticFlags);
	}
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mEmissiveSurfaces.topologyChanged = nextTopologyKeys != mEmissiveSurfaces.activeTopologyKeys;
	mEmissiveSurfaces.propertiesChanged = false;
	mEmissiveSurfaces.addedTopologyKeys.clear();
	mEmissiveSurfaces.removedTopologyKeys.clear();
	mEmissiveSurfaces.reboundTopologyKeys.clear();
	for (const auto& entry : nextPropertyHashes)
	{
		const auto previousIt = mEmissiveSurfaces.activePropertyHashes.find(entry.first);
		if (previousIt != mEmissiveSurfaces.activePropertyHashes.end() && previousIt->second != entry.second)
		{
			mEmissiveSurfaces.propertiesChanged = true;
			break;
		}
	}
	for (const auto& key : nextTopologyKeys)
	{
		if (mEmissiveSurfaces.activePropertyHashes.find(key) == mEmissiveSurfaces.activePropertyHashes.end())
		{
			mEmissiveSurfaces.addedTopologyKeys.push_back(key);
		}
	}
	for (const auto& key : mEmissiveSurfaces.activeTopologyKeys)
	{
		if (nextPropertyHashes.find(key) == nextPropertyHashes.end())
		{
			mEmissiveSurfaces.removedTopologyKeys.push_back(key);
		}
	}
	for (const auto& entry : nextDiagnosticFlags)
	{
		if ((entry.second & SceneLightDiagnosticFlag_Rebound) != 0)
		{
			mEmissiveSurfaces.reboundTopologyKeys.push_back(entry.first);
		}
	}
	mEmissiveSurfaces.lastBuildTopologyChanged = mEmissiveSurfaces.topologyChanged;
	mEmissiveSurfaces.lastBuildPropertiesChanged = mEmissiveSurfaces.propertiesChanged;
	mEmissiveSurfaces.activeTopologyKeys = std::move(nextTopologyKeys);
	mEmissiveSurfaces.activePropertyHashes = std::move(nextPropertyHashes);
	mEmissiveSurfaces.activeBindingHashes = std::move(nextBindingHashes);
	mEmissiveSurfaces.activeDiagnosticFlags = std::move(nextDiagnosticFlags);
	mEmissiveSurfaces.activeSurfaces = std::move(nextSurfaces);
}

void SceneLightSystem::RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount)
{
	mSectorLighting.sectorCount = sectorCount;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.sectors.assign(sectorCount, {});

	std::vector<uint8_t> seenSectors(sectorCount, 0u);
	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= sectorCount)
		{
			continue;
		}

		seenSectors[sectorIndex] = 1u;
	}

	mSectorLighting.activeSectorIndices.clear();
	mSectorLighting.activeSectorIndices.reserve(sectorCount);

	if (!nri_ptsectorlighting || sectorCount == 0)
	{
		mSectorLighting.topologyChanged = !mSectorLighting.activeTopologyKeys.empty();
		mSectorLighting.activeTopologyKeys.clear();
		return;
	}

	const int paletteFilter = (int)nri_ptsectorfilterpal;
	const int minShadeFilter = (int)nri_ptsectorfilterminshade;
	const int maxShadeFilter = std::max(minShadeFilter, (int)nri_ptsectorfiltermaxshade);
		const int lotagFilter = (int)nri_ptsectorfilterlotag;
		const uint32_t pulseFrames = std::max(0, (int)nri_ptsectorpulseframes);
		const float pulseAmount = std::max(0.0f, (float)nri_ptsectorpulseamount);
		const bool pulseSelectionFiltered =
			paletteFilter >= 0 ||
			lotagFilter >= 0 ||
			minShadeFilter > -128 ||
			maxShadeFilter < 127;
		const float ambientScale = std::max(0.0f, (float)nri_ptsectorambientscale);
		const float hemisphereScale = std::max(0.0f, (float)nri_ptsectorhemiscale);
		const float fogScale = std::max(0.0f, (float)nri_ptsectorfogscale);
	const float sectorClamp = std::max(0.0f, (float)nri_ptsectorclamp);

	for (uint32_t sectorIndex = 0; sectorIndex < sectorCount; ++sectorIndex)
	{
		if (sectorIndex >= seenSectors.size() || seenSectors[sectorIndex] == 0u)
		{
			continue;
		}

		mSectorLighting.eligibleSectorCount++;

		const auto& sec = sector[sectorIndex];
		const int resolvedPalette = sec.floorpal != 0 ? (int)sec.floorpal : (int)sec.ceilingpal;
		const int averageShade = ((int)sec.floorshade + (int)sec.ceilingshade) / 2;
		if ((paletteFilter >= 0 && resolvedPalette != paletteFilter) ||
			(averageShade < minShadeFilter || averageShade > maxShadeFilter) ||
			(lotagFilter >= 0 && sec.lotag != lotagFilter))
		{
			continue;
		}

		const float lightLevel = ComputeBuildLightLevel(averageShade, resolvedPalette);
		const float floorLight = ComputeBuildLightLevel((int)sec.floorshade, resolvedPalette);
		const float ceilingLight = ComputeBuildLightLevel((int)sec.ceilingshade, resolvedPalette);
		const float hemisphereBias = clamp(ceilingLight - floorLight, -1.0f, 1.0f);
		float tint[3] = {};
		float fogStrength = 0.0f;
		ResolveSectorTint(sec, resolvedPalette, tint, fogStrength);
		const bool sectorPulseEnabled = pulseSelectionFiltered && pulseFrames > 1 && pulseAmount > 0.0f;
		const float pulseScale = sectorPulseEnabled ? EvaluatePulseScale(0x5EC70B5E00000000ull ^ (uint64_t)sectorIndex, frameIndex, pulseFrames, pulseAmount) : 1.0f;
		const float clampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + lightLevel * 0.55f) * pulseScale);
		const float clampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(hemisphereBias)) * 0.45f) * pulseScale);
		const float clampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		if (clampedAmbient <= 0.0f && clampedHemisphere <= 0.0f && clampedFog <= 0.0f)
		{
			continue;
		}

		SectorLightingRegistry::SectorLightRecord entry = {};
		entry.sectorIndex = sectorIndex;
		entry.sourceFlags = SceneSectorLightSourceFlag_Heuristic;
		if (paletteFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_PaletteFilter;
		}
		if (lotagFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_LotagFilter;
		}
		if (fogStrength > 0.0f)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_FogPresent;
			mSectorLighting.fogSectorCount++;
		}
		if (sectorPulseEnabled)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_Pulsing;
			mSectorLighting.pulsingSectorCount++;
		}

		entry.paletteIndex = resolvedPalette;
		entry.lotag = sec.lotag;
		entry.hitag = sec.hitag;
		entry.averageShade = averageShade;
		entry.ambientColor[0] = tint[0];
		entry.ambientColor[1] = tint[1];
		entry.ambientColor[2] = tint[2];
		entry.ambientIntensity = clampedAmbient;
		entry.hemisphereAmount = hemisphereBias * clampedHemisphere;
		entry.fogAmount = clampedFog;
		entry.pulseScale = pulseScale;

		mSectorLighting.sectors[sectorIndex] = entry;
		mSectorLighting.activeSectorIndices.push_back(sectorIndex);
	}

	mSectorLighting.activeSectorCount = (uint32_t)mSectorLighting.activeSectorIndices.size();
	std::vector<uint32_t> nextTopologyKeys = mSectorLighting.activeSectorIndices;
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mSectorLighting.topologyChanged = nextTopologyKeys != mSectorLighting.activeTopologyKeys;
	mSectorLighting.activeTopologyKeys = std::move(nextTopologyKeys);
}

bool SceneLightSystem::AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	SceneAnalyticLight light = {};
	light.id = id;
	light.stableKey = 0x4d414e55414c0000ull | (uint64_t)id;
	light.sourceFlags = SceneAnalyticLightSourceFlag_Manual;
	light.textureId = 0;
	light.position[0] = position[0];
	light.position[1] = position[1];
	light.position[2] = position[2];
	light.color[0] = std::max(color[0], 0.0f);
	light.color[1] = std::max(color[1], 0.0f);
	light.color[2] = std::max(color[2], 0.0f);
	light.intensity = intensity;
	light.radius = radius;
	mAnalyticLights.manualLights.push_back(light);
	return true;
}

bool SceneLightSystem::RemoveManualAnalyticLight(uint32_t id)
{
	const auto it = std::find_if(mAnalyticLights.manualLights.begin(), mAnalyticLights.manualLights.end(), [id](const SceneAnalyticLight& light)
	{
		return light.id == id;
	});
	if (it == mAnalyticLights.manualLights.end())
	{
		return false;
	}

	mAnalyticLights.manualLights.erase(it);
	return true;
}

void SceneLightSystem::ClearManualAnalyticLights()
{
	mAnalyticLights.manualLights.clear();
}

void SceneLightSystem::SetTransientAnalyticLights(const std::vector<SceneAnalyticLight>& lights)
{
	mAnalyticLights.transientLights = lights;
	mAnalyticLights.transientMuzzleSlotCount = (uint32_t)lights.size();
	mAnalyticLights.transientMuzzleActiveCount = 0;
	for (const SceneAnalyticLight& light : lights)
	{
		if ((light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 &&
			light.intensity > 0.0f &&
			light.radius > 0.0f)
		{
			mAnalyticLights.transientMuzzleActiveCount++;
		}
	}
}

bool SceneLightSystem::AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	AnalyticLightHeuristicRule rule = {};
	rule.ruleId = mAnalyticLights.nextRuleId++;
	rule.textureId = textureId;
	rule.color[0] = std::max(color[0], 0.0f);
	rule.color[1] = std::max(color[1], 0.0f);
	rule.color[2] = std::max(color[2], 0.0f);
	rule.intensity = intensity;
	rule.radius = radius;
	rule.flickerFrames = flickerFrames;
	mAnalyticLights.spriteTileRules.push_back(rule);
	outRuleId = rule.ruleId;
	return true;
}

void SceneLightSystem::ClearSpriteTileHeuristics()
{
	mAnalyticLights.spriteTileRules.clear();
}

bool SceneLightSystem::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (textureId == 0 || intensityScale <= 0.0f || emissiveMode == nri_scene::MaterialEmissiveMode_None)
	{
		return false;
	}

	EmissiveSurfaceRegistry::EmissiveHeuristicRule rule = {};
	rule.ruleId = mEmissiveSurfaces.nextRuleId++;
	rule.textureId = textureId;
	rule.emissiveMode = emissiveMode;
	rule.intensityScale = intensityScale;
	rule.hasExplicitColor = hasExplicitColor && emissiveColor != nullptr;
	if (rule.hasExplicitColor)
	{
		rule.emissiveColor[0] = std::max(emissiveColor[0], 0.0f);
		rule.emissiveColor[1] = std::max(emissiveColor[1], 0.0f);
		rule.emissiveColor[2] = std::max(emissiveColor[2], 0.0f);
	}
	mEmissiveSurfaces.textureRules.push_back(rule);
	mEmissiveSurfaces.materialBindingChanged = true;
	mEmissiveSurfaces.materialPropertiesChanged = true;
	outRuleId = rule.ruleId;
	return true;
}

void SceneLightSystem::ClearTextureEmissiveHeuristics()
{
	if (mEmissiveSurfaces.textureRules.empty())
	{
		return;
	}

	mEmissiveSurfaces.textureRules.clear();
	mEmissiveSurfaces.materialBindingChanged = true;
	mEmissiveSurfaces.materialPropertiesChanged = true;
}

bool SceneLightSystem::MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const
{
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	return EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale);
}

bool SceneLightSystem::ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const
{
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
	{
		inOutMaterial.emissiveColor[0] = 0.0f;
		inOutMaterial.emissiveColor[1] = 0.0f;
		inOutMaterial.emissiveColor[2] = 0.0f;
		inOutMaterial.emissiveIntensity = 0.0f;
		inOutMaterial.emissiveMaskScale = 0.0f;
		inOutMaterial.emissiveMode = nri_scene::MaterialEmissiveMode_None;
		inOutMaterial.emissiveTextureIndex = UINT32_MAX;
		return false;
	}

	inOutMaterial.emissiveColor[0] = emissiveColor[0];
	inOutMaterial.emissiveColor[1] = emissiveColor[1];
	inOutMaterial.emissiveColor[2] = emissiveColor[2];
	inOutMaterial.emissiveIntensity = emissiveIntensity;
	inOutMaterial.emissiveMaskScale = std::max(emissiveFalloffScale, 0.0f);
	inOutMaterial.emissiveMode = emissiveMode;
	inOutMaterial.emissiveTextureIndex = emissiveTextureIndex;
	if (inOutMaterial.materialClass != 3u)
	{
		inOutMaterial.materialClass = 2u;
	}
	return true;
}

bool SceneLightSystem::ConsumeAnalyticLightTopologyChanged()
{
	const bool changed = mAnalyticLights.topologyChanged;
	mAnalyticLights.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeAnalyticLightPropertiesChanged()
{
	const bool changed = mAnalyticLights.propertiesChanged;
	mAnalyticLights.propertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveSurfaceTopologyChanged()
{
	const bool changed = mEmissiveSurfaces.topologyChanged;
	mEmissiveSurfaces.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveSurfacePropertiesChanged()
{
	const bool changed = mEmissiveSurfaces.propertiesChanged;
	mEmissiveSurfaces.propertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveMaterialBindingChanged()
{
	const bool changed = mEmissiveSurfaces.materialBindingChanged;
	mEmissiveSurfaces.materialBindingChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveMaterialPropertiesChanged()
{
	const bool changed = mEmissiveSurfaces.materialPropertiesChanged;
	mEmissiveSurfaces.materialPropertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeSectorLightingTopologyChanged()
{
	const bool changed = mSectorLighting.topologyChanged;
	mSectorLighting.topologyChanged = false;
	return changed;
}

void SceneLightSystem::AppendSurfaceList(
	const std::vector<nri_scene::SurfaceRef>& surfaces,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	uint32_t& inOutLocalMaterialIndex,
	const std::vector<uint64_t>* identityOverrides)
{
	for (size_t surfaceIndex = 0; surfaceIndex < surfaces.size(); ++surfaceIndex)
	{
		const nri_scene::SurfaceRef& surface = surfaces[surfaceIndex];
		SurfaceRecord record = {};
		record.source = source;
		record.materialIndex = materialIndexBase + inOutLocalMaterialIndex;
		record.provenance = surface.provenance;
		ComputeSurfaceBounds(surface, record.center, record.boundsRadius);
		record.surfaceArea = ComputeSurfaceArea(surface);

		const uint32_t materialLookupIndex = materialLookupIndexBase + inOutLocalMaterialIndex;
		if (materialLookupIndex < materials.lightMetadata.size())
		{
			record.material = materials.lightMetadata[materialLookupIndex];
		}
		else if (materialLookupIndex < materials.materials.size())
		{
			record.material.sectorIndex = materials.materials[materialLookupIndex].sectorIndex != UINT32_MAX ? (int32_t)materials.materials[materialLookupIndex].sectorIndex : -1;
			record.material.paletteIndex = materials.materials[materialLookupIndex].paletteIndex;
			record.material.materialFlags = materials.materials[materialLookupIndex].flags;
			record.material.alpha = materials.materials[materialLookupIndex].alpha;
			record.material.lightLevel = materials.materials[materialLookupIndex].lightLevel;
		}

		const uint64_t inheritedIdentityKey =
			identityOverrides != nullptr && surfaceIndex < identityOverrides->size() ?
			(*identityOverrides)[surfaceIndex] :
			0ull;
		record.identityKey =
			inheritedIdentityKey != 0ull ?
			inheritedIdentityKey :
			BuildSurfaceIdentityKey(record);

		mSurfaceRecords.push_back(record);
		mFrameAppendStats.totalRecordCount++;
		switch (source)
		{
		case SceneLightRecordSource::StaticMapScene:
			mFrameAppendStats.staticRecordCount++;
			break;
		case SceneLightRecordSource::RuntimeMutationScene:
			mFrameAppendStats.runtimeMutationRecordCount++;
			break;
		case SceneLightRecordSource::DynamicScene:
			mFrameAppendStats.dynamicRecordCount++;
			break;
		case SceneLightRecordSource::CapturedScene:
			mFrameAppendStats.capturedRecordCount++;
			break;
		case SceneLightRecordSource::PersistentVoxelScene:
			mFrameAppendStats.persistentVoxelRecordCount++;
			break;
		default:
			break;
		}
		++inOutLocalMaterialIndex;
	}
}
