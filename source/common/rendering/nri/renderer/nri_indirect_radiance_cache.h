#pragma once

#include "nri_resources.h"

#include <array>
#include <cstdint>

class NRIRenderer;

struct NRIIndirectRadianceCacheCompatibilityInput
{
	bool valid = false;
	uint64_t mapIdentity = 0;
	uint64_t staticSceneIdentity = 0;
	uint64_t portalRouteIdentity = 0;
	uint64_t materialIdentity = 0;
	uint64_t mutationIdentity = 0;
	uint64_t voxelOccurrenceIdentity = 0;
	uint64_t lightingIdentity = 0;
};

struct NRIIndirectRadianceCacheCompatibilitySnapshot
{
	bool valid = false;
	uint64_t hash = 0;
	NRIIndirectRadianceCacheCompatibilityInput input = {};
};

enum NRIIndirectRadianceCacheInvalidationBits : uint32_t
{
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE = 0,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_FIRST_USE = 1u << 0,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_MAP = 1u << 1,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_STATIC_SCENE = 1u << 2,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_PORTAL_ROUTE = 1u << 3,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_MATERIAL = 1u << 4,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_MUTATION = 1u << 5,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_VOXEL_OCCURRENCE = 1u << 6,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_LIGHTING = 1u << 7,
	NRI_INDIRECT_RADIANCE_CACHE_INVALID_INPUT = 1u << 8,
};

NRIIndirectRadianceCacheCompatibilitySnapshot BuildNRIIndirectRadianceCacheCompatibilitySnapshot(
	const NRIIndirectRadianceCacheCompatibilityInput& input);
uint32_t CompareNRIIndirectRadianceCacheCompatibility(
	const NRIIndirectRadianceCacheCompatibilitySnapshot& previous,
	const NRIIndirectRadianceCacheCompatibilitySnapshot& current);

struct NRIIndirectRadianceCacheFenceServices
{
	using GetRecordingCommandFenceValueFn = uint64_t (*)(void* user);
	using IsCommandFenceValueCompleteFn = bool (*)(void* user, uint64_t fenceValue);
	using IsCommandFenceValueAbandonedFn = bool (*)(void* user, uint64_t fenceValue);

	void* user = nullptr;
	GetRecordingCommandFenceValueFn getRecordingCommandFenceValue = nullptr;
	IsCommandFenceValueCompleteFn isCommandFenceValueComplete = nullptr;
	IsCommandFenceValueAbandonedFn isCommandFenceValueAbandoned = nullptr;

	uint64_t GetRecordingCommandFenceValue() const;
	bool IsCommandFenceValueComplete(uint64_t fenceValue) const;
	bool IsCommandFenceValueAbandoned(uint64_t fenceValue) const;
};

struct NRIIndirectRadianceCacheServices
{
	NRIResourceServices resources;
	nri::DescriptorPool* descriptorPool = nullptr;
	nri::PipelineLayout* pipelineLayout = nullptr;
	nri::Descriptor* fallbackStorageDescriptor = nullptr;
	NRIIndirectRadianceCacheFenceServices fences;
};

NRIIndirectRadianceCacheServices BuildNRIIndirectRadianceCacheServices(NRIRenderer& renderer);

struct NRIIndirectRadianceCachePrepareResult
{
	bool active = false;
	bool clearRequired = false;
	uint32_t invalidationMask = NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE;
	uint32_t readTableIndex = 0;
	uint32_t writeTableIndex = 1;
	nri::DescriptorSet* descriptorSet = nullptr;
};

struct NRIIndirectRadianceCacheTelemetrySnapshot
{
	bool valid = false;
	uint64_t frameNumber = 0;
	uint64_t lookupCount = 0;
	uint64_t acceptedHitCount = 0;
	uint64_t forcedMissCount = 0;
	uint64_t collisionCount = 0;
	uint64_t staleGenerationCount = 0;
	uint64_t unsupportedRouteCount = 0;
	uint64_t exactFallbackCount = 0;
	uint64_t occupancy = 0;
	uint64_t updateCount = 0;
	uint64_t clearCount = 0;
	// Shader sub-operation ticks are optional and may remain zero while cache
	// work shares the monolithic trace dispatch. Matched TraceDispatch A/B is
	// the authoritative cost measurement.
	uint64_t lookupTimeTicks = 0;
	uint64_t updateTimeTicks = 0;
	uint64_t resolveTimeTicks = 0;
	uint64_t clearTimeTicks = 0;
	uint64_t tableMemoryBytes = 0;
	uint64_t totalMemoryBytes = 0;
	uint32_t invalidationMask = NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE;
	uint32_t pendingReadbacks = 0;
};

class NRIIndirectRadianceCache
{
public:
	static constexpr uint32_t MinimumEntryCount = 131072;
	static constexpr uint32_t DefaultEntryCount = 262144;
	static constexpr uint32_t MaximumEntryCount = 262144;

	NRIIndirectRadianceCachePrepareResult Prepare(
		const NRIIndirectRadianceCacheServices& services,
		bool enabled,
		const NRIIndirectRadianceCacheCompatibilityInput& compatibility,
		uint32_t entryCount = DefaultEntryCount);
	bool RecordPendingClear(const NRIIndirectRadianceCacheServices& services);
	void AdvanceFrame(const NRIIndirectRadianceCacheServices& services);
	void CopyTelemetryForReadback(
		const NRIIndirectRadianceCacheServices& services,
		uint64_t frameNumber);
	void ReadbackTelemetry(
		const NRIIndirectRadianceCacheServices& services,
		bool enabled,
		NRIIndirectRadianceCacheTelemetrySnapshot& outSnapshot);
	void Destroy(const NRIResourceServices& services);

	bool IsAllocated() const { return mTables[0].buffer != nullptr || mTables[1].buffer != nullptr; }
	bool IsActive() const { return mActive; }
	uint64_t GetTableMemoryBytes() const;
	uint64_t GetTotalMemoryBytes() const;
	const NRIIndirectRadianceCacheCompatibilitySnapshot& GetCompatibilitySnapshot() const { return mCompatibility; }
	nri::DescriptorSet* GetDescriptorSet() const { return mDescriptorSets[mReadTableIndex]; }

private:
	static constexpr uint32_t ReadbackSlotCount = 3;
	static constexpr uint32_t FrameCommitSlotCount = 8;

	struct ReadbackSlot
	{
		NRIBufferResource buffer;
		uint64_t frameNumber = 0;
		uint64_t fenceValue = 0;
		uint64_t copySerial = 0;
		bool pending = false;
		bool initialized = false;
	};

	struct FrameCommitSlot
	{
		uint64_t fenceValue = 0;
		bool pending = false;
		bool recordedClear = false;
		bool initializedBeforeClear = false;
	};

	bool EnsureResources(
		const NRIIndirectRadianceCacheServices& services,
		uint32_t entryCount);
	bool UpdateDescriptorSets(const NRIIndirectRadianceCacheServices& services, bool useCacheTables);
	void ReconcileFrameCommits(const NRIIndirectRadianceCacheServices& services);
	void ClearReadbackSlot(uint32_t slotIndex);

	std::array<NRIBufferResource, 2> mTables = {};
	NRIBufferResource mTelemetryBuffer;
	NRIBufferResource mClearUploadBuffer;
	std::array<ReadbackSlot, ReadbackSlotCount> mReadbackSlots = {};
	std::array<FrameCommitSlot, FrameCommitSlotCount> mFrameCommitSlots = {};
	std::array<nri::DescriptorSet*, 2> mDescriptorSets = {};
	NRIIndirectRadianceCacheCompatibilitySnapshot mCompatibility = {};
	uint64_t mNextCopySerial = 1;
	uint64_t mClearCount = 0;
	uint32_t mEntryCount = 0;
	uint32_t mReadTableIndex = 0;
	uint32_t mNextReadbackSlot = 0;
	uint32_t mLastInvalidationMask = NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE;
	bool mActive = false;
	bool mClearPending = false;
	bool mGpuBuffersInitialized = false;
	bool mCurrentFrameRecordedClear = false;
	bool mCurrentFrameInitializedBeforeClear = false;
};
