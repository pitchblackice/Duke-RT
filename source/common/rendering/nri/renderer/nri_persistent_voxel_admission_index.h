#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

enum class NRIPersistentVoxelAdmissionBucket : uint8_t
{
	Active,
	RequiredReady,
	OptionalReady,
	OptionalDeferred,
	Failed,
};

class NRIPersistentVoxelAdmissionIndex
{
public:
	void Clear();
	void Add(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket);
	void Transition(
		uint64_t key,
		NRIPersistentVoxelAdmissionBucket oldBucket,
		NRIPersistentVoxelAdmissionBucket newBucket);
	void Remove(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket);

	const std::unordered_set<uint64_t>& ActiveKeys() const { return activeKeys; }
	bool HasActiveWork() const { return !activeKeys.empty(); }
	uint32_t ActiveCount() const { return (uint32_t)activeKeys.size(); }
	uint32_t RequiredReadyCount() const { return requiredReadyCount; }
	uint32_t OptionalReadyCount() const { return optionalReadyCount; }
	uint32_t OptionalDeferredCount() const { return optionalDeferredCount; }
	uint32_t FailedCount() const { return failedCount; }

private:
	void AddToBucket(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket);
	void RemoveFromBucket(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket);

	std::unordered_set<uint64_t> activeKeys;
	std::unordered_map<uint64_t, NRIPersistentVoxelAdmissionBucket> bucketsByKey;
	uint32_t requiredReadyCount = 0;
	uint32_t optionalReadyCount = 0;
	uint32_t optionalDeferredCount = 0;
	uint32_t failedCount = 0;
};
