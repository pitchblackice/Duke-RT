#pragma once

#include "nri_frame_resources.h"

class NRIRenderer;

class NRISceneUploadManager
{
public:
	static bool CreateStructuredBuffer(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after);

	static bool EnsureStructuredBuffer(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		bool writesQuiesced,
		const char* waitReason);

	static bool UpdateStructuredBufferRange(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		uint64_t byteOffset,
		const void* data,
		uint64_t size,
		nri::AccessStage after);
};
