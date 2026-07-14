#pragma once

#include "nri_smoke_grid.h"
#include "nri_smoke_grid_lighting_contracts.h"
#include "nri_smoke_contracts.h"

#include <array>

struct NRISmokeGridLightingDirectSeedSnapshot
{
	const nri::Descriptor* current = nullptr;
	const nri::Descriptor* history = nullptr;
	const nri::Descriptor* activeCells = nullptr;
	const nri::Descriptor* links = nullptr;
	const nri::Descriptor* control = nullptr;
	const nri::Descriptor* filtered = nullptr;
	const nri::Descriptor* gridControl = nullptr;
	const nri::Descriptor* gridHash = nullptr;
	const nri::Descriptor* gridBricks = nullptr;
	const nri::Descriptor* scalarA = nullptr;
	const nri::Descriptor* scalarB = nullptr;
	const nri::Descriptor* opticalA = nullptr;
	const nri::Descriptor* opticalB = nullptr;
	uint32_t cellCapacity = 0;
	uint32_t fieldPing = 0;
	uint32_t simulationEpoch = 0;
	float cellSize = 0.0f;
};

class NRISmokeGridLighting
{
public:
	static constexpr uint32_t StorageDescriptorCount = 6u;

	bool Initialize(const NRISmokeGridServices& services, nri::PipelineLayout* sharedLayout);
	bool PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
		uint32_t cellCapacity, uint32_t frameIndex, uint32_t simulationEpoch);
	bool Record(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
		NRISmokeConstants constants, bool emissiveResourcesReady);
	void PublishGridSnapshot(const std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount>& descriptors,
		uint32_t fieldPing, float cellSize);
	void Reset(uint32_t simulationEpoch, const char* reason);
	void Shutdown(const NRISmokeGridServices& services);

	bool GetStorageDescriptors(std::array<const nri::Descriptor*, StorageDescriptorCount>& descriptors) const;
	NRISmokeGridLightingDirectSeedSnapshot GetDirectSeedSnapshot() const;
	const NRISmokeGridLightingStatusSnapshot& GetStatusSnapshot() const { return mStatus; }
	bool IsWorldReady() const { return mStatus.resourcesReady; }
	uint32_t GetFieldPing() const { return mFieldPing; }

private:
	bool EnsureResources(const NRISmokeGridServices& services, uint32_t cellCapacity, bool filterRequested);
	bool CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out, uint64_t size,
		uint32_t stride, nri::BufferUsageBits usage);
	void DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource);
	void DestroyResources(const NRISmokeGridServices& services);
	void Barrier(const NRISmokeGridServices& services);
	void Dispatch(const NRISmokeGridServices& services, NRISmokeGridLightingPass pass,
		NRISmokeConstants& constants, uint32_t groups);

	NRISmokeGridLightingStatusSnapshot mStatus = {};
	std::array<nri::Pipeline*, (size_t)NRISmokeGridLightingPass::Count> mPipelines = {};
	nri::PipelineLayout* mSharedLayout = nullptr;
	NRIBufferResource mCurrent;
	NRIBufferResource mHistory;
	NRIBufferResource mActive;
	NRIBufferResource mControl;
	NRIBufferResource mLinks;
	NRIBufferResource mFiltered;
	uint32_t mResourceCellCapacity = 0;
	uint32_t mFieldPing = 0;
	uint32_t mLastRecordedFrame = UINT32_MAX;
	uint32_t mSimulationEpoch = 0;
	bool mNeedsClear = true;
	bool mResourcesInitialized = false;
	std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> mGridDescriptors = {};
	uint32_t mGridFieldPing = 0;
	float mGridCellSize = 0.0f;
};
