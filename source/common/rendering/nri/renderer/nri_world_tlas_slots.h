#pragma once

#include "nri_resources.h"

#include <cstdint>
#include <vector>

struct NRIWorldTlasFrameSlot
{
	NRIAccelerationStructureResource accelerationStructure;
	NRIBufferResource instanceBuffer;
	NRIBufferResource scratchBuffer;
};

struct NRIWorldTlasSlotLifecycleServices
{
	void* user = nullptr;
	void (*destroyBufferResource)(void* user, NRIBufferResource& resource) = nullptr;
	void (*destroyAccelerationStructureResource)(void* user, NRIAccelerationStructureResource& resource) = nullptr;
};

struct NRIWorldTlasSlotMemoryUsage
{
	uint64_t instanceBufferBytes = 0;
	uint64_t scratchBufferBytes = 0;
	uint64_t accelerationStructureBytes = 0;
};

class NRIWorldTlasFrameSlots
{
public:
	NRIWorldTlasFrameSlot& Get(uint32_t frameSlotIndex, uint32_t frameSlotCount);
	const NRIWorldTlasFrameSlot* Find(uint32_t frameSlotIndex) const;
	void EnsureSlotCount(uint32_t frameSlotCount);

	std::vector<NRIWorldTlasFrameSlot>& Slots() { return mSlots; }
	const std::vector<NRIWorldTlasFrameSlot>& Slots() const { return mSlots; }
	bool HasResources() const;
	NRIWorldTlasSlotMemoryUsage GetMemoryUsage() const;

	void DestroyScratchBuffers(const NRIWorldTlasSlotLifecycleServices& services);
	void Destroy(const NRIWorldTlasSlotLifecycleServices& services);

private:
	std::vector<NRIWorldTlasFrameSlot> mSlots;
};
