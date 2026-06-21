#pragma once

#include "nri_renderer.h"

class NRIPassDispatchContext;

struct NRIBloomDispatchDesc
{
	enum class Mode
	{
		Copy,
		Filter
	};

	Mode mode = Mode::Copy;
	uint32_t levelIndex = 0;
	uint32_t levelCount = 1;
	NRIRenderer::FrameTextureSlot inputSlot = NRIRenderer::FrameTextureSlot::Count;
	NRIRenderer::FrameTextureSlot secondaryInputSlot = NRIRenderer::FrameTextureSlot::Count;
	NRIRenderer::FrameTextureSlot outputSlot = NRIRenderer::FrameTextureSlot::Count;
};

bool DispatchBloom(NRIPassDispatchContext& context, const NRIBloomDispatchDesc& desc);
