#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"

#include <array>
#include <cstdint>
#include <vector>

static constexpr uint32_t NRI_TRACE_SHADER_SCALAR_STAT_COUNT = 64;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT = 1024;
static constexpr uint32_t NRI_TRACE_SHADER_RAY_KIND_COUNT = 6;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE = NRI_TRACE_SHADER_SCALAR_STAT_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE = NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE = NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_STAT_COUNT =
	NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + NRI_TRACE_SHADER_RAY_KIND_COUNT * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_HOT_INSTANCE_COUNT = 8;

struct NRITraceShaderHotInstance
{
	uint32_t instanceId = 0;
	uint32_t dataSource = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	uint32_t committed = 0;
	uint32_t accepted = 0;
	uint32_t primaryCommitted = 0;
	uint32_t ungatedCommitted = 0;
	uint32_t sunCommitted = 0;
	uint32_t pointCommitted = 0;
	uint32_t emissiveCommitted = 0;
	uint32_t fastEmissiveCommitted = 0;
};

struct NRITraceShaderStatsSnapshot
{
	bool valid = false;
	uint64_t frameNumber = 0;
	std::array<uint32_t, NRI_TRACE_SHADER_STAT_COUNT> counters = {};
	uint32_t hotInstanceCount = 0;
	std::array<NRITraceShaderHotInstance, NRI_TRACE_SHADER_HOT_INSTANCE_COUNT> hotInstances = {};
};

struct NRITraceShaderStatsReadbackInput
{
	using EstimatePersistentVoxelPrimitiveCountFn = uint32_t (*)(void* user, uint32_t primitiveOffset);

	bool enabled = false;
	const std::vector<SceneInstanceData>* boundSceneInstances = nullptr;
	uint32_t staticPrimitiveCount = 0;
	uint32_t dynamicPrimitiveCount = 0;
	uint32_t persistentVoxelPrimitiveCount = 0;
	void* user = nullptr;
	EstimatePersistentVoxelPrimitiveCountFn estimatePersistentVoxelPrimitiveCount = nullptr;
};

class NRITraceShaderStats
{
public:
	bool Ensure(const NRIResourceServices& services);
	void Destroy(const NRIResourceServices& services);
	void ResetBuffer(const NRIResourceServices& services, bool enabled);
	void CopyForReadback(const NRIResourceServices& services, bool enabled, uint64_t frameNumber);
	void Readback(const NRIResourceServices& services, const NRITraceShaderStatsReadbackInput& input, NRITraceShaderStatsSnapshot& outStats);

	nri::Descriptor* Descriptor() const { return mStatsBuffer.shaderView; }

private:
	bool CreateBufferWithoutViewAtLocation(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation);

	NRIBufferResource mStatsBuffer;
	NRIBufferResource mReadbackBuffer;
	NRIBufferResource mZeroBuffer;
	uint64_t mPendingFrame = 0;
};
