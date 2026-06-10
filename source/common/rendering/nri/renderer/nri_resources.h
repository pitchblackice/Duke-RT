#pragma once

#include "../system/nri_local.h"

struct NRIResourceContext
{
	nri::Device* device = nullptr;
	const nri::CoreInterface* core = nullptr;
	nri::CommandBuffer* commandBuffer = nullptr;
};

struct NRIBufferResource
{
	nri::Buffer* buffer = nullptr;
	nri::Descriptor* shaderView = nullptr;
	uint64_t size = 0;
	uint64_t memorySize = 0;
	uint64_t usedSize = 0;
	uint64_t payloadHash = 0;
	uint64_t payloadSize = 0;
	uint32_t stride = 0;
	uint32_t payloadStride = 0;
	nri::MemoryLocation memoryLocation = nri::MemoryLocation::DEVICE;
};

struct NRIResourceServices
{
	using WaitForCommandsFn = void (*)(void* user, const char* reason);
	using DestroyBufferResourceFn = void (*)(void* user, NRIBufferResource& resource);

	NRIResourceContext context;
	void* user = nullptr;
	WaitForCommandsFn waitForCommands = nullptr;
	DestroyBufferResourceFn destroyBufferResource = nullptr;

	void WaitForCommands(const char* reason = nullptr) const
	{
		if (waitForCommands != nullptr)
		{
			waitForCommands(user, reason);
		}
	}

	void DestroyBufferResource(NRIBufferResource& resource) const
	{
		if (destroyBufferResource != nullptr)
		{
			destroyBufferResource(user, resource);
		}
	}
};

struct NRIAccelerationStructureResource
{
	nri::AccelerationStructure* accelerationStructure = nullptr;
	nri::Descriptor* descriptor = nullptr;
	uint64_t memorySize = 0;
	uint64_t buildScratchSize = 0;
	nri::Buffer* buildVertexBuffer = nullptr;
	nri::Buffer* buildIndexBuffer = nullptr;
	uint32_t buildVertexCount = 0;
	uint32_t buildIndexOffset = 0;
	uint32_t buildIndexCount = 0;
	uint32_t buildPrimitiveCount = 0;
	nri::MemoryLocation memoryLocation = nri::MemoryLocation::DEVICE;
};

uint64_t GetNRIGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride);
uint64_t GetNRISceneUploadGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride);
