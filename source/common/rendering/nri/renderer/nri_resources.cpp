#include "nri_resources.h"

#include <algorithm>
#include <limits>

uint64_t GetNRIGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
{
	uint64_t newCapacity = std::max<uint64_t>(requiredSize, stride);
	if (currentCapacity >= newCapacity && currentCapacity != 0)
	{
		return currentCapacity;
	}

	if (currentCapacity != 0)
	{
		newCapacity = std::max(newCapacity, currentCapacity);
		while (newCapacity < requiredSize)
		{
			const uint64_t doubled = newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ? newCapacity * 2 : std::numeric_limits<uint64_t>::max();
			if (doubled <= newCapacity)
			{
				newCapacity = requiredSize;
				break;
			}
			newCapacity = doubled;
		}
	}

	return std::max<uint64_t>(newCapacity, stride);
}

uint64_t GetNRISceneUploadGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
{
	const uint64_t minimumCapacity = std::max<uint64_t>(requiredSize, stride);
	if (currentCapacity >= minimumCapacity && currentCapacity != 0)
	{
		return currentCapacity;
	}
	if (currentCapacity == 0)
	{
		return minimumCapacity;
	}

	uint64_t newCapacity = std::max<uint64_t>(currentCapacity, stride);
	while (newCapacity < minimumCapacity)
	{
		const uint64_t doubled =
			newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ?
			newCapacity * 2 :
			std::numeric_limits<uint64_t>::max();
		if (doubled <= newCapacity)
		{
			return minimumCapacity;
		}
		newCapacity = doubled;
	}
	return newCapacity;
}
