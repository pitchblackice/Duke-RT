#pragma once

#include <algorithm>
#include <cstdint>

struct NRISceneDataLightBufferReuseView
{
	bool resourceReady = false;
	bool descriptorReady = false;
	uint64_t usedSize = 0;
	uint64_t capacity = 0;
	uint32_t stride = 0;

	bool CanReuse(uint64_t requiredBytes, uint32_t requiredStride) const
	{
		return
			resourceReady &&
			descriptorReady &&
			stride == requiredStride &&
			usedSize >= requiredBytes &&
			capacity >= std::max<uint64_t>(requiredBytes, requiredStride);
	}
};

struct NRISceneDataRuntimeLightSlotIdentity
{
	uint64_t payloadHash = 0;
	uint32_t lightCount = 0;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		uint32_t candidateLightCount,
		const NRISceneDataLightBufferReuseView& buffer,
		uint32_t elementStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			lightCount == candidateLightCount &&
			buffer.CanReuse((uint64_t)lightCount * elementStride, elementStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(uint64_t newPayloadHash, uint32_t newLightCount)
	{
		payloadHash = newPayloadHash;
		lightCount = newLightCount;
		valid = true;
	}
};

struct NRISceneDataRuntimeLightClusterSlotIdentity
{
	uint64_t payloadHash = 0;
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t tileIndexCount = 0;
	uint32_t maxTileOccupancy = 0;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		uint32_t headerStride,
		const NRISceneDataLightBufferReuseView& indexBuffer,
		uint32_t indexStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			headerBuffer.CanReuse((uint64_t)tileCountX * tileCountY * headerStride, headerStride) &&
			indexBuffer.CanReuse((uint64_t)tileIndexCount * indexStride, indexStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(
		uint64_t newPayloadHash,
		uint32_t newTileCountX,
		uint32_t newTileCountY,
		uint32_t newTileIndexCount,
		uint32_t newMaxTileOccupancy)
	{
		payloadHash = newPayloadHash;
		tileCountX = newTileCountX;
		tileCountY = newTileCountY;
		tileIndexCount = newTileIndexCount;
		maxTileOccupancy = newMaxTileOccupancy;
		valid = true;
	}
};

struct NRISceneDataSectorLightSlotIdentity
{
	uint64_t payloadHash = 0;
	uint32_t sectorCount = 0;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		uint32_t candidateSectorCount,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		uint32_t headerStride,
		const NRISceneDataLightBufferReuseView& dataBuffer,
		uint32_t dataStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			sectorCount == candidateSectorCount &&
			headerBuffer.CanReuse(headerStride, headerStride) &&
			dataBuffer.CanReuse((uint64_t)sectorCount * dataStride, dataStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(uint64_t newPayloadHash, uint32_t newSectorCount)
	{
		payloadHash = newPayloadHash;
		sectorCount = newSectorCount;
		valid = true;
	}
};

struct NRISceneDataLightSlotReuseState
{
	NRISceneDataRuntimeLightSlotIdentity runtimeLight;
	NRISceneDataRuntimeLightClusterSlotIdentity runtimeLightCluster;
	NRISceneDataSectorLightSlotIdentity sectorLight;

	void Reset()
	{
		*this = {};
	}
};
