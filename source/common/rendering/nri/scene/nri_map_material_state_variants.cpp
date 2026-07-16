#include "nri_map_material_state_variants.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace nri_scene
{
namespace
{
	bool SameRecordIdentity(
		const PTMapMaterialStateVariantIdentity& a,
		const PTMapMaterialStateVariantIdentity& b)
	{
		return a.mapEpoch == b.mapEpoch &&
			a.buildSerial == b.buildSerial &&
			a.moverStableId == b.moverStableId &&
			a.chunkIndex == b.chunkIndex;
	}

	bool HasValidIdentity(const PTMapMaterialStateVariantIdentity& identity)
	{
		return identity.mapEpoch != 0 &&
			identity.buildSerial != 0 &&
			identity.moverStableId != UINT64_MAX &&
			identity.chunkIndex != UINT32_MAX;
	}

	bool HasValidCanonicalLayout(const CanonicalPTMapMaterialLayout& layout)
	{
		const size_t slotCount = layout.canonicalSlotIds.size();
		if (!layout.valid || slotCount == 0 ||
			layout.canonicalSemanticSlots.size() != slotCount ||
			layout.canonicalMaterialStateKeys.size() != slotCount ||
			layout.emittedToCanonical.size() != slotCount ||
			layout.canonicalToEmitted.size() != slotCount)
		{
			return false;
		}
		for (uint32_t slot = 0; slot < (uint32_t)slotCount; ++slot)
		{
			if ((slot != 0 && layout.canonicalSlotIds[slot - 1] >= layout.canonicalSlotIds[slot]) ||
				layout.emittedToCanonical[slot] >= slotCount ||
				layout.canonicalToEmitted[slot] >= slotCount)
			{
				return false;
			}
		}
		for (uint32_t emitted = 0; emitted < (uint32_t)slotCount; ++emitted)
		{
			const uint32_t canonical = layout.emittedToCanonical[emitted];
			if (layout.canonicalToEmitted[canonical] != emitted)
			{
				return false;
			}
		}
		return true;
	}

	CanonicalPTMapMaterialLayout CopyLayoutIdentity(const CanonicalPTMapMaterialLayout& source)
	{
		CanonicalPTMapMaterialLayout result = {};
		result.valid = true;
		result.chunkIndex = source.chunkIndex;
		result.layoutKey = source.layoutKey;
		result.canonicalSlotIds = source.canonicalSlotIds;
		result.canonicalSemanticSlots = source.canonicalSemanticSlots;
		return result;
	}
}

PTMapMaterialStateVariantResult PTMapMaterialStateVariantOwner::Resolve(
	const PTMapMaterialStateVariantIdentity& identity,
	const CanonicalPTMapMaterialLayout& layout)
{
	PTMapMaterialStateVariantResult result = {};
	result.stateKey = layout.stateKey;
	mStats.requests++;
	if (!HasValidIdentity(identity) || !HasValidCanonicalLayout(layout) || layout.chunkIndex != identity.chunkIndex)
	{
		mStats.fallbacks++;
		return result;
	}

	if (!mHasEpochBuild || mMapEpoch != identity.mapEpoch || mBuildSerial != identity.buildSerial)
	{
		if (mHasEpochBuild)
		{
			mStats.epochBuildResets++;
		}
		mHasEpochBuild = true;
		mMapEpoch = identity.mapEpoch;
		mBuildSerial = identity.buildSerial;
		mAccessSerial = 0;
		mRecords.clear();
		mStats.residentRecords = 0;
		mStats.residentVariants = 0;
	}

	auto recordIt = std::find_if(mRecords.begin(), mRecords.end(), [&](const Record& record)
	{
		return SameRecordIdentity(record.identity, identity);
	});
	if (recordIt != mRecords.end())
	{
		PTMapMaterialLayoutValidation validation;
		if (!ValidatePTMapMaterialLayoutCompatibility(recordIt->layoutIdentity, layout, validation))
		{
			result.decision = PTMapMaterialStateVariantDecision::LayoutReject;
			mStats.layoutRejects++;
			return result;
		}
	}

	result.eligible = true;
	mStats.eligible++;
	const uint64_t accessSerial = ++mAccessSerial;
	if (recordIt == mRecords.end())
	{
		if (mRecords.size() == MaxPTMapMaterialStateVariantRecords)
		{
			auto evictionIt = std::min_element(mRecords.begin(), mRecords.end(), [](const Record& a, const Record& b)
			{
				return std::tie(a.lastUsedSerial, a.insertionSerial, a.identity.moverStableId, a.identity.chunkIndex) <
					std::tie(b.lastUsedSerial, b.insertionSerial, b.identity.moverStableId, b.identity.chunkIndex);
			});
			result.recordEvicted = true;
			result.evictedRecordMoverStableId = evictionIt->identity.moverStableId;
			result.evictedRecordChunkIndex = evictionIt->identity.chunkIndex;
			mStats.residentVariants -= (uint32_t)evictionIt->variants.size();
			*evictionIt = {};
			recordIt = evictionIt;
			mStats.recordEvictions++;
		}
		else
		{
			mRecords.push_back({});
			recordIt = mRecords.end() - 1;
			mStats.residentRecords = (uint32_t)mRecords.size();
		}
		recordIt->identity = identity;
		recordIt->layoutIdentity = CopyLayoutIdentity(layout);
		recordIt->lastUsedSerial = accessSerial;
		recordIt->insertionSerial = accessSerial;
	}
	else
	{
		recordIt->lastUsedSerial = accessSerial;
	}
	auto variantIt = std::find_if(recordIt->variants.begin(), recordIt->variants.end(), [&](const Variant& variant)
	{
		return variant.stateKey == layout.stateKey &&
			variant.canonicalMaterialStateKeys == layout.canonicalMaterialStateKeys;
	});
	if (variantIt != recordIt->variants.end())
	{
		variantIt->lastUsedSerial = accessSerial;
		result.decision = PTMapMaterialStateVariantDecision::Hit;
		result.variantIndex = (uint32_t)(variantIt - recordIt->variants.begin());
		mStats.hits++;
		return result;
	}

	Variant variant = {};
	variant.stateKey = layout.stateKey;
	variant.lastUsedSerial = accessSerial;
	variant.insertionSerial = accessSerial;
	variant.canonicalMaterialStateKeys = layout.canonicalMaterialStateKeys;
	if (recordIt->variants.size() < MaxPTMapMaterialStateVariantsPerRecord)
	{
		recordIt->variants.push_back(std::move(variant));
		result.decision = PTMapMaterialStateVariantDecision::Insert;
		result.variantIndex = (uint32_t)recordIt->variants.size() - 1;
		mStats.residentVariants++;
	}
	else
	{
		auto evictionIt = std::min_element(recordIt->variants.begin(), recordIt->variants.end(), [](const Variant& a, const Variant& b)
		{
			return std::tie(a.lastUsedSerial, a.insertionSerial, a.stateKey) <
				std::tie(b.lastUsedSerial, b.insertionSerial, b.stateKey);
		});
		result.decision = PTMapMaterialStateVariantDecision::InsertEvicted;
		result.evicted = true;
		result.evictedStateKey = evictionIt->stateKey;
		result.variantIndex = (uint32_t)(evictionIt - recordIt->variants.begin());
		*evictionIt = std::move(variant);
		mStats.variantEvictions++;
	}
	result.inserted = true;
	mStats.inserts++;
	mStats.residentVariantHighWater = std::max(mStats.residentVariantHighWater, mStats.residentVariants);
	return result;
}

void PTMapMaterialStateVariantOwner::Reset()
{
	mHasEpochBuild = false;
	mMapEpoch = 0;
	mBuildSerial = 0;
	mAccessSerial = 0;
	mRecords.clear();
	mStats = {};
}

const char* GetPTMapMaterialStateVariantDecisionName(PTMapMaterialStateVariantDecision decision)
{
	switch (decision)
	{
	case PTMapMaterialStateVariantDecision::Fallback: return "fallback";
	case PTMapMaterialStateVariantDecision::LayoutReject: return "layout-reject";
	case PTMapMaterialStateVariantDecision::Hit: return "hit";
	case PTMapMaterialStateVariantDecision::Insert: return "insert";
	case PTMapMaterialStateVariantDecision::InsertEvicted: return "insert-evicted";
	}
	return "unknown";
}
}
