#include "nri_world_tlas_slots.h"

#include <algorithm>

NRIWorldTlasFrameSlot& NRIWorldTlasFrameSlots::Get(uint32_t frameSlotIndex, uint32_t frameSlotCount)
{
	EnsureSlotCount(frameSlotCount);
	return mSlots[frameSlotIndex % (uint32_t)mSlots.size()];
}

const NRIWorldTlasFrameSlot* NRIWorldTlasFrameSlots::Find(uint32_t frameSlotIndex) const
{
	if (mSlots.empty())
	{
		return nullptr;
	}

	return &mSlots[frameSlotIndex % (uint32_t)mSlots.size()];
}

void NRIWorldTlasFrameSlots::EnsureSlotCount(uint32_t frameSlotCount)
{
	const uint32_t requiredSlotCount = std::max(frameSlotCount, 1u);
	if (mSlots.size() < requiredSlotCount)
	{
		mSlots.resize(requiredSlotCount);
	}
}

bool NRIWorldTlasFrameSlots::HasResources() const
{
	for (const NRIWorldTlasFrameSlot& slot : mSlots)
	{
		if (slot.accelerationStructure.accelerationStructure != nullptr ||
			slot.accelerationStructure.descriptor != nullptr ||
			slot.instanceBuffer.buffer != nullptr ||
			slot.scratchBuffer.buffer != nullptr)
		{
			return true;
		}
	}

	return false;
}

NRIWorldTlasSlotMemoryUsage NRIWorldTlasFrameSlots::GetMemoryUsage() const
{
	NRIWorldTlasSlotMemoryUsage usage;
	for (const NRIWorldTlasFrameSlot& slot : mSlots)
	{
		usage.instanceBufferBytes += slot.instanceBuffer.memorySize;
		usage.scratchBufferBytes += slot.scratchBuffer.memorySize;
		usage.accelerationStructureBytes += slot.accelerationStructure.memorySize;
	}
	return usage;
}

void NRIWorldTlasFrameSlots::DestroyScratchBuffers(const NRIWorldTlasSlotLifecycleServices& services)
{
	if (services.destroyBufferResource == nullptr)
	{
		return;
	}

	for (NRIWorldTlasFrameSlot& slot : mSlots)
	{
		services.destroyBufferResource(services.user, slot.scratchBuffer);
	}
}

void NRIWorldTlasFrameSlots::Destroy(const NRIWorldTlasSlotLifecycleServices& services)
{
	for (NRIWorldTlasFrameSlot& slot : mSlots)
	{
		if (services.destroyAccelerationStructureResource != nullptr)
		{
			services.destroyAccelerationStructureResource(services.user, slot.accelerationStructure);
		}
		if (services.destroyBufferResource != nullptr)
		{
			services.destroyBufferResource(services.user, slot.instanceBuffer);
			services.destroyBufferResource(services.user, slot.scratchBuffer);
		}
	}
	mSlots.clear();
}
