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
};

static_assert(sizeof(NRISmokeAnalyticCarrierGpu) == 64u,
	"analytic carrier GPU records must retain a four-register layout");

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
	uint32_t epoch = 0u;
	uint32_t maximumActiveQuantity = 0u;
	uint32_t activeQuantity = 0u;
	uint32_t highWaterQuantity = 0u;
	uint32_t oldestActiveAgeMilliseconds = 0u;
};

// Owns immediate analytic smoke admission and gameplay-time lifetime state.
// Rejected requests are never queued or replayed on a later frame.
class NRISmokeAnalyticCarriers
{
public:
	static constexpr uint32_t FixedCarrierCapacity = 128u;

	void BeginFrame(double gameplayTimeSeconds, uint32_t maximumActiveQuantity);
	NRISmokeAnalyticCarrierAdmission Admit(const NRISmokeAnalyticCarrierRequest& request);
	uint32_t AdmitBatch(const NRISmokeAnalyticCarrierRequest* requests, uint32_t count);
	bool IsLive(const NRISmokeAnalyticCarrierHandle& handle) const;
	void Reset(uint32_t epoch);

	const std::vector<NRISmokeAnalyticCarrierGpu>& GetGpuCarriers() const { return mGpuCarriers; }
	const NRISmokeAnalyticCarrierSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct Slot
	{
		NRISmokeAnalyticCarrierRequest request = {};
		uint32_t generation = 0u;
		bool active = false;
	};

	void Refresh();
	NRISmokeAnalyticCarrierAdmission Drop(NRISmokeAnalyticCarrierDropReason reason);

	std::array<Slot, FixedCarrierCapacity> mSlots = {};
	std::vector<NRISmokeAnalyticCarrierGpu> mGpuCarriers;
	NRISmokeAnalyticCarrierSnapshot mSnapshot = {};
	double mGameplayTimeSeconds = 0.0;
	bool mPrepared = false;
};
