#pragma once

#include "nri_renderer.h"

#include <vector>

struct HWDrawInfo;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

class NRIPassDispatcher
{
public:
	static bool DispatchBootstrapView(NRIRenderer& renderer);
	static bool DispatchFrameGraph(
		NRIRenderer& renderer,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		int drawmode);
	static bool DispatchTraceOpaque(
		NRIRenderer& renderer,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials);
	static bool DispatchDenoiser(NRIRenderer& renderer);
	static bool DispatchComposition(NRIRenderer& renderer, NRIRenderer::FrameTextureSlot outputSlot = NRIRenderer::FrameTextureSlot::Composed);
	static bool DispatchTraceTransparent(NRIRenderer& renderer);
	static bool DispatchUpscalerPrepass(NRIRenderer& renderer, NRIMainUpscalerKind mainKind);
	static bool DispatchRawPresent(
		NRIRenderer& renderer,
		NRIRenderer::FrameTextureSlot inputSlot,
		NRIRenderer::FrameTextureSlot secondarySlot = NRIRenderer::FrameTextureSlot::Count,
		NRIRenderer::FrameTextureSlot tertiarySlot = NRIRenderer::FrameTextureSlot::Count);
	static bool DispatchFinalPresent(NRIRenderer& renderer, NRIRenderer::FrameTextureSlot inputSlot);
	static bool DispatchUpscaleChain(NRIRenderer& renderer);
	static bool DispatchFinal(NRIRenderer& renderer);
};
