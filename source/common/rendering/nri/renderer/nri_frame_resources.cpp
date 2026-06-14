#include "nri_frame_resources.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include "nri_upscaler.h"
#include "../framegen/nri_framegen.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

EXTERN_CVAR(Bool, nri_ptscenestats)
EXTERN_CVAR(Float, nri_renderscale)

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
		NRIResourceFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
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

bool NRIRenderer::EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	Clocker clock(NriPTFrameResources);

	if (outputWidth == 0 || outputHeight == 0 || targetWidth == 0 || targetHeight == 0)
	{
		return false;
	}

	const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
	// Preserve the oversized hardware viewport and crop it during present instead of shrinking it to the visible target.
	const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
	const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - (int32_t)outputHeight;

	const NRIMainUpscalerKind mainUpscalerKind = ResolveMainUpscalerKind(false);
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainUpscalerKind, requestedUpscalerMode);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float renderScale = NRIResolveRenderScaleForMain(mainUpscalerKind, requestedUpscalerMode, requestedRenderScale);
	const NRIFrameGenerationPresentContract& presentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();

	const uint32_t renderWidth = std::max(1u, (uint32_t)std::lround((double)outputWidth * renderScale));
	const uint32_t renderHeight = std::max(1u, (uint32_t)std::lround((double)outputHeight * renderScale));
	const nri::Format finalFormat = ResolveFinalSceneFormat();
	const nri::Format activeTargetFormat =
		(mFrameBuffer->mActiveTarget != nullptr && mFrameBuffer->mActiveTarget->format != nri::Format::UNKNOWN)
		? mFrameBuffer->mActiveTarget->format
		: nri::Format::UNKNOWN;

	const bool upToDate =
		mRenderWidth == renderWidth &&
		mRenderHeight == renderHeight &&
		mOutputWidth == outputWidth &&
		mOutputHeight == outputHeight &&
		mTargetWidth == targetWidth &&
		mTargetHeight == targetHeight &&
		mSceneLeft == sceneLeft &&
		mSceneTop == sceneTop &&
		mFinalSceneFormat == finalFormat &&
		GetFrameTexture(FrameTextureSlot::Final).texture != nullptr;

	if (upToDate)
	{
		return true;
	}

	// Frame-resource rebuilds on resize/upscaler mode changes can retire textures that the current
	// command allocator still references. Drain GPU work before destroying frame-sized resources.
	const bool dimensionsChanged =
		mRenderWidth != renderWidth ||
		mRenderHeight != renderHeight ||
		mOutputWidth != outputWidth ||
		mOutputHeight != outputHeight ||
		mTargetWidth != targetWidth ||
		mTargetHeight != targetHeight;
	WaitForCommandsTracked();
	mNrd.Shutdown();
	DestroyFrameTextures();
	mRenderWidth = renderWidth;
	mRenderHeight = renderHeight;
	mOutputWidth = outputWidth;
	mOutputHeight = outputHeight;
	mTargetWidth = targetWidth;
	mTargetHeight = targetHeight;
	mSceneLeft = sceneLeft;
	mSceneTop = sceneTop;
	mFinalSceneFormat = finalFormat;
	RequestHistoryReset(dimensionsChanged ? "resize" : "frame-resources");
	if (nri_ptscenestats)
	{
		Printf("NRI PT frame resources: main=%s policy=%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f render=%ux%u output=%ux%u final=%s contract=%s active=%s jitter=%s phases=%u\n",
			NRIGetMainUpscalerName(mainUpscalerKind),
			NRIGetRenderResolutionPolicyName(mainUpscalerKind),
			NRIGetUpscalerModeName(requestedUpscalerMode),
			NRIGetUpscalerModeName(resolvedUpscalerMode),
			requestedRenderScale,
			renderScale,
			renderWidth,
			renderHeight,
			outputWidth,
			outputHeight,
			NRIFrameGenerationContext::GetNriFormatName(finalFormat),
			NRIFrameGenerationContext::GetNriFormatName(presentContract.resolvedTextureFormat),
			NRIFrameGenerationContext::GetNriFormatName(activeTargetFormat),
			NRIGetTemporalJitterModeName(mainUpscalerKind, mGuiCaptureActive),
			NRIGetTemporalJitterPhaseCount(mainUpscalerKind, resolvedUpscalerMode, mGuiCaptureActive));
	}

	const nri::Format colorFormat = nri::Format::RGBA16_SFLOAT;
	const nri::Format normalRoughnessFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format upscalerDepthFormat = nri::Format::R32_SFLOAT;
	const nri::Format rrGuideAlbedoFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format rrGuideSpecHitDistanceFormat = nri::Format::R16_SFLOAT;
	const nri::Format rrGuideNormalRoughnessFormat = nri::Format::RGBA16_SFLOAT;

	return
		CreateFrameTexture(FrameTextureSlot::ViewZ, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Motion, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::NormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::BaseColorMetalness, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredPenumbra, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedShadow, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Composed, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TraceTransparentOutput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectLighting, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectEmission, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::SrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UpscalerDepth, renderWidth, renderHeight, upscalerDepthFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance, renderWidth, renderHeight, rrGuideSpecHitDistanceFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideNormalRoughness, renderWidth, renderHeight, rrGuideNormalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::VendorOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::PostSharpenOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Final, targetWidth, targetHeight, finalFormat);
}

bool NRIRenderer::CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format)
{
	return NRIFrameResources::CreateFrameTexture(*this, (uint32_t)slot, width, height, format);
}





nri::Format NRIRenderer::ResolveFinalSceneFormat() const
{
	return NRIFrameResources::ResolveFinalSceneFormat(*this);
}




void NRIRenderer::DestroyFrameTextures()
{
	NRIFrameResources::DestroyFrameTextures(*this);
}
