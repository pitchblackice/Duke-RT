#pragma once

#include <cstddef>
#include <cstdint>

enum class NRISmokePass : uint32_t
{
	Clear = 0,
	Simulate,
	Spawn,
	Bin,
	Evaluate,
	Integrate,
	Composite,
};

struct NRISmokeConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t particleCapacity = 0;

	uint32_t commandCount = 0;
	uint32_t styleCount = 0;
	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;

	uint32_t froxelDepth = 0;
	uint32_t columnCapacity = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;

	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t debugMode = 0;
	uint32_t flags = 0;

	float deltaTime = 0.0f;
	float simulationTime = 0.0f;
	float froxelMaxDistance = 0.0f;
	float depthExponent = 1.0f;

	float densityScale = 1.0f;
	float radianceScale = 1.0f;
	float tanHalfFovX = 1.0f;
	float tanHalfFovY = 1.0f;

	float cameraPosition[3] = {};
	float timeScale = 1.0f;

	float cameraForward[3] = {};
	float cameraForwardPad = 0.0f;

	float cameraRight[3] = {};
	float cameraRightPad = 0.0f;

	float cameraUp[3] = {};
	float cameraUpPad = 0.0f;

	float wind[3] = {};
	float windPad = 0.0f;
};

static_assert(sizeof(NRISmokeConstants) == 176, "NRISmokeConstants must match SmokeConstants.hlsli");
static_assert(offsetof(NRISmokeConstants, cameraPosition) == 96, "NRISmokeConstants camera offset must match HLSL");
