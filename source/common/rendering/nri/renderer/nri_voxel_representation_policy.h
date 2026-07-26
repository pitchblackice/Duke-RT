#pragma once

#include "nri_tlas_masks.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class NRIVoxelRepresentationKind : uint8_t
{
	Exact = 0,
};

enum class NRIVoxelRepresentationReason : uint8_t
{
	ExactOnly = 0,
	EmptyWorkload,
	MissingBounds,
	InvalidTransform,
	ProjectionUnavailable,
	BehindCamera,
};

struct NRIVoxelRepresentationFrameInput
{
	uint64_t mapBuildSerial = 0;
	uint32_t frameIndex = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	std::array<float, 3> cameraPosition = {};
	std::array<float, 3> cameraForward = {};
	std::array<float, 3> cameraRight = {};
	std::array<float, 3> cameraUp = {};
	float tanHalfFovX = 1.0f;
	float tanHalfFovY = 1.0f;
};

struct NRIVoxelRepresentationFacts
{
	uint64_t sourceIdentityKey = 0;
	uint64_t meshResourceKey = 0;
	uint64_t materialKeyHash = 0;
	int32_t actorIndex = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	uint64_t retainedFrameAge = 0;
	uint8_t workloadMask = 0;
	bool capturedThisFrame = false;
	bool routedThroughSharedBlas = false;
	bool boundsValid = false;
	std::array<float, 3> boundsMin = {};
	std::array<float, 3> boundsMax = {};
	std::array<float, 12> transform =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
	};
};

struct NRIVoxelProjectedBounds
{
	bool valid = false;
	bool intersectsViewport = false;
	bool clippedByNearPlane = false;
	float minX = 0.0f;
	float minY = 0.0f;
	float maxX = 0.0f;
	float maxY = 0.0f;
	float widthPixels = 0.0f;
	float heightPixels = 0.0f;
	float areaPixels = 0.0f;
	float maxExtentPixels = 0.0f;
	float nearestDepth = 0.0f;
};

struct NRIVoxelRepresentationDecision
{
	uint64_t decisionIdentity = 0;
	uint64_t sourceIdentityKey = 0;
	uint64_t meshResourceKey = 0;
	uint64_t materialKeyHash = 0;
	int32_t actorIndex = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	uint64_t retainedFrameAge = 0;
	NRIVoxelRepresentationKind representation = NRIVoxelRepresentationKind::Exact;
	NRIVoxelRepresentationReason reason = NRIVoxelRepresentationReason::ExactOnly;
	uint8_t requestedWorkloadMask = 0;
	uint8_t exactWorkloadMask = 0;
	uint8_t proxyWorkloadMask = 0;
	uint8_t primaryWorkloadMask = 0;
	uint8_t shadowWorkloadMask = 0;
	uint8_t reflectionWorkloadMask = 0;
	uint8_t giWorkloadMask = 0;
	uint8_t emissiveWorkloadMask = 0;
	uint8_t debugWorkloadMask = 0;
	bool capturedThisFrame = false;
	bool routedThroughSharedBlas = false;
	bool proxyEligible = false;
	bool proxyReady = false;
	bool hysteresisObservationReady = false;
	bool transitionReady = false;
	uint32_t framesInExactState = 0;
	uint32_t consecutiveProjectedFrames = 0;
	uint32_t transitionCount = 0;
	NRIVoxelProjectedBounds projectedBounds;
};

struct NRIVoxelRepresentationSnapshot
{
	uint64_t mapBuildSerial = 0;
	uint32_t frameIndex = 0;
	uint32_t decisionCount = 0;
	uint32_t exactDecisionCount = 0;
	uint32_t proxyDecisionCount = 0;
	uint32_t projectedDecisionCount = 0;
	uint32_t viewportIntersectionCount = 0;
	uint32_t hysteresisObservationReadyCount = 0;
	uint32_t proxyReadyCount = 0;
	uint32_t primaryOccurrenceCount = 0;
	uint32_t shadowOccurrenceCount = 0;
	uint32_t reflectionOccurrenceCount = 0;
	uint32_t giOccurrenceCount = 0;
	uint32_t emissiveOccurrenceCount = 0;
	uint32_t debugOccurrenceCount = 0;
	uint64_t exactPrimitiveCount = 0;
	std::vector<NRIVoxelRepresentationDecision> decisions;
};

class NRIVoxelRepresentationPolicy
{
public:
	void BeginFrame(const NRIVoxelRepresentationFrameInput& input);
	NRIVoxelRepresentationDecision EvaluateExact(const NRIVoxelRepresentationFacts& facts);
	void Reset();

	const NRIVoxelRepresentationSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct HysteresisState
	{
		uint64_t sourceIdentityKey = 0;
		uint32_t lastFrameIndex = 0;
		uint32_t framesInExactState = 0;
		uint32_t consecutiveProjectedFrames = 0;
		uint32_t transitionCount = 0;
		bool valid = false;
	};

	NRIVoxelRepresentationFrameInput mFrame = {};
	NRIVoxelRepresentationSnapshot mSnapshot = {};
	std::unordered_map<uint64_t, HysteresisState> mHysteresis;
	bool mHasFrame = false;
};

const char* GetNRIVoxelRepresentationKindName(NRIVoxelRepresentationKind kind);
const char* GetNRIVoxelRepresentationReasonName(NRIVoxelRepresentationReason reason);
