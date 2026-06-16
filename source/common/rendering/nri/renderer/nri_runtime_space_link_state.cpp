#include "nri_renderer.h"
#include "nri_cvars.h"
#include "gamestruct.h"
#include "printf.h"

#include <algorithm>
#include <string>

namespace
{
	static bool SameRuntimeLinkDebugState(const RuntimeLinkDebugState& a, const RuntimeLinkDebugState& b)
	{
		return
			a.available == b.available &&
			a.specialWaterSector == b.specialWaterSector &&
			a.playerSectorIndex == b.playerSectorIndex &&
			a.playerSectorLotag == b.playerSectorLotag &&
			a.playerSectorHitag == b.playerSectorHitag &&
			a.effectiveSectorLotag == b.effectiveSectorLotag &&
			a.actorSectorIndex == b.actorSectorIndex &&
			a.actorSectorLotag == b.actorSectorLotag &&
			a.actorSectorHitag == b.actorSectorHitag &&
			a.onWarpingSector == b.onWarpingSector &&
			a.transporterHold == b.transporterHold &&
			a.rrGeoCount == b.rrGeoCount;
	}

	static bool SameRuntimeTaggedSectorDebugInfo(const RuntimeTaggedSectorDebugInfo& a, const RuntimeTaggedSectorDebugInfo& b)
	{
		if (a.available != b.available ||
			a.sectorIndex != b.sectorIndex ||
			a.lotag != b.lotag ||
			a.hitag != b.hitag ||
			a.effectorCount != b.effectorCount)
		{
			return false;
		}

		for (size_t i = 0; i < countof(a.effectorLotags); ++i)
		{
			if (a.effectorLotags[i] != b.effectorLotags[i] || a.effectorHitags[i] != b.effectorHitags[i])
			{
				return false;
			}
		}

		return true;
	}

	static bool ShouldStoreRuntimeSectorControlInfo(const RuntimeTaggedSectorDebugInfo& info)
	{
		return info.available && (info.lotag != 0 || info.hitag != 0 || info.effectorCount > 0);
	}

	static bool AppendRuntimeSectorControlInfo(std::array<RuntimeTaggedSectorDebugInfo, 12>& infos, uint32_t& infoCount, const RuntimeTaggedSectorDebugInfo& info)
	{
		if (!ShouldStoreRuntimeSectorControlInfo(info))
		{
			return false;
		}

		for (uint32_t i = 0; i < infoCount; ++i)
		{
			if (infos[i].sectorIndex == info.sectorIndex)
			{
				return false;
			}
		}

		if (infoCount >= infos.size())
		{
			return false;
		}

		infos[infoCount++] = info;
		return true;
	}

	static bool GetRuntimeSectorControlInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo& info)
	{
		if (!validSectorIndex(sectorIndex))
		{
			return false;
		}

		info = {};
		if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo(sectorIndex, &info))
		{
			return true;
		}

		const auto& sec = sector[(unsigned)sectorIndex];
		info.available = true;
		info.sectorIndex = sectorIndex;
		info.lotag = sec.lotag;
		info.hitag = sec.hitag;
		return true;
	}

}

void NRIRenderer::TraceRuntimeLinkEvents(HWDrawInfo& di)
{
	if (!nri_ptruntimelinktrace)
	{
		mHasRuntimeLinkTraceState = false;
		mLastRuntimeLinkTraceState = {};
		return;
	}

	RuntimeLinkTraceState current = {};
	current.valid = true;
	current.candidateSectorIndex = mRuntimeSpaceLinkLastFrame.candidateSectorIndex;
	current.sourceSectorIndex = mRuntimeSpaceLinkLastFrame.sourceSectorIndex;
	current.geoEffectActive = mRuntimeSpaceLinkLastFrame.geoEffectActive;

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		const auto& sec = sector[sectorIndex];
		if (sec.lotag != 0)
		{
			current.visibleTaggedSectorCount++;
			if (current.taggedVisibleSectorStoredCount < current.taggedVisibleSectors.size())
			{
				RuntimeTaggedSectorDebugInfo info = {};
				if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo((int)sectorIndex, &info))
				{
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
				else
				{
					info.available = true;
					info.sectorIndex = (int32_t)sectorIndex;
					info.lotag = sec.lotag;
					info.hitag = sec.hitag;
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
			}
		}
		if (sec.lotag == 848)
		{
			current.visible848SectorCount++;
		}
		if (sec.lotag == 160 || sec.lotag == 161)
		{
			current.visibleTeleportSectorCount++;
		}
	}

	if (gi != nullptr)
	{
		gi->GetRuntimeLinkDebugState(&current.game);
	}

	std::array<int32_t, 4> controlRoots =
	{
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.actorSectorIndex
	};

	for (const int32_t rootSectorIndex : controlRoots)
	{
		if (!validSectorIndex(rootSectorIndex))
		{
			continue;
		}

		RuntimeTaggedSectorDebugInfo rootInfo = {};
		if (GetRuntimeSectorControlInfo(rootSectorIndex, rootInfo))
		{
			AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, rootInfo);
		}

		const auto& rootSector = sector[(unsigned)rootSectorIndex];
		for (const auto& wal : rootSector.walls)
		{
			if (!wal.twoSided())
			{
				continue;
			}

			const int32_t adjacentSectorIndex = wal.nextsector;
			RuntimeTaggedSectorDebugInfo adjacentInfo = {};
			if (GetRuntimeSectorControlInfo(adjacentSectorIndex, adjacentInfo))
			{
				AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, adjacentInfo);
			}
		}
	}

	const bool sameAsLast =
		mHasRuntimeLinkTraceState &&
		mLastRuntimeLinkTraceState.valid == current.valid &&
		mLastRuntimeLinkTraceState.candidateSectorIndex == current.candidateSectorIndex &&
		mLastRuntimeLinkTraceState.sourceSectorIndex == current.sourceSectorIndex &&
		mLastRuntimeLinkTraceState.geoEffectActive == current.geoEffectActive &&
		mLastRuntimeLinkTraceState.visibleTaggedSectorCount == current.visibleTaggedSectorCount &&
		mLastRuntimeLinkTraceState.visible848SectorCount == current.visible848SectorCount &&
		mLastRuntimeLinkTraceState.visibleTeleportSectorCount == current.visibleTeleportSectorCount &&
		mLastRuntimeLinkTraceState.taggedVisibleSectorStoredCount == current.taggedVisibleSectorStoredCount &&
		mLastRuntimeLinkTraceState.nearbyControlSectorStoredCount == current.nearbyControlSectorStoredCount &&
		SameRuntimeLinkDebugState(mLastRuntimeLinkTraceState.game, current.game);

	bool sameTaggedSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.taggedVisibleSectors[i], current.taggedVisibleSectors[i]))
			{
				sameTaggedSectors = false;
				break;
			}
		}
	}

	bool sameNearbyControlSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.nearbyControlSectors[i], current.nearbyControlSectors[i]))
			{
				sameNearbyControlSectors = false;
				break;
			}
		}
	}

	if (sameAsLast && sameTaggedSectors && sameNearbyControlSectors)
	{
		return;
	}

	mLastRuntimeLinkTraceState = current;
	mHasRuntimeLinkTraceState = true;

	Printf("NRI PT runtime link event: geo_effect=%s candidate_sector=%d source_sector=%d player_sector=%d lotag=%d hitag=%d effective_lotag=%d actor_sector=%d actor_lotag=%d actor_hitag=%d on_warp=%d transporter_hold=%d rr_geo_count=%d special_water=%s visible_tagged=%u visible_848=%u visible_teleport=%u\n",
		current.geoEffectActive ? "yes" : "no",
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.playerSectorLotag,
		current.game.playerSectorHitag,
		current.game.effectiveSectorLotag,
		current.game.actorSectorIndex,
		current.game.actorSectorLotag,
		current.game.actorSectorHitag,
		current.game.onWarpingSector,
		current.game.transporterHold,
		current.game.rrGeoCount,
		current.game.specialWaterSector ? "yes" : "no",
		current.visibleTaggedSectorCount,
		current.visible848SectorCount,
		current.visibleTeleportSectorCount);

	if (current.taggedVisibleSectorStoredCount > 0)
	{
		std::string taggedLine = "NRI PT runtime tagged sectors:";
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			const auto& info = current.taggedVisibleSectors[i];
			taggedLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				taggedLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						taggedLine += ",";
					}
					taggedLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					taggedLine += ",...";
				}
			}
			taggedLine += "]";
		}
		Printf("%s\n", taggedLine.c_str());
	}

	if (current.nearbyControlSectorStoredCount > 0)
	{
		std::string controlLine = "NRI PT runtime nearby controls:";
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			const auto& info = current.nearbyControlSectors[i];
			controlLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				controlLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						controlLine += ",";
					}
					controlLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					controlLine += ",...";
				}
			}
			controlLine += "]";
		}
		Printf("%s\n", controlLine.c_str());
	}
}
