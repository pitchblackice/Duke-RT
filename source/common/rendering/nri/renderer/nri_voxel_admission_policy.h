#pragma once

#include <cstdint>

inline bool ShouldHoldNRIVoxelRuntimeTailAdmission(
	bool runtimeRequested,
	bool predictiveGeometry,
	bool runtimeWithheldMesh,
	bool runtimeTailReleased)
{
	return
		!runtimeRequested &&
		!predictiveGeometry &&
		runtimeWithheldMesh &&
		!runtimeTailReleased;
}

inline bool IsNRIVoxelRuntimeAdmissionRequestMoreRecent(
	uint32_t leftLastRequestedFrame,
	uint32_t rightLastRequestedFrame)
{
	if (leftLastRequestedFrame == rightLastRequestedFrame)
	{
		return false;
	}
	if (leftLastRequestedFrame == UINT32_MAX)
	{
		return false;
	}
	if (rightLastRequestedFrame == UINT32_MAX)
	{
		return true;
	}
	return leftLastRequestedFrame > rightLastRequestedFrame;
}

inline bool IsNRIVoxelRuntimeAdmissionRequestStale(
	uint32_t lastRequestedFrame,
	uint32_t currentFrame,
	uint32_t graceFrames)
{
	return
		lastRequestedFrame != UINT32_MAX &&
		currentFrame >= lastRequestedFrame &&
		currentFrame - lastRequestedFrame > graceFrames;
}
