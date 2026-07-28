#pragma once

#include "nri_resources.h"
#include "nri_smoke_dormant_grid_contracts.h"
#include "nri_smoke_grid.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Writable fine-grid descriptors in the exact order consumed by
// SmokeDormantGridResources.hlsli. The grid remains authoritative until the
// archive shader publishes a coarse record and reports Archived.
struct NRISmokeDormantGridFineDescriptors
{
	std::array<const nri::Descriptor*, NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT> storage = {};
	std::array<nri::Buffer*, NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT> buffers = {};
	uint32_t brickCapacity = 0u;
	uint32_t hashCapacity = 0u;
	uint32_t cellCapacity = 0u;
	uint32_t fieldPing = 0u;
	uint32_t activePing = 0u;
	float cellSize = 8.0f;

	bool IsValid() const;
};

struct NRISmokeDormantGridConfig
{
	bool enabled = false;
	uint32_t archiveCapacity = 256u;
	uint32_t maximumDemotionsPerFrame = 4u;
	uint32_t maximumPromotionsPerFrame = 4u;
	uint32_t maximumEvolutionPerFrame = 0u;
	float opticalMassRelativeTolerance = 0.25f;
};

struct NRISmokeDormantGridFrameDesc
{
	uint32_t frameIndex = 0u;
	uint32_t simulationEpoch = 0u;
	float deltaTime = 0.0f;
	float cameraPosition[3] = {};
	NRISmokeDormantGridFineDescriptors fine = {};
	const NRISmokeDormantGridWorkGpu* demotions = nullptr;
	uint32_t demotionCount = 0u;
	const NRISmokeDormantGridWorkGpu* promotions = nullptr;
	uint32_t promotionCount = 0u;
};

struct NRISmokeDormantGridStatusSnapshot
{
	bool requested = false;
	bool initialized = false;
	bool resourcesReady = false;
	bool gpuStatsValid = false;
	uint64_t gpuRendererFrame = UINT64_MAX;
	uint32_t epoch = 0u;
	uint32_t archiveCapacity = 0u;
	uint32_t hashCapacity = 0u;
	uint32_t maximumWork = 0u;
	uint64_t residentBytes = 0u;
	uint64_t payloadBytes = 0u;
	uint64_t readbackBytes = 0u;
	uint32_t submittedDemotions = 0u;
	uint32_t submittedPromotions = 0u;
	uint32_t clippedDemotions = 0u;
	uint32_t clippedPromotions = 0u;
	NRISmokeDormantGridControlGpu gpu = {};
	std::vector<NRISmokeDormantGridResultGpu> results;
	std::string failureReason = "not-requested";
	std::string resetReason = "initial";
};

class NRISmokeDormantGrid
{
public:
	static_assert(NRISmokeGrid::DormantTransactionDescriptorCount ==
		NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT,
		"fine-grid transaction descriptor contracts must stay aligned");
	static constexpr uint32_t FineDescriptorCount = NRI_SMOKE_DORMANT_GRID_FINE_DESCRIPTOR_COUNT;
	static constexpr uint32_t StorageDescriptorCount = NRI_SMOKE_DORMANT_GRID_STORAGE_DESCRIPTOR_COUNT;
	static constexpr uint32_t EvaluationDescriptorCount = NRI_SMOKE_DORMANT_GRID_EVALUATION_DESCRIPTOR_COUNT;

	bool Initialize(const NRISmokeGridServices& services);
	bool PrepareFrame(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config, uint32_t simulationEpoch);
	bool RecordFrame(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame);
	bool RecordPromotions(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame);
	bool RecordDemotions(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame);
	bool RecordReadback(const NRISmokeGridServices& services,
		const NRISmokeDormantGridFrameDesc& frame);
	void Reset(uint32_t simulationEpoch, const char* reason);
	void Shutdown(const NRISmokeGridServices& services);

	// control, hash, records, scalar, velocity, optical, dynamics. These are
	// sufficient for a materializer to find and sample coarse authority.
	bool GetEvaluationStorageDescriptors(
		std::array<const nri::Descriptor*, EvaluationDescriptorCount>& descriptors) const;
	const NRISmokeDormantGridStatusSnapshot& GetStatusSnapshot() const { return mStatus; }

private:
	struct FrameSlot
	{
		NRIBufferResource demotionUpload;
		NRIBufferResource promotionUpload;
		NRIBufferResource controlReadback;
		NRIBufferResource resultReadback;
		bool readbackPending = false;
		uint64_t readbackRendererFrame = UINT64_MAX;
		uint32_t readbackEpoch = 0u;
		uint32_t resultCount = 0u;
		uint32_t promotionResultCount = 0u;
		uint32_t demotionResultCount = 0u;
		uint64_t workRendererFrame = UINT64_MAX;
	};

	bool EnsureResources(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config);
	bool CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
		uint64_t size, uint32_t stride, nri::BufferUsageBits usage,
		nri::MemoryLocation location, bool storageView);
	void DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource);
	void DestroyResources(const NRISmokeGridServices& services);
	void ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch);
	void SetFailure(const char* reason);
	void TransitionArchiveToStorage(const NRISmokeGridServices& services);
	void StorageBarrier(const NRISmokeGridServices& services,
		const NRISmokeDormantGridFineDescriptors& fine);
	void Dispatch(const NRISmokeGridServices& services, NRISmokeDormantGridConstants& constants,
		NRISmokeDormantGridPass pass, uint32_t x);
	bool RecordStage(const NRISmokeGridServices& services,
		const NRISmokeDormantGridConfig& config, const NRISmokeDormantGridFrameDesc& frame,
		bool promotions);

	std::array<NRIBufferResource*, StorageDescriptorCount> StorageResources();
	std::array<const NRIBufferResource*, StorageDescriptorCount> StorageResources() const;

	NRISmokeDormantGridStatusSnapshot mStatus = {};
	nri::PipelineLayout* mPipelineLayout = nullptr;
	std::array<nri::Pipeline*, 4> mPipelines = {};
	nri::DescriptorSet* mFineSet = nullptr;
	nri::DescriptorSet* mStorageSet = nullptr;
	std::vector<FrameSlot> mFrameSlots;

	NRIBufferResource mControl;
	NRIBufferResource mHash;
	NRIBufferResource mRecords;
	NRIBufferResource mFreeList;
	NRIBufferResource mScalar;
	NRIBufferResource mVelocity;
	NRIBufferResource mOptical;
	NRIBufferResource mDynamics;
	NRIBufferResource mDemotions;
	NRIBufferResource mPromotions;
	NRIBufferResource mResults;

	uint32_t mResourceArchiveCapacity = 0u;
	uint32_t mResourceHashCapacity = 0u;
	uint32_t mResourceMaximumWork = 0u;
	uint32_t mResourceEpoch = 0u;
	bool mInitialized = false;
	bool mResourcesInitialized = false;
	bool mNeedsClear = true;
};
