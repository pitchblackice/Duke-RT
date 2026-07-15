#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRISceneTextureSlotHandle
{
	uint32_t slot = 0;
	uint64_t generation = 0;

	explicit operator bool() const { return generation != 0; }
};

inline bool operator==(
	const NRISceneTextureSlotHandle& left,
	const NRISceneTextureSlotHandle& right)
{
	return left.slot == right.slot && left.generation == right.generation;
}

inline bool operator!=(
	const NRISceneTextureSlotHandle& left,
	const NRISceneTextureSlotHandle& right)
{
	return !(left == right);
}

struct NRISceneTextureSlotStats
{
	size_t capacity = 0;
	size_t live = 0;
	size_t quarantined = 0;
	size_t free = 0;
	uint64_t reuseCount = 0;
	uint64_t exhaustionCount = 0;
	uint64_t generationAdvanceCount = 0;
	uint64_t highestGeneration = 0;
};

// Stable-key slot ownership for scene textures. Removed slots remain quarantined
// until completedSerial >= retireSerial. That inclusive comparison is intentional:
// a serial reports all work through that serial complete.
class NRISceneTextureSlotTable
{
public:
	explicit NRISceneTextureSlotTable(uint32_t capacity = 512);

	// Zero keys are ignored and duplicate keys are folded. On exhaustion, slot
	// ownership and generations are unchanged; exhaustionCount still records the
	// failed attempt for telemetry.
	bool UpdateActiveKeys(
		const std::vector<uint64_t>& keys,
		uint64_t currentSerial,
		uint64_t completedSerial);
	// Adds/reactivates keys without retiring other owners. Subset scene builders
	// must use this so their local bridge cannot invalidate resident slots.
	bool EnsureActiveKeys(const std::vector<uint64_t>& keys, uint64_t completedSerial);

	NRISceneTextureSlotHandle Lookup(uint64_t key) const;
	bool IsCurrent(uint64_t key, NRISceneTextureSlotHandle handle) const;
	bool Owns(uint64_t key, NRISceneTextureSlotHandle handle) const;
	NRISceneTextureSlotStats GetStats() const;
	uint64_t MappingRevision() const { return mappingRevision; }

	// Invalidates all handles without resetting per-slot generation history or
	// lifetime telemetry.
	void Reset();

private:
	struct SlotRecord
	{
		uint64_t key = 0;
		uint64_t generation = 0;
		uint64_t retireSerial = 0;
		bool quarantined = false;
		bool hasBeenAssigned = false;
	};

	static uint64_t NextGeneration(uint64_t generation);
	void Assign(size_t slot, uint64_t key);
	void RebuildLookup();

	std::vector<SlotRecord> slots;
	std::unordered_map<uint64_t, size_t> keyToSlot;
	std::vector<uint64_t> lastInputKeys;
	uint64_t reuseCount = 0;
	uint64_t exhaustionCount = 0;
	uint64_t generationAdvanceCount = 0;
	uint64_t highestGeneration = 0;
	uint64_t mappingRevision = 1;
};
