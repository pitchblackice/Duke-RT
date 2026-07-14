#pragma once

#include "nri_map_material_layout.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
static constexpr uint32_t MaxPTMapMaterialStateVariantsPerRecord = 4;
static constexpr uint32_t MaxPTMapMaterialStateVariantRecords = 256;

struct PTMapMaterialStateVariantIdentity
{
	uint64_t mapEpoch = 0;
	uint64_t buildSerial = 0;
	uint64_t moverStableId = UINT64_MAX;
	uint32_t chunkIndex = UINT32_MAX;
};

enum class PTMapMaterialStateVariantDecision : uint8_t
{
	Fallback = 0,
	LayoutReject,
	Hit,
	Insert,
	InsertEvicted,
};

struct PTMapMaterialStateVariantResult
{
	PTMapMaterialStateVariantDecision decision = PTMapMaterialStateVariantDecision::Fallback;
	bool eligible = false;
	bool inserted = false;
	bool evicted = false;
	bool recordEvicted = false;
	uint32_t variantIndex = UINT32_MAX;
	uint64_t stateKey = 0;
	uint64_t evictedStateKey = 0;
	uint64_t evictedRecordMoverStableId = UINT64_MAX;
	uint32_t evictedRecordChunkIndex = UINT32_MAX;
};

struct PTMapMaterialStateVariantStats
{
	uint64_t requests = 0;
	uint64_t eligible = 0;
	uint64_t layoutRejects = 0;
	uint64_t hits = 0;
	uint64_t inserts = 0;
	uint64_t variantEvictions = 0;
	uint64_t recordEvictions = 0;
	uint64_t fallbacks = 0;
	uint64_t epochBuildResets = 0;
	uint32_t residentRecords = 0;
	uint32_t residentVariants = 0;
	uint32_t residentVariantHighWater = 0;
};

// Renderer-independent shadow owner. It stores canonical identities and small
// material-state vectors only; geometry and GPU resources remain renderer-owned.
class PTMapMaterialStateVariantOwner
{
public:
	PTMapMaterialStateVariantResult Resolve(
		const PTMapMaterialStateVariantIdentity& identity,
		const CanonicalPTMapMaterialLayout& layout);

	void Reset();
	const PTMapMaterialStateVariantStats& GetStats() const { return mStats; }

private:
	struct Variant
	{
		uint64_t stateKey = 0;
		uint64_t lastUsedSerial = 0;
		uint64_t insertionSerial = 0;
		std::vector<uint64_t> canonicalMaterialStateKeys;
	};

	struct Record
	{
		PTMapMaterialStateVariantIdentity identity;
		CanonicalPTMapMaterialLayout layoutIdentity;
		uint64_t lastUsedSerial = 0;
		uint64_t insertionSerial = 0;
		std::vector<Variant> variants;
	};

	bool mHasEpochBuild = false;
	uint64_t mMapEpoch = 0;
	uint64_t mBuildSerial = 0;
	uint64_t mAccessSerial = 0;
	std::vector<Record> mRecords;
	PTMapMaterialStateVariantStats mStats;
};

const char* GetPTMapMaterialStateVariantDecisionName(PTMapMaterialStateVariantDecision decision);
}
