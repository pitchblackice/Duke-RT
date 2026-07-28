#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct NRISmokeAnalyticCarrierRequest
{
	float position[3] = {};
	float initialRadius = 0.0f;
	float velocity[3] = {};
	float initialDensity = 0.0f;
	float halfAxisU[3] = {};
	uint32_t shape = 0u;
	float halfAxisV[3] = {};
	uint32_t rangeCount = 1u;
	float expansionVelocity = 0.0f;
	float densityHalfLife = 0.0f;
	float lifetimeSeconds = 0.0f;
	uint32_t styleIndex = 0u;
	uint32_t sourceId = 0u;
	uint32_t epoch = 0u;
	double authoredGameplaySeconds = 0.0;
	float maximumLatencySeconds = 0.0f;
	uint64_t sourceEventSerial = 0u;
	uint32_t batchIndex = 0u;
	uint32_t batchCount = 1u;
};

struct NRISmokeAnalyticCarrierGpu
{
	float position[3] = {};
	float radius = 0.0f;
	float halfAxisU[3] = {};
	uint32_t shape = 0u;
	float halfAxisV[3] = {};
	uint32_t styleIndex = 0u;
	float densityScale = 0.0f;
	uint32_t rangeCount = 0u;
	uint32_t epoch = 0u;
	uint32_t flags = 0u;
	uint32_t lightGroupSlot = UINT32_MAX;
	uint32_t lightGroupGeneration = 0u;
	uint32_t lightAnchorCount = 0u;
	uint32_t lightSampleCountAndFlags = 0u;
};

static_assert(sizeof(NRISmokeAnalyticCarrierGpu) == 80u,
	"analytic carrier GPU records must retain an explicit five-register layout");

struct NRISmokeAnalyticLightPolicy
{
	uint32_t maximumEventBuilds = 0u;
	uint32_t anchorsPerEvent = 0u;
	uint32_t samplesPerAnchor = 0u;
	bool enabled = false;
};

struct NRISmokeAnalyticCarrierHandle
{
	uint32_t slot = UINT32_MAX;
	uint32_t generation = 0u;
	uint32_t epoch = 0u;
};

enum class NRISmokeAnalyticCarrierDropReason : uint32_t
{
	None = 0u,
	NotPrepared,
	Disabled,
	InvalidRequest,
	StaleEpoch,
	ExpiredOnArrival,
	StaleOnArrival,
	Capacity,
	LightingBudget,
};

struct NRISmokeAnalyticCarrierAdmission
{
	NRISmokeAnalyticCarrierHandle handle = {};
	NRISmokeAnalyticCarrierDropReason dropReason =
		NRISmokeAnalyticCarrierDropReason::NotPrepared;

	bool Accepted() const { return dropReason == NRISmokeAnalyticCarrierDropReason::None; }
};

struct NRISmokeAnalyticCarrierSnapshot
{
	uint64_t requested = 0u;
	uint64_t admitted = 0u;
	uint64_t expired = 0u;
	uint64_t droppedNotPrepared = 0u;
	uint64_t droppedDisabled = 0u;
	uint64_t droppedInvalidRequest = 0u;
	uint64_t droppedStaleEpoch = 0u;
	uint64_t droppedExpiredOnArrival = 0u;
	uint64_t droppedStaleOnArrival = 0u;
	uint64_t droppedCapacity = 0u;
	uint64_t droppedLightingBudget = 0u;
	uint32_t epoch = 0u;
	uint32_t maximumActiveQuantity = 0u;
	uint32_t activeQuantity = 0u;
	uint32_t highWaterQuantity = 0u;
	uint32_t oldestActiveAgeMilliseconds = 0u;
	uint32_t lightEventsBuiltThisFrame = 0u;
	uint32_t lightEventsRequestedThisFrame = 0u;
	uint32_t lightEventsAdmittedThisFrame = 0u;
	uint32_t lightEventsRejectedThisFrame = 0u;
	uint32_t lightEventsFirstFrameReady = 0u;
	uint32_t lightRejectedNotPrepared = 0u;
	uint32_t lightRejectedDisabled = 0u;
	uint32_t lightRejectedInvalidRequest = 0u;
	uint32_t lightRejectedStaleEpoch = 0u;
	uint32_t lightRejectedExpiredOnArrival = 0u;
	uint32_t lightRejectedStaleOnArrival = 0u;
	uint32_t lightRejectedCapacity = 0u;
	uint32_t lightRejectedLightingBudget = 0u;
	uint32_t activeLightGroups = 0u;
	uint32_t freeLightGroupSlots = 128u;
	uint32_t lightGroupHighWater = 0u;
	uint32_t sharedCarrierReferences = 0u;
	uint32_t lightAnchorsRequested = 0u;
	uint32_t lightAnchorsReserved = 0u;
	uint32_t lightSamplesRequested = 0u;
	uint32_t lightSamplesReserved = 0u;
};

// Owns immediate analytic smoke admission and gameplay-time lifetime state.
// Rejected requests are never queued or replayed on a later frame.
class NRISmokeAnalyticCarriers
{
public:
	static constexpr uint32_t FixedCarrierCapacity = 128u;

	void BeginFrame(double gameplayTimeSeconds, uint32_t maximumActiveQuantity,
		const NRISmokeAnalyticLightPolicy& lightPolicy = {});
	NRISmokeAnalyticCarrierAdmission Admit(const NRISmokeAnalyticCarrierRequest& request);
	uint32_t AdmitBatch(const NRISmokeAnalyticCarrierRequest* requests, uint32_t count);
	void CommitLightBuilds();
	bool IsLive(const NRISmokeAnalyticCarrierHandle& handle) const;
	void Reset(uint32_t epoch);

	const std::vector<NRISmokeAnalyticCarrierGpu>& GetGpuCarriers() const { return mGpuCarriers; }
	const NRISmokeAnalyticCarrierSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct Slot
	{
		NRISmokeAnalyticCarrierRequest request = {};
		uint32_t generation = 0u;
		uint32_t lightGroupSlot = UINT32_MAX;
		uint32_t lightGroupGeneration = 0u;
		uint32_t lightAnchorCount = 0u;
		uint32_t lightSampleCount = 0u;
		bool lightGroupOwner = false;
		bool lightBuildPending = false;
		bool lightFirstBuild = false;
		bool active = false;
	};

	void Refresh();
	NRISmokeAnalyticCarrierAdmission Drop(NRISmokeAnalyticCarrierDropReason reason);
	NRISmokeAnalyticCarrierAdmission DropEvent(NRISmokeAnalyticCarrierDropReason reason);
	void BeginLightEventRequest();
	void AdmitLightEvent();

	std::array<Slot, FixedCarrierCapacity> mSlots = {};
	std::vector<NRISmokeAnalyticCarrierGpu> mGpuCarriers;
	NRISmokeAnalyticCarrierSnapshot mSnapshot = {};
	double mGameplayTimeSeconds = 0.0;
	bool mPrepared = false;
	NRISmokeAnalyticLightPolicy mLightPolicy = {};
};
