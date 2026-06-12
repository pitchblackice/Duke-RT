#include "nri_scene_lights.h"

#include "c_cvars.h"
#include "gamefuncs.h"
#include "maptypes.h"
#include "palette.h"
#include "printf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

EXTERN_CVAR(Float, nri_ptemissiveminpower)
EXTERN_CVAR(Float, nri_ptemissiveminsurface)
EXTERN_CVAR(Float, nri_ptglowscale)
EXTERN_CVAR(Float, nri_ptglowreach)
EXTERN_CVAR(Float, nri_ptglowfalloff)
EXTERN_CVAR(Float, nri_ptglowblend)
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
EXTERN_CVAR(Float, nri_ptsectoremissionsignalstrength)
EXTERN_CVAR(Float, nri_ptsectoremissionresponsemin)
EXTERN_CVAR(Float, nri_ptsectoremissionresponsemax)
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
		case nri_scene::SurfaceSourceType::SurfaceLightOverlay: return "surface_light_overlay";
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

	const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	const char* GetMaterialEmissiveModeName(uint32_t mode)
	{
		switch (mode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: return "base";
		case nri_scene::MaterialEmissiveMode_UseConstantColor: return "constant";
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: return "glowmap";
		default: return "none";
		}
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

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
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
		hash = HashCombine64(hash, (uint64_t)light.flags);
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

	float ComputeSectorEmitterResponseScale(float brightness, float neutralBrightness, float intensity, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		if (neutralBrightness <= 0.0001f || intensity <= 0.0f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float normalizedDelta = (brightness - neutralBrightness) / neutralBrightness;
		return clamp(1.0f + normalizedDelta * intensity, clampedMin, clampedMax);
	}

	float ComputeSectorEmitterRangeResponseScale(float signal, float inputMin, float inputMax, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		const float inputRange = inputMax - inputMin;
		if (std::abs(inputRange) <= 0.0001f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float t = clamp((signal - inputMin) / inputRange, 0.0f, 1.0f);
		return clampedMin + (clampedMax - clampedMin) * t;
	}

	bool IsGlowDrivenEmissive(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		if (emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			return true;
		}

		return (sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	float ResolveGlowStrengthScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max(settings.glowScale, 0.0f) : 1.0f;
	}

	float ResolveGlowSamplingScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max(settings.glowReach, 0.0f) : 1.0f;
	}

	float ResolveGlowFalloffScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::clamp(settings.glowFalloff, 0.25f, 4.0f) : 1.0f;
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
		hash = HashCombine64(hash, (uint64_t)emissive.overrideRuleId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.sectorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.authoredSectorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.wallIndex);
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
		hash = HashQuantizedFloat(hash, emissive.reachScale, 4096.0f);
		hash = HashCombine64(hash, emissive.hasSectorResponseParams ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseIntensity, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseMax, 4096.0f);
		hash = HashCombine64(hash, emissive.hasSectorResponseInputRange ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseInputMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseInputMax, 4096.0f);
		hash = HashCombine64(hash, emissive.materialResponseEnabled ? 1ull : 0ull);
		hash = HashCombine64(hash, emissive.materialResponseExplicit ? 1ull : 0ull);
		hash = HashCombine64(hash, emissive.hasMaterialResponseParams ? 1ull : 0ull);
		hash = HashCombine64(hash, emissive.hasMaterialResponseMin ? 1ull : 0ull);
		hash = HashCombine64(hash, emissive.hasMaterialResponseMax ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.materialResponseMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.materialResponseMax, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.powerEstimate, 256.0f);
		hash = HashCombine64(hash, emissive.sectorResponseEnabled ? 1ull : 0ull);
		return hash;
	}

	uint64_t BuildEmissiveBindingHash(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)emissive.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)emissive.sourceRuleId);
		hash = HashCombine64(hash, (uint64_t)emissive.overrideRuleId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.sectorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.authoredSectorIndex);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)emissive.wallIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.textureId);
		hash = HashCombine64(hash, (uint64_t)emissive.materialIndex);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveMode);
		hash = HashCombine64(hash, (uint64_t)emissive.emissiveTextureIndex);
		return hash;
	}

	bool EmissiveOverrideMatchesSurface(
		const SceneLightSystem::EmissiveOverrideRule& rule,
		const SceneLightSystem::SurfaceRecord& record)
	{
		if (rule.hasSectorFilter && record.provenance.sectorIndex != rule.sectorFilter)
		{
			return false;
		}
		if (rule.hasWallFilter && record.provenance.wallIndex != rule.wallFilter)
		{
			return false;
		}
		if (rule.hasTileFilter && record.material.textureId != rule.tileFilter)
		{
			return false;
		}
		return rule.hasSectorFilter || rule.hasWallFilter || rule.hasTileFilter;
	}

	std::string NormalizeMaterialTextureName(const FGameTexture* texture)
	{
		std::string normalized = texture != nullptr ? texture->GetName().GetChars() : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	bool EmissiveMaterialResponseRuleMatchesSurface(
		const SceneLightSystem::EmissiveMaterialResponseRule& rule,
		const SceneLightSystem::SurfaceRecord& record)
	{
		for (uint32_t textureId : rule.textureIds)
		{
			if (record.material.textureId == textureId)
			{
				return true;
			}
		}

		for (const auto& range : rule.textureRanges)
		{
			if (record.material.textureId >= range.first && record.material.textureId <= range.second)
			{
				return true;
			}
		}

		if (!rule.textureNames.empty())
		{
			const std::string textureName = NormalizeMaterialTextureName(record.material.texture);
			for (const std::string& ruleTextureName : rule.textureNames)
			{
				if (textureName == ruleTextureName)
				{
					return true;
				}
			}
		}

		return false;
	}

	void ApplyEmissiveMaterialResponseRule(
		const SceneLightSystem::EmissiveMaterialResponseRule& rule,
		SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		if (rule.hasMaterialResponse)
		{
			emissive.materialResponseEnabled = rule.materialResponse;
			emissive.materialResponseExplicit = true;
		}
		else
		{
			emissive.materialResponseEnabled = true;
		}
		if (rule.hasMaterialResponseMin || rule.hasMaterialResponseMax)
		{
			if (!rule.hasMaterialResponse)
			{
				emissive.materialResponseEnabled = true;
			}
			emissive.hasMaterialResponseParams = true;
			emissive.hasMaterialResponseMin = rule.hasMaterialResponseMin;
			emissive.hasMaterialResponseMax = rule.hasMaterialResponseMax;
			if (rule.hasMaterialResponseMin)
			{
				emissive.materialResponseMin = std::max(0.0f, rule.materialResponseMin);
			}
			if (rule.hasMaterialResponseMax)
			{
				emissive.materialResponseMax = std::max(0.0f, rule.materialResponseMax);
			}
		}
	}

	void ApplyEmissiveOverrideRule(
		const SceneLightSystem::EmissiveOverrideRule& rule,
		SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive,
		const NRILightingSettings& settings)
	{
		emissive.sourceFlags |= SceneEmissiveSurfaceSourceFlag_LightOverlayOverride;
		emissive.overrideRuleId = rule.ruleId;
		if (rule.hasIntensityScale)
		{
			emissive.emissiveIntensity *= std::max(rule.intensityScale, 0.0f);
		}
		if (rule.hasReachScale)
		{
			emissive.reachScale *= std::max(rule.reachScale, 0.0f);
		}
		if (rule.hasSectorResponse)
		{
			emissive.sectorResponseEnabled = rule.sectorResponse;
		}
		if (rule.hasSignalSector)
		{
			emissive.sectorIndex = rule.signalSector;
			if (!rule.hasSectorResponse)
			{
				emissive.sectorResponseEnabled = true;
			}
		}
		if (rule.hasResponseIntensity || rule.hasResponseMin || rule.hasResponseMax || rule.hasResponseInputMin || rule.hasResponseInputMax)
		{
			emissive.hasSectorResponseParams = true;
			emissive.sectorResponseIntensity = rule.hasResponseIntensity ? rule.responseIntensity : std::max(0.0f, settings.sectorEmissionSignalStrength);
			emissive.sectorResponseMin = rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin);
			emissive.sectorResponseMax = rule.hasResponseMax ? rule.responseMax : std::max(emissive.sectorResponseMin, settings.sectorEmissionResponseMax);
			emissive.hasSectorResponseInputRange = rule.hasResponseInputMin && rule.hasResponseInputMax;
			emissive.sectorResponseInputMin = rule.hasResponseInputMin ? rule.responseInputMin : 0.0f;
			emissive.sectorResponseInputMax = rule.hasResponseInputMax ? rule.responseInputMax : 1.0f;
		}
		if (rule.hasResponseIntensityMin)
		{
			emissive.hasSectorResponseIntensityMin = true;
			emissive.sectorResponseIntensityMin = rule.responseIntensityMin;
		}
		if (rule.hasResponseIntensityMax)
		{
			emissive.hasSectorResponseIntensityMax = true;
			emissive.sectorResponseIntensityMax = rule.responseIntensityMax;
		}
		if (rule.hasResponseReachMin)
		{
			emissive.hasSectorResponseReachMin = true;
			emissive.sectorResponseReachMin = rule.responseReachMin;
		}
		if (rule.hasResponseReachMax)
		{
			emissive.hasSectorResponseReachMax = true;
			emissive.sectorResponseReachMax = rule.responseReachMax;
		}
		if (rule.hasMaterialResponse)
		{
			emissive.materialResponseEnabled = rule.materialResponse;
			emissive.materialResponseExplicit = true;
		}
		if (rule.hasMaterialResponseMin || rule.hasMaterialResponseMax)
		{
			if (!rule.hasMaterialResponse)
			{
				emissive.materialResponseEnabled = true;
			}
			emissive.hasMaterialResponseParams = true;
			emissive.hasMaterialResponseMin = rule.hasMaterialResponseMin;
			emissive.hasMaterialResponseMax = rule.hasMaterialResponseMax;
			if (rule.hasMaterialResponseMin)
			{
				emissive.materialResponseMin = std::max(0.0f, rule.materialResponseMin);
			}
			if (rule.hasMaterialResponseMax)
			{
				emissive.materialResponseMax = std::max(0.0f, rule.materialResponseMax);
			}
		}
	}

	bool EvaluateEmissiveMaterial(
		const SceneLightSystem::EmissiveSurfaceRegistry& registry,
		const nri_scene::MaterialLightingMetadata& metadata,
		const NRILightingSettings& settings,
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

		outIntensity *= ResolveGlowStrengthScale(outSourceFlags, outMode, settings);
		outSamplingScale = ResolveGlowSamplingScale(outSourceFlags, outMode, settings);
		outFalloffScale = ResolveGlowFalloffScale(outSourceFlags, outMode, settings);
		return outIntensity > 0.0f;
	}
}

NRILightingSettings SceneLightSystem::CaptureSettings()
{
	NRILightingSettings settings = {};
	settings.emissiveMinPower = (float)nri_ptemissiveminpower;
	settings.emissiveMinSurface = (float)nri_ptemissiveminsurface;
	settings.glowScale = (float)nri_ptglowscale;
	settings.glowReach = (float)nri_ptglowreach;
	settings.glowFalloff = (float)nri_ptglowfalloff;
	settings.sectorLighting = !!nri_ptsectorlighting;
	settings.sectorAmbientScale = (float)nri_ptsectorambientscale;
	settings.sectorHemisphereScale = (float)nri_ptsectorhemiscale;
	settings.sectorFogScale = (float)nri_ptsectorfogscale;
	settings.sectorClamp = (float)nri_ptsectorclamp;
	settings.sectorFilterPalette = (int)nri_ptsectorfilterpal;
	settings.sectorFilterMinShade = (int)nri_ptsectorfilterminshade;
	settings.sectorFilterMaxShade = (int)nri_ptsectorfiltermaxshade;
	settings.sectorFilterLotag = (int)nri_ptsectorfilterlotag;
	settings.sectorPulseFrames = (int)nri_ptsectorpulseframes;
	settings.sectorPulseAmount = (float)nri_ptsectorpulseamount;
	settings.sectorEmissionSignalStrength = (float)nri_ptsectoremissionsignalstrength;
	settings.sectorEmissionResponseMin = (float)nri_ptsectoremissionresponsemin;
	settings.sectorEmissionResponseMax = (float)nri_ptsectoremissionresponsemax;
	return settings;
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
	mEmissiveSurfaces.overrideRuleCount = 0;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = 0;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
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
	mEmissiveSurfaces.overrideRuleCount = 0;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = 0;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
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

SceneLightSystem::SurfaceRecord SceneLightSystem::BuildSurfaceRecord(
	const nri_scene::SurfaceRef& surface,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndex,
	uint32_t materialLookupIndex,
	uint64_t identityOverride) const
{
	SurfaceRecord record = {};
	record.source = source;
	record.materialIndex = materialIndex;
	record.provenance = surface.provenance;
	ComputeSurfaceBounds(surface, record.center, record.boundsRadius);
	record.surfaceArea = ComputeSurfaceArea(surface);

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

	record.identityKey = identityOverride != 0ull ? identityOverride : BuildSurfaceIdentityKey(record);
	return record;
}

void SceneLightSystem::AppendSurfaceRecords(
	const std::vector<SurfaceRecord>& records,
	uint32_t materialIndexBase)
{
	for (SurfaceRecord record : records)
	{
		AppendSurfaceRecord(record, materialIndexBase);
	}
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
	const NRILightingSettings settings = CaptureSettings();
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
				light.flags = rule.flags;
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
			float sectorScale = 1.0f;
			const int32_t signalSector = rule.hasSignalSector ? rule.signalSector : -1;
			const bool sectorResponseEnabled = rule.hasSectorResponse ? rule.sectorResponse : false;
			if (sectorResponseEnabled && signalSector >= 0 && (uint32_t)signalSector < mSectorLighting.sectors.size())
			{
				const auto& sectorRecord = mSectorLighting.sectors[(uint32_t)signalSector];
				if (rule.hasResponseInputMin && rule.hasResponseInputMax)
				{
					sectorScale = ComputeSectorEmitterRangeResponseScale(
						sectorRecord.rawResponseSignal,
						rule.responseInputMin,
						rule.responseInputMax,
						rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin),
						rule.hasResponseMax ? rule.responseMax : std::max(std::max(0.0f, settings.sectorEmissionResponseMin), settings.sectorEmissionResponseMax));
				}
				else if (rule.hasResponseIntensity || rule.hasResponseMin || rule.hasResponseMax)
				{
					const float responseMin = rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin);
					sectorScale = ComputeSectorEmitterResponseScale(
						sectorRecord.rawResponseBrightness,
						std::min(std::max(0.0f, settings.sectorClamp), std::max(0.0f, settings.sectorAmbientScale) * (0.10f + 0.75f * 0.55f)),
						rule.hasResponseIntensity ? rule.responseIntensity : std::max(0.0f, settings.sectorEmissionSignalStrength),
						responseMin,
						rule.hasResponseMax ? rule.responseMax : std::max(responseMin, settings.sectorEmissionResponseMax));
				}
				else
				{
					sectorScale = std::max(0.0f, sectorRecord.emitterResponseScale);
				}
			}
			light.intensity = rule.intensity * sectorScale * EvaluateFlickerScale(light.stableKey, flickerTimeIndex, rule.flickerFrames);
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

void SceneLightSystem::RebuildEmissiveSurfaces(
	uint32_t maxActiveSurfaces,
	const std::vector<EmissiveOverrideRule>* overrideRules,
	const std::vector<EmissiveMaterialResponseRule>* materialResponseRules)
{
	const NRILightingSettings settings = CaptureSettings();
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.overrideRuleCount = overrideRules != nullptr ? (uint32_t)overrideRules->size() : 0u;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = materialResponseRules != nullptr ? (uint32_t)materialResponseRules->size() : 0u;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;

	std::vector<EmissiveSurfaceRegistry::EmissiveSurfaceRecord> nextSurfaces;
	nextSurfaces.reserve(std::min<uint32_t>((uint32_t)mSurfaceRecords.size(), maxActiveSurfaces));

	const float minSurfaceArea = std::max(settings.emissiveMinSurface, 0.0f);
	const float minPower = std::max(settings.emissiveMinPower, 0.0f);

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
		if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, record.material, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
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

		EmissiveSurfaceRegistry::EmissiveSurfaceRecord emissive = {};
		emissive.stableKey = BuildEmissiveTopologyKey(record);
		emissive.sourceFlags = sourceFlags;
		emissive.sourceRuleId = sourceRuleId;
		emissive.source = record.source;
		emissive.actorIndex = record.provenance.actorIndex;
		emissive.sectorIndex = record.provenance.sectorIndex;
		emissive.authoredSectorIndex = record.provenance.sectorIndex;
		emissive.wallIndex = record.provenance.wallIndex;
		emissive.textureId = record.material.textureId;
		emissive.emissiveTextureIndex = emissiveTextureIndex;
		emissive.materialIndex = record.materialIndex;
		emissive.emissiveMode = emissiveMode;
		emissive.surfaceArea = record.surfaceArea;
		emissive.boundsRadius = record.boundsRadius;
		Copy3f(record.center, emissive.center);
		Copy3f(emissiveColor, emissive.emissiveColor);
		emissive.emissiveIntensity = emissiveIntensity;
		bool matchedMaterialResponse = false;
		if (materialResponseRules != nullptr)
		{
			for (const EmissiveMaterialResponseRule& rule : *materialResponseRules)
			{
				if (EmissiveMaterialResponseRuleMatchesSurface(rule, record))
				{
					ApplyEmissiveMaterialResponseRule(rule, emissive);
					matchedMaterialResponse = true;
				}
			}
		}
		bool matchedOverride = false;
		if (overrideRules != nullptr)
		{
			for (const EmissiveOverrideRule& rule : *overrideRules)
			{
				if (EmissiveOverrideMatchesSurface(rule, record))
				{
					ApplyEmissiveOverrideRule(rule, emissive, settings);
					matchedOverride = true;
				}
			}
		}
		emissive.powerEstimate = record.surfaceArea * resolvedLuminance * emissive.emissiveIntensity;
		if (emissive.powerEstimate < minPower)
		{
			continue;
		}

		if (nextSurfaces.size() >= maxActiveSurfaces)
		{
			mEmissiveSurfaces.truncatedSurfaceCount++;
			continue;
		}
		if (matchedOverride)
		{
			mEmissiveSurfaces.overrideMatchedSurfaceCount++;
		}
		if (matchedMaterialResponse)
		{
			mEmissiveSurfaces.materialResponseMatchedSurfaceCount++;
		}
		nextSurfaces.push_back(emissive);

		if ((sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoFullbright | SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0)
		{
			mEmissiveSurfaces.autoTaggedCount++;
		}
		if ((sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) != 0)
		{
			mEmissiveSurfaces.explicitRuleMatchCount++;
		}
		mEmissiveSurfaces.totalPowerEstimate += emissive.powerEstimate;
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
	const NRILightingSettings settings = CaptureSettings();
	mSectorLighting.sectorCount = sectorCount;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.rawActiveSectorCount = 0;
	mSectorLighting.rawNonNeutralSectorCount = 0;
	mSectorLighting.responseBoostSectorCount = 0;
	mSectorLighting.responseDimSectorCount = 0;
	mSectorLighting.responseNeutralSectorCount = 0;
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
	mSectorLighting.rawActiveSectorIndices.clear();
	mSectorLighting.rawActiveSectorIndices.reserve(sectorCount);

	if (!settings.sectorLighting || sectorCount == 0)
	{
		mSectorLighting.topologyChanged = !mSectorLighting.activeTopologyKeys.empty();
		mSectorLighting.activeTopologyKeys.clear();
		return;
	}

	const int paletteFilter = settings.sectorFilterPalette;
	const int minShadeFilter = settings.sectorFilterMinShade;
	const int maxShadeFilter = std::max(minShadeFilter, settings.sectorFilterMaxShade);
		const int lotagFilter = settings.sectorFilterLotag;
		const uint32_t pulseFrames = std::max(0, settings.sectorPulseFrames);
		const float pulseAmount = std::max(0.0f, settings.sectorPulseAmount);
		const bool pulseSelectionFiltered =
			paletteFilter >= 0 ||
			lotagFilter >= 0 ||
			minShadeFilter > -128 ||
			maxShadeFilter < 127;
		const float ambientScale = std::max(0.0f, settings.sectorAmbientScale);
		const float hemisphereScale = std::max(0.0f, settings.sectorHemisphereScale);
		const float fogScale = std::max(0.0f, settings.sectorFogScale);
	const float sectorClamp = std::max(0.0f, settings.sectorClamp);
	const float responseIntensity = std::max(0.0f, settings.sectorEmissionSignalStrength);
	const float responseMin = std::max(0.0f, settings.sectorEmissionResponseMin);
	const float responseMax = std::max(responseMin, settings.sectorEmissionResponseMax);
	const float neutralAmbient = std::min(sectorClamp, ambientScale * (0.10f + 0.75f * 0.55f));

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
		const int rawFloorShade = (int)sec.floorshade;
		const int rawCeilingShade = (int)sec.ceilingshade;
		const float rawLightLevel = ComputeBuildLightLevel(averageShade, resolvedPalette);
		const float rawFloorLight = ComputeBuildLightLevel(rawFloorShade, resolvedPalette);
		const float rawCeilingLight = ComputeBuildLightLevel(rawCeilingShade, resolvedPalette);
		const float rawHemisphereBias = clamp(rawCeilingLight - rawFloorLight, -1.0f, 1.0f);
		const int lightingAverageShade = 0;
		const int lightingFloorShade = 0;
		const int lightingCeilingShade = 0;
		float tint[3] = {};
		float fogStrength = 0.0f;
		ResolveSectorTint(sec, resolvedPalette, tint, fogStrength);
		const bool sectorPulseEnabled = pulseSelectionFiltered && pulseFrames > 1 && pulseAmount > 0.0f;
		const float pulseScale = sectorPulseEnabled ? EvaluatePulseScale(0x5EC70B5E00000000ull ^ (uint64_t)sectorIndex, frameIndex, pulseFrames, pulseAmount) : 1.0f;
		const float rawClampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + rawLightLevel * 0.55f) * pulseScale);
		const float rawClampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(rawHemisphereBias)) * 0.45f) * pulseScale);
		const float rawClampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		const float rawHemisphereAmount = rawHemisphereBias * rawClampedHemisphere;
		const float rawResponseBrightness = rawClampedAmbient + std::abs(rawHemisphereAmount);
		const float rawResponseSignal = clamp(rawLightLevel + std::abs(rawHemisphereBias), 0.0f, 1.0f);
		const float emitterResponseScale = ComputeSectorEmitterResponseScale(rawResponseBrightness, neutralAmbient, responseIntensity, responseMin, responseMax);

		SectorLightingRegistry::SectorLightRecord entry = {};
		entry.sectorIndex = sectorIndex;
		entry.paletteIndex = resolvedPalette;
		entry.lotag = sec.lotag;
		entry.hitag = sec.hitag;
		entry.averageShade = averageShade;
		entry.rawAverageShade = averageShade;
		entry.rawLightLevel = rawLightLevel;
		entry.rawFloorLight = rawFloorLight;
		entry.rawCeilingLight = rawCeilingLight;
		entry.rawAmbientIntensity = rawClampedAmbient;
		entry.rawHemisphereAmount = rawHemisphereAmount;
		entry.rawFogAmount = rawClampedFog;
		entry.rawResponseBrightness = rawResponseBrightness;
		entry.rawResponseSignal = rawResponseSignal;
		entry.emitterResponseScale = emitterResponseScale;
		entry.ambientColor[0] = tint[0];
		entry.ambientColor[1] = tint[1];
		entry.ambientColor[2] = tint[2];
		entry.pulseScale = pulseScale;

		const bool rawActive = rawClampedAmbient > 0.0f || rawClampedHemisphere > 0.0f || rawClampedFog > 0.0f;
		if (rawActive)
		{
			mSectorLighting.rawActiveSectorIndices.push_back(sectorIndex);
		}
		if (averageShade != 0 || rawFloorShade != 0 || rawCeilingShade != 0)
		{
			mSectorLighting.rawNonNeutralSectorCount++;
		}
		if (emitterResponseScale > 1.01f)
		{
			mSectorLighting.responseBoostSectorCount++;
		}
		else if (emitterResponseScale < 0.99f)
		{
			mSectorLighting.responseDimSectorCount++;
		}
		else
		{
			mSectorLighting.responseNeutralSectorCount++;
		}
		mSectorLighting.sectors[sectorIndex] = entry;

		if ((paletteFilter >= 0 && resolvedPalette != paletteFilter) ||
			(lightingAverageShade < minShadeFilter || lightingAverageShade > maxShadeFilter) ||
			(lotagFilter >= 0 && sec.lotag != lotagFilter))
		{
			continue;
		}

		const float lightLevel = ComputeBuildLightLevel(lightingAverageShade, resolvedPalette);
		const float floorLight = ComputeBuildLightLevel(lightingFloorShade, resolvedPalette);
		const float ceilingLight = ComputeBuildLightLevel(lightingCeilingShade, resolvedPalette);
		const float hemisphereBias = clamp(ceilingLight - floorLight, -1.0f, 1.0f);
		const float clampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + lightLevel * 0.55f) * pulseScale);
		const float clampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(hemisphereBias)) * 0.45f) * pulseScale);
		const float clampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		if (clampedAmbient <= 0.0f && clampedHemisphere <= 0.0f && clampedFog <= 0.0f)
		{
			continue;
		}

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

		entry.ambientIntensity = clampedAmbient;
		entry.hemisphereAmount = hemisphereBias * clampedHemisphere;
		entry.fogAmount = clampedFog;

		mSectorLighting.sectors[sectorIndex] = entry;
		mSectorLighting.activeSectorIndices.push_back(sectorIndex);
	}

	mSectorLighting.rawActiveSectorCount = (uint32_t)mSectorLighting.rawActiveSectorIndices.size();
	mSectorLighting.activeSectorCount = (uint32_t)mSectorLighting.activeSectorIndices.size();
	std::vector<uint32_t> nextTopologyKeys = mSectorLighting.activeSectorIndices;
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mSectorLighting.topologyChanged = nextTopologyKeys != mSectorLighting.activeTopologyKeys;
	mSectorLighting.activeTopologyKeys = std::move(nextTopologyKeys);
}

void SceneLightSystem::BuildRuntimePointLightUpload(std::vector<NRIRuntimePointLightGpuData>& outLights) const
{
	const auto& activeLights = mAnalyticLights.activeLights;
	outLights.clear();
	outLights.reserve(activeLights.size());
	for (const SceneAnalyticLight& light : activeLights)
	{
		NRIRuntimePointLightGpuData gpuLight = {};
		Copy3f(light.position, gpuLight.position);
		gpuLight.radius = light.radius;
		Copy3f(light.color, gpuLight.color);
		gpuLight.intensity = light.intensity;
		gpuLight.flags = light.flags;
		outLights.push_back(gpuLight);
	}
}

uint64_t SceneLightSystem::BuildRuntimeLightPayloadHash() const
{
	const auto& activeLights = mAnalyticLights.activeLights;
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, (uint64_t)activeLights.size());
	for (const SceneAnalyticLight& light : activeLights)
	{
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.position[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.color[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.intensity));
		hash = HashCombine64(hash, (uint64_t)FloatBits(light.radius));
		hash = HashCombine64(hash, (uint64_t)light.flags);
	}

	return hash;
}

void SceneLightSystem::BuildSectorLightingUpload(
	float sectorLightMultiplier,
	bool sectorLightingEnabled,
	NRISectorLightHeaderGpuData& outHeader,
	std::vector<NRISectorLightGpuData>& outSectors) const
{
	const auto& registry = mSectorLighting;
	outHeader = {};
	outHeader.sectorCount = registry.sectorCount;
	outHeader.activeCount = registry.activeSectorCount;
	outHeader.pulsingCount = registry.pulsingSectorCount;
	outHeader.flags = sectorLightingEnabled ? 1u : 0u;
	outSectors.assign(registry.sectorCount, {});

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size() || sectorIndex >= outSectors.size())
		{
			continue;
		}

		const auto& source = registry.sectors[sectorIndex];
		auto& target = outSectors[sectorIndex];
		Copy3f(source.ambientColor, target.ambientColor);
		Copy3f(source.ambientColor, target.hemisphereColor);
		target.ambientIntensity = source.ambientIntensity * sectorLightMultiplier;
		target.hemisphereAmount = source.hemisphereAmount * sectorLightMultiplier;
		target.fogAmount = source.fogAmount * sectorLightMultiplier;
		target.pulseScale = source.pulseScale;
		target.sourceFlags = source.sourceFlags;
		target.paletteIndex = source.paletteIndex;
		target.lotag = source.lotag;
		target.hitag = source.hitag;
	}
}

uint64_t SceneLightSystem::BuildSectorLightingPayloadHash(float sectorLightMultiplier, bool sectorLightingEnabled) const
{
	const auto& registry = mSectorLighting;
	uint64_t hash = 1469598103934665603ull;
	hash = HashCombine64(hash, sectorLightingEnabled ? 1ull : 0ull);
	hash = HashCombine64(hash, (uint64_t)FloatBits(sectorLightMultiplier));
	hash = HashCombine64(hash, (uint64_t)registry.sectorCount);
	hash = HashCombine64(hash, (uint64_t)registry.activeSectorCount);
	hash = HashCombine64(hash, (uint64_t)registry.pulsingSectorCount);
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		hash = HashCombine64(hash, (uint64_t)sectorIndex);
		if (sectorIndex >= registry.sectors.size())
		{
			continue;
		}

		const auto& sector = registry.sectors[sectorIndex];
		hash = HashCombine64(hash, (uint64_t)sector.sourceFlags);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.paletteIndex);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.lotag);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.hitag);
		hash = HashCombine64(hash, (uint64_t)(int64_t)sector.averageShade);
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.ambientIntensity));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.hemisphereAmount));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.fogAmount));
		hash = HashCombine64(hash, (uint64_t)FloatBits(sector.pulseScale));
	}

	return hash;
}

bool SceneLightSystem::AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t maxLights, uint32_t& outId)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	if (GetManualAnalyticLightCount() >= maxLights)
	{
		return false;
	}

	const uint32_t id = mNextRuntimePointLightId++;
	if (!AddManualAnalyticLight(id, position, color, intensity, radius))
	{
		return false;
	}

	outId = id;
	return true;
}

bool SceneLightSystem::UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	return UpdateManualAnalyticLight(id, position, color, intensity, radius);
}

bool SceneLightSystem::RemoveRuntimePointLight(uint32_t id)
{
	return RemoveManualAnalyticLight(id);
}

bool SceneLightSystem::ClearRuntimePointLights()
{
	if (GetManualAnalyticLightCount() == 0)
	{
		return false;
	}

	ClearManualAnalyticLights();
	return true;
}

void SceneLightSystem::ResetRuntimePointLights()
{
	ClearManualAnalyticLights();
	mNextRuntimePointLightId = 1;
}

void SceneLightSystem::PrintRuntimePointLights(uint32_t maxLights) const
{
	const auto& analyticLights = GetAnalyticLights();
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u overlay_rules=%u map_rules=%u matched_surfaces=%u overlay_matches=%u deduped=%u truncated=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u limit=%u\n",
		(uint32_t)analyticLights.activeLights.size(),
		(uint32_t)analyticLights.manualLights.size(),
		analyticLights.transientMuzzleSlotCount,
		analyticLights.transientMuzzleActiveCount,
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.actorOverlayRuleCount,
		analyticLights.mapOverlayRuleCount,
		analyticLights.matchedSurfaceCount,
		analyticLights.actorOverlayMatchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount,
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		maxLights);
	if (analyticLights.activeLights.empty())
	{
		return;
	}

	for (const SceneAnalyticLight& light : analyticLights.activeLights)
	{
		const char* sourceBase =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_Manual) != 0 ? "manual" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? "transient" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? "overlay" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? "overlay" :
			"heuristic";
		const char* sourceSuffix =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? ":muzzle" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_SpriteTileHeuristic) != 0 ? ":sprite_tile" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? ":actor" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? ":map" :
			"";
		const auto diagnosticIt = analyticLights.activeDiagnosticFlags.find(light.stableKey);
		const uint32_t diagnosticFlags = diagnosticIt != analyticLights.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT analytic light %u: id=%u topology=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s shadow=%s source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			YesNo((light.flags & SceneAnalyticLightFlag_CastsShadow) != 0),
			sourceBase,
			sourceSuffix,
			light.sourceRuleId,
			light.actorIndex,
			light.textureId,
			light.position[0],
			light.position[1],
			light.position[2],
			light.color[0],
			light.color[1],
			light.color[2],
			light.intensity,
			light.radius);
	}
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

bool SceneLightSystem::UpdateManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	const auto it = std::find_if(mAnalyticLights.manualLights.begin(), mAnalyticLights.manualLights.end(), [id](const SceneAnalyticLight& light)
	{
		return light.id == id;
	});
	if (it == mAnalyticLights.manualLights.end())
	{
		return false;
	}

	it->position[0] = position[0];
	it->position[1] = position[1];
	it->position[2] = position[2];
	it->color[0] = std::max(color[0], 0.0f);
	it->color[1] = std::max(color[1], 0.0f);
	it->color[2] = std::max(color[2], 0.0f);
	it->intensity = intensity;
	it->radius = radius;
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

bool SceneLightSystem::ClearSpriteTileHeuristics()
{
	if (mAnalyticLights.spriteTileRules.empty())
	{
		return false;
	}

	mAnalyticLights.spriteTileRules.clear();
	return true;
}

void SceneLightSystem::PrintSpriteTileLightHeuristics() const
{
	const auto& analyticLights = GetAnalyticLights();
	Printf("NRI PT analytic sprite-tile heuristics: rules=%u matched_surfaces=%u deduped=%u truncated=%u\n",
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.matchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount);
	for (const auto& rule : analyticLights.spriteTileRules)
	{
		Printf("NRI PT analytic heuristic %u: tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
			rule.ruleId,
			rule.textureId,
			rule.color[0],
			rule.color[1],
			rule.color[2],
			rule.intensity,
			rule.radius,
			rule.flickerFrames);
	}
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

bool SceneLightSystem::ClearTextureEmissiveHeuristics()
{
	if (mEmissiveSurfaces.textureRules.empty())
	{
		return false;
	}

	mEmissiveSurfaces.textureRules.clear();
	mEmissiveSurfaces.materialBindingChanged = true;
	mEmissiveSurfaces.materialPropertiesChanged = true;
	return true;
}

void SceneLightSystem::PrintTextureEmissiveHeuristics() const
{
	const auto& emissive = GetEmissiveSurfaces();
	Printf("NRI PT emissive heuristics: rules=%u auto_tagged=%u explicit_matches=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u active=%u total_power=%.3f glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f truncated=%u\n",
		(uint32_t)emissive.textureRules.size(),
		emissive.autoTaggedCount,
		emissive.explicitRuleMatchCount,
		emissive.overrideRuleCount,
		emissive.overrideMatchedSurfaceCount,
		emissive.materialResponseRuleCount,
		emissive.materialResponseMatchedSurfaceCount,
		(uint32_t)emissive.activeSurfaces.size(),
		emissive.totalPowerEstimate,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend,
		emissive.truncatedSurfaceCount);
	for (const auto& rule : emissive.textureRules)
	{
		Printf("NRI PT emissive heuristic %u: tile=%u mode=%s intensity_scale=%.3f explicit_color=%s color=(%.3f, %.3f, %.3f)\n",
			rule.ruleId,
			rule.textureId,
			GetMaterialEmissiveModeName(rule.emissiveMode),
			rule.intensityScale,
			rule.hasExplicitColor ? "yes" : "no",
			rule.emissiveColor[0],
			rule.emissiveColor[1],
			rule.emissiveColor[2]);
	}
}

bool SceneLightSystem::MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const
{
	const NRILightingSettings settings = CaptureSettings();
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	return EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale);
}

bool SceneLightSystem::ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const
{
	const NRILightingSettings settings = CaptureSettings();
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
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

void SceneLightSystem::AppendSurfaceRecord(SurfaceRecord record, uint32_t materialIndexBase)
{
	if (record.materialIndex != UINT32_MAX)
	{
		record.materialIndex += materialIndexBase;
	}

	mSurfaceRecords.push_back(record);
	mFrameAppendStats.totalRecordCount++;
	switch (record.source)
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
		const uint32_t materialLookupIndex = materialLookupIndexBase + inOutLocalMaterialIndex;
		const uint64_t inheritedIdentityKey =
			identityOverrides != nullptr && surfaceIndex < identityOverrides->size() ?
			(*identityOverrides)[surfaceIndex] :
			0ull;
		const SurfaceRecord record = BuildSurfaceRecord(
			surface,
			materials,
			source,
			inOutLocalMaterialIndex,
			materialLookupIndex,
			inheritedIdentityKey);

		AppendSurfaceRecord(record, materialIndexBase);
		++inOutLocalMaterialIndex;
	}
}
