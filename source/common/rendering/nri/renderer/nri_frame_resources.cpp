#include "nri_frame_resources.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

namespace
{
	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}
}

bool NRIFrameResources::CreateFrameTexture(NRIRenderer& renderer, uint32_t slot, uint32_t width, uint32_t height, nri::Format format)
{
	if (slot >= (uint32_t)NRIRenderer::FrameTextureSlot::Count)
	{
		return false;
	}

	return renderer.mFrameBuffer->CreateOwnedTexture(
		renderer.GetFrameTexture((NRIRenderer::FrameTextureSlot)slot),
		width,
		height,
		format,
		NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
}

nri::Format NRIFrameResources::ResolveFinalSceneFormat(const NRIRenderer& renderer)
{
	if (renderer.mFrameBuffer == nullptr)
	{
		return nri::Format::BGRA8_UNORM;
	}

	const NRIFrameGenerationPresentContract& presentContract = renderer.mFrameBuffer->mFrameGeneration.GetPresentContract();
	if (presentContract.resolvedTextureFormat != nri::Format::UNKNOWN)
	{
		return presentContract.resolvedTextureFormat;
	}

	if (renderer.mFrameBuffer->mResolvedSwapChainTextureFormat != nri::Format::UNKNOWN)
	{
		return renderer.mFrameBuffer->mResolvedSwapChainTextureFormat;
	}

	return nri::Format::BGRA8_UNORM;
}

void NRIFrameResources::DestroyFrameTextures(NRIRenderer& renderer)
{
	renderer.DestroyAutoExposureResources();
	for (auto& texture : renderer.mFrameTextures)
	{
		renderer.mFrameBuffer->DestroyTextureResource(texture);
	}
	renderer.mRenderWidth = 0;
	renderer.mRenderHeight = 0;
	renderer.mOutputWidth = 0;
	renderer.mOutputHeight = 0;
	renderer.mTargetWidth = 0;
	renderer.mTargetHeight = 0;
	renderer.mSceneLeft = 0;
	renderer.mSceneTop = 0;
	renderer.mFinalSceneFormat = nri::Format::UNKNOWN;
}
