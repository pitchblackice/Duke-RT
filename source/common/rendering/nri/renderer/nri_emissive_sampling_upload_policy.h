#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

enum class NRIEmissiveSamplingUploadAction : uint8_t
{
	Create,
	Grow,
	Replace,
	Reuse,
};

struct NRIEmissiveSamplingUploadResourceInput
{
	bool hasBuffer = false;
	bool hasShaderView = false;
	uint64_t capacityBytes = 0;
	uint32_t currentStride = 0;
	uint64_t payloadBytes = 0;
	uint32_t payloadStride = 0;
};

struct NRIEmissiveSamplingUploadResourceDecision
{
	NRIEmissiveSamplingUploadAction action = NRIEmissiveSamplingUploadAction::Create;
	bool requiresQuiescence = false;
};

struct NRIEmissiveSamplingUploadBatchDecision
{
	uint32_t createCount = 0;
	uint32_t growCount = 0;
	uint32_t replaceCount = 0;
	uint32_t reuseCount = 0;
	bool waitRequired = false;
};

inline NRIEmissiveSamplingUploadResourceDecision NRIPlanEmissiveSamplingUploadResource(
	const NRIEmissiveSamplingUploadResourceInput& input)
{
	const bool hasExistingResource = input.hasBuffer || input.hasShaderView;
	const uint64_t requiredBytes = std::max<uint64_t>(input.payloadBytes, input.payloadStride);
	const bool needsAllocation =
		!input.hasBuffer ||
		!input.hasShaderView ||
		input.currentStride != input.payloadStride ||
		input.capacityBytes < requiredBytes;

	NRIEmissiveSamplingUploadResourceDecision decision = {};
	if (needsAllocation)
	{
		decision.action = hasExistingResource ? NRIEmissiveSamplingUploadAction::Grow : NRIEmissiveSamplingUploadAction::Create;
	}
	else
	{
		decision.action = input.payloadBytes != 0 ? NRIEmissiveSamplingUploadAction::Replace : NRIEmissiveSamplingUploadAction::Reuse;
	}
	decision.requiresQuiescence = hasExistingResource && (needsAllocation || input.payloadBytes != 0);
	return decision;
}

inline NRIEmissiveSamplingUploadBatchDecision NRIPlanEmissiveSamplingUploadBatch(
	const NRIEmissiveSamplingUploadResourceInput* inputs,
	size_t inputCount,
	bool destinationRecycled,
	bool writesAlreadyQuiesced)
{
	NRIEmissiveSamplingUploadBatchDecision batch = {};
	for (size_t i = 0; i < inputCount; i++)
	{
		const NRIEmissiveSamplingUploadResourceDecision decision = NRIPlanEmissiveSamplingUploadResource(inputs[i]);
		switch (decision.action)
		{
		case NRIEmissiveSamplingUploadAction::Create: batch.createCount++; break;
		case NRIEmissiveSamplingUploadAction::Grow: batch.growCount++; break;
		case NRIEmissiveSamplingUploadAction::Replace: batch.replaceCount++; break;
		case NRIEmissiveSamplingUploadAction::Reuse: batch.reuseCount++; break;
		}
		batch.waitRequired = batch.waitRequired || decision.requiresQuiescence;
	}

	batch.waitRequired = batch.waitRequired && !destinationRecycled && !writesAlreadyQuiesced;
	return batch;
}
