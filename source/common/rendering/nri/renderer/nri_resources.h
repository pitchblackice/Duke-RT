#pragma once

#include "../system/nri_local.h"

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
