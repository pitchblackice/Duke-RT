#pragma once

#include "nri_smoke_grid.h"
#include "nri_smoke_view_work_contracts.h"

#include <array>
#include <vector>

struct NRISmokeViewWorkFrame
{
	NRISmokeViewWorkConstants constants = {};
	// Existing grid descriptors: control, bricks, optical A, optical B.
	std::array<const nri::Descriptor*, 4> gridDescriptors = {};
};

struct NRISmokeViewWorkOutputs
{
	const nri::Descriptor* columnMasks = nullptr;
	const nri::Descriptor* compactIndices = nullptr;
	const nri::Descriptor* control = nullptr;
	const nri::Descriptor* indirectArgs = nullptr;
	uint32_t columnCount = 0;
	uint32_t froxelCapacity = 0;
};

struct NRISmokeViewWorkStatusSnapshot
{
	bool initialized = false;
	bool resourcesReady = false;
	bool gpuStatsValid = false;
	NRISmokeViewWorkLayout layout = {};
	NRISmokeViewWorkControlGpu gpu = {};
	uint64_t gpuRendererFrame = UINT64_MAX;
	uint64_t residentBytes = 0;
	const char* failureReason = "not-initialized";
};

// Owns bounded sparse-view preparation only. The current dense evaluation
// remains authoritative until a later integration checkpoint validates masks
// against it and explicitly selects an execution route.
class NRISmokeViewWork
{
public:
	static constexpr uint32_t StorageDescriptorCount = 11u;

	static NRISmokeViewWorkLayout Describe(uint32_t froxelWidth, uint32_t froxelHeight,
		uint32_t froxelDepth, uint32_t brickCapacity);

	bool Initialize(const NRISmokeGridServices& services);
	bool Prepare(const NRISmokeGridServices& services, const NRISmokeViewWorkFrame& frame);
	bool CompareDense(const NRISmokeGridServices& services,
		const nri::Descriptor* denseMedium, const nri::Descriptor* denseSource);
	void Finish(const NRISmokeGridServices& services);
	void PrintStatus(bool compareRequested, uint32_t routeRequested) const;
	void Shutdown(const NRISmokeGridServices& services);

	bool GetOutputs(NRISmokeViewWorkOutputs& outputs) const;
	const NRISmokeViewWorkStatusSnapshot& GetStatusSnapshot() const { return mStatus; }

private:
	bool EnsureResources(const NRISmokeGridServices& services, const NRISmokeViewWorkLayout& layout,
		uint32_t froxelDepth);
	bool CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out, uint64_t size,
		uint32_t stride, nri::BufferUsageBits usage);
	void DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource);
	void DestroyResources(const NRISmokeGridServices& services);
	void TransitionOutputsToStorage(const NRISmokeGridServices& services);
	void StorageBarrier(const NRISmokeGridServices& services);
	void Dispatch(const NRISmokeGridServices& services, NRISmokeViewWorkPass pass,
		const NRISmokeViewWorkConstants& constants, uint32_t groups);
	bool CreateReadback(const NRISmokeGridServices& services, NRIBufferResource& out);
	void ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch);
	void RecordReadback(const NRISmokeGridServices& services);
	void SetFailure(const char* reason);
	struct FrameSlot
	{
		NRIBufferResource controlReadback;
		bool pending = false;
		bool initialized = false;
		uint64_t rendererFrame = UINT64_MAX;
		uint32_t simulationEpoch = 0;
	};

	NRISmokeViewWorkStatusSnapshot mStatus = {};
	nri::PipelineLayout* mPipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)NRISmokeViewWorkPass::Count> mPipelines = {};
	nri::DescriptorSet* mStorageSet = nullptr;
	NRIBufferResource mTileMasks;
	NRIBufferResource mColumnMasks;
	NRIBufferResource mControl;
	NRIBufferResource mCompactIndices;
	NRIBufferResource mIndirectArgs;
	std::vector<FrameSlot> mFrameSlots;
	NRISmokeViewWorkConstants mLastConstants = {};
	uint32_t mResourceFroxelDepth = 0;
	bool mResourcesInitialized = false;
};
