#include "ns.h"

#include <algorithm>

#include "duke3d.h"
#include "runtime_map_movers.h"

BEGIN_DUKE_NS

namespace
{
	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;

	struct MoverSource
	{
		DDukeActor* actor;
		int sectorIndex;
		int canonicalWallOffset;
	};

	struct MoverCandidate
	{
		RuntimeMapMoverSnapshot snapshot;
		TArray<MoverSource> sources;
	};

	TArray<RuntimeMapMoverSnapshot> MoverSnapshots;
	uint64_t MoverMapEpoch = 1;

	void HashBytes(uint64_t& hash, const void* data, size_t size)
	{
		auto bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; i++)
		{
			hash = (hash ^ bytes[i]) * HashPrime;
		}
	}

	template<class T>
	void HashValue(uint64_t& hash, const T& value)
	{
		HashBytes(hash, &value, sizeof(value));
	}

	bool UsesCanonicalWallOffsets(int lotag)
	{
		switch (lotag)
		{
		case SE_0_ROTATING_SECTOR:
		case SE_2_EARTHQUAKE:
		case SE_5_BOSS:
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_15_SLIDING_DOOR:
		case SE_16_REACTOR:
		case SE_26:
		case SE_30_TWO_WAY_TRAIN:
			return true;
		default:
			return false;
		}
	}

	RuntimeMapMoverCapability ClassifyMover(const DDukeActor* actor)
	{
		switch (actor->spr.lotag)
		{
		case SE_15_SLIDING_DOOR:
			return RuntimeMapMoverCapability::RigidTranslation;
		case SE_0_ROTATING_SECTOR:
			return (actor->sector()->lotag & 0xff) == 30
				? RuntimeMapMoverCapability::StableTopologyDeformer
				: RuntimeMapMoverCapability::RigidTransform;
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_30_TWO_WAY_TRAIN:
			return RuntimeMapMoverCapability::RigidTransform;
		case SE_2_EARTHQUAKE:
		case SE_5_BOSS:
		case SE_16_REACTOR:
		case SE_17_WARP_ELEVATOR:
		case SE_18_INCREMENTAL_SECTOR_RISE_FALL:
		case SE_20_STRETCH_BRIDGE:
		case SE_21_DROP_FLOOR:
		case SE_22_TEETH_DOOR:
		case SE_25_PISTON:
		case SE_26:
		case SE_29_WAVES:
		case SE_31_FLOOR_RISE_FALL:
		case SE_32_CEILING_RISE_FALL:
			return RuntimeMapMoverCapability::StableTopologyDeformer;
		case SE_3_RANDOM_LIGHTS_AFTER_SHOT_OUT:
		case SE_4_RANDOM_LIGHTS:
		case SE_8_UP_OPEN_DOOR_LIGHTS:
		case SE_9_DOWN_OPEN_DOOR_LIGHTS:
		case SE_12_LIGHT_SWITCH:
		case SE_28_LIGHTNING:
		case SE_47_LIGHT_SWITCH:
		case SE_48_LIGHT_SWITCH:
		case SE_49_POINT_LIGHT:
		case SE_50_SPOT_LIGHT:
			return RuntimeMapMoverCapability::MaterialOrLightOnly;
		default:
			return RuntimeMapMoverCapability::Unknown;
		}
	}

	uint64_t StableGroupId(DDukeActor* actor)
	{
		const bool pivotGroup = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr;
		const auto authority = pivotGroup ? actor->GetOwner() : actor;
		uint64_t hash = HashOffset;
		const uint32_t kind = pivotGroup ? 0x44554b30u : 0x44554b4du;
		const int sectorIndex = authority->sectno();
		HashValue(hash, kind);
		HashValue(hash, sectorIndex);
		HashValue(hash, actor->spr.lotag);
		HashValue(hash, actor->spr.hitag);
		if (!pivotGroup) HashValue(hash, actor->temp_data[1]);
		return hash;
	}

	RuntimeMapMoverPose CapturePose(const DDukeActor* actor)
	{
		RuntimeMapMoverPose pose = {};
		if (!UsesCanonicalWallOffsets(actor->spr.lotag)) return pose;
		auto translationOwner = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->ownerActor != nullptr
			? actor->ownerActor.Get() : actor;
		pose.translation = { translationOwner->spr.pos.X, translationOwner->spr.pos.Y, 0.0 };
		switch (actor->spr.lotag)
		{
		case SE_0_ROTATING_SECTOR:
		case SE_5_BOSS:
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_16_REACTOR:
		case SE_30_TWO_WAY_TRAIN:
			pose.rotation = actor->temp_angle;
			break;
		default:
			break;
		}
		return pose;
	}

	bool SamePose(const RuntimeMapMoverPose& a, const RuntimeMapMoverPose& b)
	{
		return a.translation == b.translation && a.rotation == b.rotation;
	}

	RuntimeMapMoverPose InterpolatePose(const RuntimeMapMoverPose& previous, const RuntimeMapMoverPose& current, double fraction)
	{
		RuntimeMapMoverPose result = {};
		fraction = std::clamp(fraction, 0.0, 1.0);
		result.translation = previous.translation + (current.translation - previous.translation) * fraction;
		result.rotation = previous.rotation + deltaangle(previous.rotation, current.rotation) * fraction;
		return result;
	}

	MoverCandidate* FindCandidate(TArray<MoverCandidate>& candidates, uint64_t id)
	{
		for (auto& candidate : candidates)
			if (candidate.snapshot.stableGroupId == id) return &candidate;
		return nullptr;
	}

	const RuntimeMapMoverSnapshot* FindPrevious(uint64_t id)
	{
		for (const auto& snapshot : MoverSnapshots)
			if (snapshot.stableGroupId == id) return &snapshot;
		return nullptr;
	}

	void AppendMember(RuntimeMapMoverSnapshot& snapshot, int sectorIndex, int wallOffset, int wallCount, uint32_t flags)
	{
		for (auto& member : snapshot.members)
		{
			if (member.sectorIndex == sectorIndex && member.canonicalWallOffset == wallOffset)
			{
				member.flags |= flags;
				return;
			}
		}
		const auto index = snapshot.members.Reserve(1);
		snapshot.members[index] = { sectorIndex, wallOffset, wallCount, flags };
	}

	void BuildCandidates(TArray<MoverCandidate>& candidates)
	{
		DukeStatIterator it(STAT_EFFECTOR);
		while (auto actor = it.Next())
		{
			if (!actor->exists() || actor->sector() == nullptr || actor->spr.lotag == SE_1_PIVOT) continue;

			const auto id = StableGroupId(actor);
			auto candidate = FindCandidate(candidates, id);
			if (candidate == nullptr)
			{
				const auto index = candidates.Reserve(1);
				candidate = &candidates[index];
				auto authority = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr
					? actor->GetOwner() : actor;
				candidate->snapshot.stableGroupId = id;
				candidate->snapshot.mapEpoch = MoverMapEpoch;
				candidate->snapshot.capability = ClassifyMover(actor);
				candidate->snapshot.ownerActorIndex = authority->GetIndex();
				candidate->snapshot.ownerSectorIndex = authority->sectno();
				candidate->snapshot.effectorLotag = actor->spr.lotag;
				candidate->snapshot.effectorHitag = actor->spr.hitag;
			}
			else if (candidate->snapshot.capability != ClassifyMover(actor))
			{
				candidate->snapshot.capability = RuntimeMapMoverCapability::Unknown;
			}

			const int sectorIndex = actor->sectno();
			const int wallCount = (int)actor->sector()->walls.Size();
			const int wallOffset = UsesCanonicalWallOffsets(actor->spr.lotag) ? actor->temp_data[1] : -1;
			uint32_t flags;
			if (candidate->snapshot.capability == RuntimeMapMoverCapability::MaterialOrLightOnly)
				flags = RuntimeMapMoverMember_ControlOnly;
			else if (actor->spr.lotag == SE_29_WAVES)
				flags = RuntimeMapMoverMember_OwnsFloor;
			else
				flags = RuntimeMapMoverMember_OwnsWalls | RuntimeMapMoverMember_OwnsFloor |
					RuntimeMapMoverMember_OwnsCeiling | RuntimeMapMoverMember_SharedVertexPropagation;
			AppendMember(candidate->snapshot, sectorIndex, wallOffset, wallCount, flags);
			candidate->sources.Push({ actor, sectorIndex, wallOffset });

			if (actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr && actor->GetOwner()->sector() != actor->sector())
			{
				AppendMember(candidate->snapshot, actor->GetOwner()->sectno(), -1,
					(int)actor->GetOwner()->sector()->walls.Size(), RuntimeMapMoverMember_ControlOnly);
			}
		}
	}

	void HashSector(const MoverSource& source, RuntimeMapMoverCapability capability,
		uint64_t& topology, uint64_t& geometry, uint64_t& material, uint64_t& visibility, uint64_t& light)
	{
		const auto actor = source.actor;
		const auto sec = actor->sector();
		const int wallCount = (int)sec->walls.Size();
		HashValue(topology, source.sectorIndex);
		HashValue(topology, source.canonicalWallOffset);
		HashValue(topology, actor->spr.lotag);
		HashValue(topology, actor->spr.hitag);
		HashValue(topology, wallCount);

		const bool canonicalRigid =
			(capability == RuntimeMapMoverCapability::RigidTranslation || capability == RuntimeMapMoverCapability::RigidTransform) &&
			source.canonicalWallOffset >= 0 &&
			(unsigned)(source.canonicalWallOffset + wallCount) <= mspos.Size();
		HashValue(geometry, sec->floorz);
		HashValue(geometry, sec->ceilingz);
		HashValue(geometry, sec->floorheinum);
		HashValue(geometry, sec->ceilingheinum);

		const int floorTexture = sec->floortexture.GetIndex();
		const int ceilingTexture = sec->ceilingtexture.GetIndex();
		HashValue(material, floorTexture);
		HashValue(material, ceilingTexture);
		HashValue(material, sec->floorxpan_);
		HashValue(material, sec->floorypan_);
		HashValue(material, sec->ceilingxpan_);
		HashValue(material, sec->ceilingypan_);
		HashValue(material, sec->floorpal);
		HashValue(material, sec->ceilingpal);

		const auto floorFlags = sec->floorstat.GetValue();
		const auto ceilingFlags = sec->ceilingstat.GetValue();
		HashValue(visibility, floorFlags);
		HashValue(visibility, ceilingFlags);
		HashValue(visibility, sec->portalflags);
		HashValue(visibility, sec->portalnum);
		HashValue(visibility, sec->visibility);
		HashValue(light, sec->floorshade);
		HashValue(light, sec->ceilingshade);

		for (int i = 0; i < wallCount; i++)
		{
			const auto& wal = sec->walls[i];
			const int wallIndex = wallindex(&wal);
			HashValue(topology, wallIndex);
			HashValue(topology, wal.point2);
			HashValue(topology, wal.nextwall);
			HashValue(topology, wal.nextsector);
			if (canonicalRigid)
			{
				const auto& local = mspos[source.canonicalWallOffset + i];
				HashValue(geometry, local.X);
				HashValue(geometry, local.Y);
			}
			else
			{
				HashValue(geometry, wal.pos.X);
				HashValue(geometry, wal.pos.Y);
			}

			const int wallTexture = wal.walltexture.GetIndex();
			const int overTexture = wal.overtexture.GetIndex();
			HashValue(material, wallTexture);
			HashValue(material, overTexture);
			HashValue(material, wal.xpan_);
			HashValue(material, wal.ypan_);
			HashValue(material, wal.xrepeat);
			HashValue(material, wal.yrepeat);
			HashValue(material, wal.pal);
			const auto wallFlags = wal.cstat.GetValue();
			HashValue(visibility, wallFlags);
			HashValue(visibility, wal.portalflags);
			HashValue(visibility, wal.portalnum);
			HashValue(light, wal.shade);
		}
	}

	void FinishCandidate(MoverCandidate& candidate)
	{
		if (candidate.sources.Size() > 1)
		{
			std::sort(candidate.sources.Data(), candidate.sources.Data() + candidate.sources.Size(),
				[](const MoverSource& a, const MoverSource& b)
				{
					if (a.sectorIndex != b.sectorIndex) return a.sectorIndex < b.sectorIndex;
					return a.actor->GetIndex() < b.actor->GetIndex();
				});
		}
		if (candidate.snapshot.members.Size() > 1)
		{
			std::sort(candidate.snapshot.members.Data(), candidate.snapshot.members.Data() + candidate.snapshot.members.Size(),
				[](const RuntimeMapMoverMember& a, const RuntimeMapMoverMember& b)
				{
					if (a.sectorIndex != b.sectorIndex) return a.sectorIndex < b.sectorIndex;
					return a.canonicalWallOffset < b.canonicalWallOffset;
				});
		}

		auto pose = CapturePose(candidate.sources[0].actor);
		for (const auto& source : candidate.sources)
		{
			if (!SamePose(pose, CapturePose(source.actor))) candidate.snapshot.capability = RuntimeMapMoverCapability::Unknown;
		}

		uint64_t topology = HashOffset, geometry = HashOffset, material = HashOffset;
		uint64_t visibility = HashOffset, light = HashOffset;
		for (const auto& member : candidate.snapshot.members)
		{
			HashValue(topology, member.sectorIndex);
			HashValue(topology, member.canonicalWallOffset);
			HashValue(topology, member.wallCount);
			HashValue(topology, member.flags);
		}
		for (const auto& source : candidate.sources)
			HashSector(source, candidate.snapshot.capability, topology, geometry, material, visibility, light);

		auto& snapshot = candidate.snapshot;
		snapshot.topologySignature = topology;
		snapshot.geometrySignature = geometry;
		snapshot.materialSignature = material;
		snapshot.visibilitySignature = visibility;
		snapshot.lightSignature = light;
		snapshot.simulationCurrentPose = pose;

		const auto previous = FindPrevious(snapshot.stableGroupId);
		if (previous == nullptr)
		{
			snapshot.topologyGeneration = snapshot.geometryGeneration = snapshot.materialGeneration = 1;
			snapshot.transformGeneration = snapshot.visibilityGeneration = snapshot.lightGeneration = 1;
			snapshot.simulationPreviousPose = pose;
		}
		else
		{
			snapshot.topologyGeneration = previous->topologyGeneration + (previous->topologySignature != topology);
			snapshot.geometryGeneration = previous->geometryGeneration + (previous->geometrySignature != geometry);
			snapshot.materialGeneration = previous->materialGeneration + (previous->materialSignature != material);
			snapshot.visibilityGeneration = previous->visibilityGeneration + (previous->visibilitySignature != visibility);
			snapshot.lightGeneration = previous->lightGeneration + (previous->lightSignature != light);
			snapshot.transformGeneration = previous->transformGeneration + !SamePose(previous->simulationCurrentPose, pose);
			snapshot.simulationPreviousPose = previous->simulationCurrentPose;
		}
		snapshot.presentationPreviousPose = snapshot.simulationPreviousPose;
		snapshot.presentationCurrentPose = snapshot.simulationCurrentPose;
	}
}

void ResetRuntimeMapMoverAuthority()
{
	MoverSnapshots.Clear();
	if (++MoverMapEpoch == 0) MoverMapEpoch = 1;
}

void UpdateRuntimeMapMoverAuthority()
{
	TArray<MoverCandidate> candidates;
	BuildCandidates(candidates);
	for (auto& candidate : candidates) FinishCandidate(candidate);
	if (candidates.Size() > 1)
	{
		std::sort(candidates.Data(), candidates.Data() + candidates.Size(),
			[](const MoverCandidate& a, const MoverCandidate& b)
			{
				return a.snapshot.stableGroupId < b.snapshot.stableGroupId;
			});
	}

	MoverSnapshots.Clear();
	for (auto& candidate : candidates) MoverSnapshots.Push(std::move(candidate.snapshot));
}

void CaptureRuntimeMapMoverAuthority(TArray<RuntimeMapMoverSnapshot>& out, double presentationFraction)
{
	out = MoverSnapshots;
	for (auto& snapshot : out)
	{
		snapshot.presentationPreviousPose = snapshot.simulationPreviousPose;
		snapshot.presentationCurrentPose = InterpolatePose(snapshot.simulationPreviousPose,
			snapshot.simulationCurrentPose, presentationFraction);
	}
}

void GameInterface::CaptureRuntimeMapMovers(TArray<RuntimeMapMoverSnapshot>& out, double presentationFraction)
{
	CaptureRuntimeMapMoverAuthority(out, presentationFraction);
}

END_DUKE_NS
