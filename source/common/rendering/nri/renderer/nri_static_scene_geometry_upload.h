#pragma once

#include "nri_frame_resources.h"

#include <cstdint>

struct NRIStaticSceneGeometryUploadServices
{
	using EnsureResidentStructuredBufferFn = bool (*)(
		void* user,
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		const char* waitReason,
		int uploadKind);
	using RefreshResidentStaticSceneDataSetFn = bool (*)(void* user);
	using NoteResidentStaticAtlasGrowFn = void (*)(void* user);

	void* user = nullptr;
	EnsureResidentStructuredBufferFn ensureResidentStructuredBuffer = nullptr;
	RefreshResidentStaticSceneDataSetFn refreshResidentStaticSceneDataSet = nullptr;
	NoteResidentStaticAtlasGrowFn noteResidentStaticAtlasGrow = nullptr;

	bool EnsureResidentStructuredBuffer(
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		const char* waitReason,
		int uploadKind) const
	{
		return ensureResidentStructuredBuffer != nullptr &&
			ensureResidentStructuredBuffer(user, resource, stats, data, size, stride, usage, after, waitReason, uploadKind);
	}

	bool RefreshResidentStaticSceneDataSet() const
	{
		return refreshResidentStaticSceneDataSet != nullptr &&
			refreshResidentStaticSceneDataSet(user);
	}

	void NoteResidentStaticAtlasGrow() const
	{
		if (noteResidentStaticAtlasGrow != nullptr)
		{
			noteResidentStaticAtlasGrow(user);
		}
	}
};
