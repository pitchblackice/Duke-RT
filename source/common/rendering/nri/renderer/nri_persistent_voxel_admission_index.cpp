#include "nri_persistent_voxel_admission_index.h"

void NRIPersistentVoxelAdmissionIndex::Clear()
{
	activeKeys.clear();
	bucketsByKey.clear();
	requiredReadyCount = 0;
	optionalReadyCount = 0;
	failedCount = 0;
}

void NRIPersistentVoxelAdmissionIndex::Add(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket)
{
	const auto existing = bucketsByKey.find(key);
	if (existing != bucketsByKey.end())
	{
		if (existing->second == bucket)
		{
			return;
		}
		RemoveFromBucket(key, existing->second);
		existing->second = bucket;
		AddToBucket(key, bucket);
		return;
	}
	bucketsByKey.emplace(key, bucket);
	AddToBucket(key, bucket);
}

void NRIPersistentVoxelAdmissionIndex::Transition(
	uint64_t key,
	NRIPersistentVoxelAdmissionBucket oldBucket,
	NRIPersistentVoxelAdmissionBucket newBucket)
{
	const auto existing = bucketsByKey.find(key);
	if (existing == bucketsByKey.end())
	{
		Add(key, newBucket);
		return;
	}
	if (existing->second == newBucket)
	{
		return;
	}
	(void)oldBucket;
	RemoveFromBucket(key, existing->second);
	existing->second = newBucket;
	AddToBucket(key, newBucket);
}

void NRIPersistentVoxelAdmissionIndex::Remove(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket)
{
	const auto existing = bucketsByKey.find(key);
	if (existing == bucketsByKey.end())
	{
		return;
	}
	(void)bucket;
	RemoveFromBucket(key, existing->second);
	bucketsByKey.erase(existing);
}

void NRIPersistentVoxelAdmissionIndex::AddToBucket(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket)
{
	switch (bucket)
	{
	case NRIPersistentVoxelAdmissionBucket::Active:
		activeKeys.insert(key);
		break;
	case NRIPersistentVoxelAdmissionBucket::RequiredReady:
		requiredReadyCount++;
		break;
	case NRIPersistentVoxelAdmissionBucket::OptionalReady:
		optionalReadyCount++;
		break;
	case NRIPersistentVoxelAdmissionBucket::Failed:
		failedCount++;
		break;
	}
}

void NRIPersistentVoxelAdmissionIndex::RemoveFromBucket(uint64_t key, NRIPersistentVoxelAdmissionBucket bucket)
{
	switch (bucket)
	{
	case NRIPersistentVoxelAdmissionBucket::Active:
		activeKeys.erase(key);
		break;
	case NRIPersistentVoxelAdmissionBucket::RequiredReady:
		if (requiredReadyCount != 0)
		{
			requiredReadyCount--;
		}
		break;
	case NRIPersistentVoxelAdmissionBucket::OptionalReady:
		if (optionalReadyCount != 0)
		{
			optionalReadyCount--;
		}
		break;
	case NRIPersistentVoxelAdmissionBucket::Failed:
		if (failedCount != 0)
		{
			failedCount--;
		}
		break;
	}
}
