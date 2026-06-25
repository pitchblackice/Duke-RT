#pragma once

#include "nri_resources.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class NRIRenderer;

class NRIFrameResources
{
public:
	static bool EnsureFrameResources(NRIRenderer& renderer, uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	static bool CreateFrameTexture(NRIRenderer& renderer, uint32_t slot, uint32_t width, uint32_t height, nri::Format format);
	static nri::Format ResolveFinalSceneFormat(const NRIRenderer& renderer);
	static void DestroyFrameTextures(NRIRenderer& renderer);
};

struct SceneBufferDebugStats
{
	const char* label = "";
	uint32_t growthCount = 0;
	uint32_t overwriteCount = 0;
	uint32_t uploadCount = 0;
	uint32_t growEventsLastFrame = 0;
	uint32_t overwriteEventsLastFrame = 0;
	uint64_t bytesUploadedLastFrame = 0;
	uint64_t growthOldBytesLastFrame = 0;
	uint64_t growthRequestedBytesLastFrame = 0;
	uint64_t growthAllocatedBytesLastFrame = 0;
	uint64_t peakUsedBytes = 0;
};

struct SceneUploadDirtyRange
{
	uint64_t byteOffset = 0;
	uint64_t size = 0;
};

enum ResidentUploadKind
{
	ResidentUploadKind_Vertex = 0,
	ResidentUploadKind_Index = 1,
	ResidentUploadKind_Primitive = 2,
	ResidentUploadKind_Material = 3,
};

struct SceneInstanceData
{
	uint32_t primitiveBase = 0;
	uint32_t dataSource = 0;
	uint32_t materialBase = 0;
	uint32_t materialCount = UINT32_MAX;
	uint32_t visibilityChunk = UINT32_MAX;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	uint32_t metadata2 = 0;
	float currentTransform[12] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	float previousTransform[12] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
};

static_assert(sizeof(SceneInstanceData) == 128, "SceneInstanceData must match the HLSL scene instance layout");
static_assert(offsetof(SceneInstanceData, currentTransform) == 32, "SceneInstanceData currentTransform offset must match HLSL");
static_assert(offsetof(SceneInstanceData, previousTransform) == 80, "SceneInstanceData previousTransform offset must match HLSL");

struct SceneUploadBufferRingSlot
{
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	NRIAccelerationStructureResource dynamicBottomLevelAS;
	std::vector<uint8_t> vertexMirror;
	std::vector<uint8_t> indexMirror;
	std::vector<uint8_t> primitiveMirror;
	std::vector<uint8_t> materialMirror;
};
