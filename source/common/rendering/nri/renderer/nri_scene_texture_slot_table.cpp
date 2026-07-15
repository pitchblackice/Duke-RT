#include "nri_scene_texture_slot_table.h"

#include <algorithm>

NRISceneTextureSlotTable::NRISceneTextureSlotTable(uint32_t capacity)
	: slots(capacity)
{
	keyToSlot.reserve(capacity);
}

bool NRISceneTextureSlotTable::UpdateActiveKeys(
	const std::vector<uint64_t>& keys,
	uint64_t currentSerial,
	uint64_t completedSerial)
{
	if (keys == lastInputKeys)
	{
		return true;
	}
	std::vector<uint64_t> activeKeys;
	activeKeys.reserve(keys.size());
	for (uint64_t key : keys)
	{
		if (key != 0)
			activeKeys.push_back(key);
	}
	std::sort(activeKeys.begin(), activeKeys.end());
	activeKeys.erase(std::unique(activeKeys.begin(), activeKeys.end()), activeKeys.end());

	size_t liveSlotCount = 0;
	for (const SlotRecord& record : slots)
	{
		if (record.key != 0 && !record.quarantined)
			++liveSlotCount;
	}
	bool exactLiveSet = activeKeys.size() == liveSlotCount;
	if (exactLiveSet)
	{
		for (uint64_t key : activeKeys)
		{
			const auto found = keyToSlot.find(key);
			if (found == keyToSlot.end() || slots[found->second].quarantined)
			{
				exactLiveSet = false;
				break;
			}
		}
	}
	if (exactLiveSet)
	{
		lastInputKeys = keys;
		return true;
	}

	if (activeKeys.size() > slots.size())
	{
		++exhaustionCount;
		return false;
	}

	NRISceneTextureSlotTable candidate(*this);

	// Reactivation precedes collection so a requested key can cancel retirement
	// even when this update also advances the completed serial past its fence.
	for (uint64_t key : activeKeys)
	{
		auto found = candidate.keyToSlot.find(key);
		if (found != candidate.keyToSlot.end())
		{
			SlotRecord& record = candidate.slots[found->second];
			record.quarantined = false;
			record.retireSerial = 0;
		}
	}

	for (SlotRecord& record : candidate.slots)
	{
		if (record.key != 0 && !record.quarantined &&
			!std::binary_search(activeKeys.begin(), activeKeys.end(), record.key))
		{
			record.quarantined = true;
			record.retireSerial = currentSerial;
		}
	}

	for (SlotRecord& record : candidate.slots)
	{
		if (record.quarantined && completedSerial >= record.retireSerial)
		{
			record.key = 0;
			record.retireSerial = 0;
			record.quarantined = false;
			candidate.mappingRevision = NextGeneration(candidate.mappingRevision);
		}
	}
	candidate.RebuildLookup();

	for (uint64_t key : activeKeys)
	{
		if (candidate.keyToSlot.find(key) != candidate.keyToSlot.end())
			continue;

		auto freeSlot = std::find_if(
			candidate.slots.begin(),
			candidate.slots.end(),
			[](const SlotRecord& record) { return record.key == 0; });
		if (freeSlot == candidate.slots.end())
		{
			++exhaustionCount;
			return false;
		}
		const size_t slot = static_cast<size_t>(freeSlot - candidate.slots.begin());
		candidate.Assign(slot, key);
		candidate.keyToSlot.emplace(key, slot);
	}

	slots.swap(candidate.slots);
	keyToSlot.swap(candidate.keyToSlot);
	lastInputKeys = keys;
	reuseCount = candidate.reuseCount;
	generationAdvanceCount = candidate.generationAdvanceCount;
	highestGeneration = candidate.highestGeneration;
	mappingRevision = candidate.mappingRevision;
	return true;
}

bool NRISceneTextureSlotTable::EnsureActiveKeys(
	const std::vector<uint64_t>& keys,
	uint64_t completedSerial)
{
	std::vector<uint64_t> requestedKeys;
	requestedKeys.reserve(keys.size());
	for (uint64_t key : keys)
	{
		if (key != 0)
			requestedKeys.push_back(key);
	}
	std::sort(requestedKeys.begin(), requestedKeys.end());
	requestedKeys.erase(std::unique(requestedKeys.begin(), requestedKeys.end()), requestedKeys.end());
	if (requestedKeys.size() > slots.size())
	{
		++exhaustionCount;
		return false;
	}

	NRISceneTextureSlotTable candidate(*this);
	for (uint64_t key : requestedKeys)
	{
		auto found = candidate.keyToSlot.find(key);
		if (found != candidate.keyToSlot.end())
		{
			SlotRecord& record = candidate.slots[found->second];
			record.quarantined = false;
			record.retireSerial = 0;
		}
	}
	for (SlotRecord& record : candidate.slots)
	{
		if (record.quarantined && completedSerial >= record.retireSerial)
		{
			record.key = 0;
			record.retireSerial = 0;
			record.quarantined = false;
			candidate.mappingRevision = NextGeneration(candidate.mappingRevision);
		}
	}
	candidate.RebuildLookup();
	for (uint64_t key : requestedKeys)
	{
		if (candidate.keyToSlot.find(key) != candidate.keyToSlot.end())
			continue;
		auto freeSlot = std::find_if(candidate.slots.begin(), candidate.slots.end(),
			[](const SlotRecord& record) { return record.key == 0; });
		if (freeSlot == candidate.slots.end())
		{
			++exhaustionCount;
			return false;
		}
		const size_t slot = static_cast<size_t>(freeSlot - candidate.slots.begin());
		candidate.Assign(slot, key);
		candidate.keyToSlot.emplace(key, slot);
	}

	slots.swap(candidate.slots);
	keyToSlot.swap(candidate.keyToSlot);
	lastInputKeys.clear();
	reuseCount = candidate.reuseCount;
	generationAdvanceCount = candidate.generationAdvanceCount;
	highestGeneration = candidate.highestGeneration;
	mappingRevision = candidate.mappingRevision;
	return true;
}

NRISceneTextureSlotHandle NRISceneTextureSlotTable::Lookup(uint64_t key) const
{
	auto found = keyToSlot.find(key);
	if (found == keyToSlot.end())
		return {};
	const SlotRecord& record = slots[found->second];
	if (record.quarantined)
		return {};
	return { static_cast<uint32_t>(found->second), record.generation };
}

bool NRISceneTextureSlotTable::IsCurrent(
	uint64_t key,
	NRISceneTextureSlotHandle handle) const
{
	return handle && Lookup(key) == handle;
}

bool NRISceneTextureSlotTable::Owns(
	uint64_t key,
	NRISceneTextureSlotHandle handle) const
{
	return handle && handle.slot < slots.size() &&
		slots[handle.slot].key == key && slots[handle.slot].generation == handle.generation;
}

NRISceneTextureSlotStats NRISceneTextureSlotTable::GetStats() const
{
	NRISceneTextureSlotStats stats = {};
	stats.capacity = slots.size();
	for (const SlotRecord& record : slots)
	{
		if (record.key == 0)
			++stats.free;
		else if (record.quarantined)
			++stats.quarantined;
		else
			++stats.live;
	}
	stats.reuseCount = reuseCount;
	stats.exhaustionCount = exhaustionCount;
	stats.generationAdvanceCount = generationAdvanceCount;
	stats.highestGeneration = highestGeneration;
	return stats;
}

void NRISceneTextureSlotTable::Reset()
{
	for (SlotRecord& record : slots)
	{
		record.key = 0;
		record.retireSerial = 0;
		record.quarantined = false;
	}
	keyToSlot.clear();
	lastInputKeys.clear();
	mappingRevision = NextGeneration(mappingRevision);
}

uint64_t NRISceneTextureSlotTable::NextGeneration(uint64_t generation)
{
	++generation;
	if (generation == 0)
		generation = 1;
	return generation;
}

void NRISceneTextureSlotTable::Assign(size_t slot, uint64_t key)
{
	SlotRecord& record = slots[slot];
	if (record.hasBeenAssigned)
		++reuseCount;
	record.hasBeenAssigned = true;
	record.key = key;
	record.generation = NextGeneration(record.generation);
	record.retireSerial = 0;
	record.quarantined = false;
	++generationAdvanceCount;
	mappingRevision = NextGeneration(mappingRevision);
	highestGeneration = std::max(highestGeneration, record.generation);
}

void NRISceneTextureSlotTable::RebuildLookup()
{
	keyToSlot.clear();
	for (size_t slot = 0; slot < slots.size(); ++slot)
	{
		if (slots[slot].key != 0)
			keyToSlot.emplace(slots[slot].key, slot);
	}
}
