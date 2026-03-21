#pragma once

#include "../system/nri_local.h"

struct NRIBufferResource
{
	nri::Buffer* buffer = nullptr;
	nri::Descriptor* shaderView = nullptr;
	uint64_t size = 0;
	uint64_t usedSize = 0;
	uint32_t stride = 0;
};

struct NRIAccelerationStructureResource
{
	nri::AccelerationStructure* accelerationStructure = nullptr;
	nri::Descriptor* descriptor = nullptr;
};
