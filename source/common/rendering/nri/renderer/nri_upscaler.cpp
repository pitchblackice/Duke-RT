#include "nri_upscaler.h"
#include "nri_cvars.h"

#include "../system/nri_renderdevice.h"
#include "nri_renderer.h"
#include "c_cvars.h"
#include "printf.h"

#include <cstring>
#include <limits>


namespace
{
	nri::Upscaler* SelectMainUpscaler(NRIMainUpscalerKind kind, nri::Upscaler* dlsr, nri::Upscaler* dlrr)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return dlsr;
		case NRIMainUpscalerKind::DLRR: return dlrr;
		default: return nullptr;
		}
	}

	nri::Upscaler* SelectPostSharpen(NRIPostSharpenKind kind, nri::Upscaler* nis)
	{
		switch (kind)
		{
		case NRIPostSharpenKind::NIS: return nis;
		default: return nullptr;
		}
	}

	const char* GetUpscalerTypeName(nri::UpscalerType type)
	{
		switch (type)
		{
		case nri::UpscalerType::NIS: return "NIS";
		case nri::UpscalerType::DLSR: return "DLSS-SR";
		case nri::UpscalerType::DLRR: return "DLRR";
		default: return "unknown";
		}
	}
}

void NRISyncLegacyUpscalerConfig(bool logMigration)
{
	if ((int)nri_upscaler == 1)
	{
		nri_upscaler = 0;
		if ((int)nri_postsharpen == 0)
		{
			nri_postsharpen = 1;
		}

		static bool loggedLegacyNisMigration = false;
		if (logMigration && !loggedLegacyNisMigration)
		{
			Printf("NRI upscaler config: migrated legacy nri_upscaler=1 (NIS) to nri_upscaler=0 + nri_postsharpen=1\n");
			loggedLegacyNisMigration = true;
		}
	}

	const int clampedMainUpscaler =
		(int)nri_upscaler == 0 || (int)nri_upscaler == 2 || (int)nri_upscaler == 3
		? (int)nri_upscaler
		: 0;
	if ((int)nri_upscaler != clampedMainUpscaler)
	{
		const int invalidValue = (int)nri_upscaler;
		nri_upscaler = clampedMainUpscaler;

		static int lastLoggedInvalidMainUpscaler = std::numeric_limits<int>::min();
		if (logMigration && lastLoggedInvalidMainUpscaler != invalidValue)
		{
			Printf("NRI upscaler config: invalid main upscaler value %d, forcing off\n", invalidValue);
			lastLoggedInvalidMainUpscaler = invalidValue;
		}
	}

	const int clampedPostSharpen = (int)nri_postsharpen == 1 ? 1 : 0;
	if ((int)nri_postsharpen != clampedPostSharpen)
	{
		const int invalidValue = (int)nri_postsharpen;
		nri_postsharpen = clampedPostSharpen;

		static int lastLoggedInvalidPostSharpen = std::numeric_limits<int>::min();
		if (logMigration && lastLoggedInvalidPostSharpen != invalidValue)
		{
			Printf("NRI upscaler config: invalid post sharpen value %d, forcing off\n", invalidValue);
			lastLoggedInvalidPostSharpen = invalidValue;
		}
	}
}

const char* NRIGetMainUpscalerName(NRIMainUpscalerKind kind)
{
	switch (kind)
	{
	case NRIMainUpscalerKind::DLSR: return "DLSS-SR";
	case NRIMainUpscalerKind::DLRR: return "DLRR";
	default: return "off";
	}
}

const char* NRIGetPostSharpenName(NRIPostSharpenKind kind)
{
	switch (kind)
	{
	case NRIPostSharpenKind::NIS: return "NIS";
	default: return "off";
	}
}

const char* NRIGetRenderResolutionPolicyName(NRIMainUpscalerKind kind)
{
	switch (kind)
	{
	case NRIMainUpscalerKind::DLSR: return "sr-mode-scale";
	case NRIMainUpscalerKind::DLRR: return "rr-mode-scale";
	default: return "manual-scale";
	}
}

const char* NRIGetUpscalerModeName(nri::UpscalerMode mode)
{
	switch (mode)
	{
	case nri::UpscalerMode::ULTRA_QUALITY: return "ultra_quality";
	case nri::UpscalerMode::QUALITY: return "quality";
	case nri::UpscalerMode::BALANCED: return "balanced";
	case nri::UpscalerMode::PERFORMANCE: return "performance";
	case nri::UpscalerMode::ULTRA_PERFORMANCE: return "ultra_performance";
	default: return "native";
	}
}

nri::UpscalerType NRIToMainUpscalerType(NRIMainUpscalerKind kind)
{
	switch (kind)
	{
	case NRIMainUpscalerKind::DLSR: return nri::UpscalerType::DLSR;
	case NRIMainUpscalerKind::DLRR: return nri::UpscalerType::DLRR;
	default: return nri::UpscalerType::NIS;
	}
}

nri::UpscalerType NRIToPostSharpenType(NRIPostSharpenKind kind)
{
	switch (kind)
	{
	case NRIPostSharpenKind::NIS: return nri::UpscalerType::NIS;
	default: return nri::UpscalerType::NIS;
	}
}

float NRIGetUpscalerRenderScale(nri::UpscalerMode mode)
{
	switch (mode)
	{
	default:
	case nri::UpscalerMode::NATIVE: return 1.0f;
	case nri::UpscalerMode::ULTRA_QUALITY: return 1.0f / 1.3f;
	case nri::UpscalerMode::QUALITY: return 1.0f / 1.5f;
	case nri::UpscalerMode::BALANCED: return 1.0f / 1.7f;
	case nri::UpscalerMode::PERFORMANCE: return 0.5f;
	case nri::UpscalerMode::ULTRA_PERFORMANCE: return 1.0f / 3.0f;
	}
}

uint32_t NRIGetUpscalerJitterPhaseCount(nri::UpscalerMode mode)
{
	switch (mode)
	{
	case nri::UpscalerMode::NATIVE: return 8u;
	case nri::UpscalerMode::ULTRA_QUALITY: return 14u;
	case nri::UpscalerMode::QUALITY: return 18u;
	case nri::UpscalerMode::BALANCED: return 23u;
	case nri::UpscalerMode::PERFORMANCE: return 32u;
	case nri::UpscalerMode::ULTRA_PERFORMANCE: return 72u;
	default: return 8u;
	}
}

nri::UpscalerMode NRIResolveUpscalerModeForMain(NRIMainUpscalerKind, nri::UpscalerMode requestedMode)
{
	return requestedMode;
}

float NRIResolveRenderScaleForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode, float manualRenderScale)
{
	switch (kind)
	{
	case NRIMainUpscalerKind::DLSR:
	case NRIMainUpscalerKind::DLRR:
		return NRIGetUpscalerRenderScale(requestedMode);
	default:
		return manualRenderScale;
	}
}

bool NRIIsAppTaaEligibleUpscaler(NRIMainUpscalerKind kind)
{
	return kind == NRIMainUpscalerKind::Off;
}

bool NRIShouldRunAppTaa(NRIMainUpscalerKind kind)
{
	return NRIIsAppTaaEligibleUpscaler(kind) && !!nri_pttaa;
}

bool NRIShouldUseTemporalJitter(NRIMainUpscalerKind kind)
{
	return NRIShouldRunAppTaa(kind) || kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR;
}

const char* NRIGetTemporalJitterModeName(NRIMainUpscalerKind kind, bool guiCaptureActive)
{
	if (guiCaptureActive)
	{
		return "off-gui-capture";
	}

	if (kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR)
	{
		return "upscaler";
	}

	return NRIShouldRunAppTaa(kind) ? "taa" : "off";
}

uint32_t NRIGetTemporalJitterPhaseCount(NRIMainUpscalerKind kind, nri::UpscalerMode mode, bool guiCaptureActive)
{
	if (guiCaptureActive)
	{
		return 0u;
	}

	if (kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR)
	{
		return NRIGetUpscalerJitterPhaseCount(mode);
	}

	return 8u;
}

bool NRIUpscalerContext::EnsureUpscaler(
	NRIRenderDevice& frameBuffer,
	UpscalerSlotState& slot,
	nri::UpscalerType type,
	nri::UpscalerMode mode,
	uint32_t upscaleWidth,
	uint32_t upscaleHeight,
	nri::UpscalerBits flags)
{
	if (slot.instance != nullptr &&
		slot.mode == mode &&
		slot.upscaleWidth == upscaleWidth &&
		slot.upscaleHeight == upscaleHeight &&
		slot.flags == flags)
	{
		return true;
	}

	if (slot.instance != nullptr && slot.flags != flags)
	{
		Printf("NRI PT upscaler recreate: type=%s reason=flags-change old_flags=0x%x new_flags=0x%x\n",
			GetUpscalerTypeName(type),
			(uint32_t)slot.flags,
			(uint32_t)flags);
	}

	DestroyUpscaler(frameBuffer, slot.instance);
	if (!frameBuffer.mUpscaler.IsUpscalerSupported(*frameBuffer.mDevice, type))
	{
		return false;
	}

	nri::UpscalerDesc upscalerDesc = {};
	upscalerDesc.upscaleResolution = { (nri::Dim_t)upscaleWidth, (nri::Dim_t)upscaleHeight };
	upscalerDesc.type = type;
	upscalerDesc.mode = mode;
	upscalerDesc.commandBuffer = frameBuffer.mCommandBuffer;
	upscalerDesc.flags = flags;

	const nri::Result upsResult = frameBuffer.mUpscaler.CreateUpscaler(*frameBuffer.mDevice, upscalerDesc, slot.instance);
	if (upsResult != nri::Result::SUCCESS)
	{
		// Worth reporting: a failure here makes the whole upscale pass bail out,
		// which presents as a black frame with no other diagnostic.
		Printf(TEXTCOLOR_YELLOW "NRI upscaler creation failed: type=%s result=%u size=%ux%u\n",
			GetUpscalerTypeName(type), (unsigned)upsResult, upscaleWidth, upscaleHeight);
		slot.instance = nullptr;
		return false;
	}

	slot.mode = mode;
	slot.upscaleWidth = upscaleWidth;
	slot.upscaleHeight = upscaleHeight;
	slot.flags = flags;
	return true;
}

bool NRIUpscalerContext::EnsureMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight, bool useExposure, bool useReactive)
{
	if (kind == NRIMainUpscalerKind::Off)
	{
		return true;
	}

	const nri::UpscalerType type =
		kind == NRIMainUpscalerKind::DLSR ? nri::UpscalerType::DLSR :
		nri::UpscalerType::DLRR;
	UpscalerSlotState& slot =
		kind == NRIMainUpscalerKind::DLSR ? mDlsr :
		mDlrr;
	nri::UpscalerBits flags =
		kind == NRIMainUpscalerKind::DLSR
		? nri::UpscalerBits::HDR
		: (nri::UpscalerBits)((uint32_t)nri::UpscalerBits::HDR | (uint32_t)nri::UpscalerBits::DEPTH_LINEAR);
	if (useExposure)
	{
		flags = (nri::UpscalerBits)((uint32_t)flags | (uint32_t)nri::UpscalerBits::USE_EXPOSURE);
	}
	if (useReactive)
		flags = (nri::UpscalerBits)((uint32_t)flags | (uint32_t)nri::UpscalerBits::USE_REACTIVE);
	return EnsureUpscaler(frameBuffer, slot, type, mode, upscaleWidth, upscaleHeight, flags);
}

bool NRIUpscalerContext::EnsurePostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, uint32_t upscaleWidth, uint32_t upscaleHeight)
{
	if (kind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	return EnsureUpscaler(
		frameBuffer,
		mNis,
		nri::UpscalerType::NIS,
		nri::UpscalerMode::QUALITY,
		upscaleWidth,
		upscaleHeight,
		nri::UpscalerBits::HDR);
}

bool NRIUpscalerContext::DispatchMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc)
{
	nri::Upscaler* upscaler = SelectMainUpscaler(kind, mDlsr.instance, mDlrr.instance);
	if (upscaler == nullptr || desc.commandBuffer == nullptr || desc.input == nullptr || desc.output == nullptr)
	{
		return false;
	}

	nri::DispatchUpscaleDesc dispatchDesc = {};
	dispatchDesc.output = { desc.output->texture, desc.output->storageView };
	dispatchDesc.input = { desc.input->texture, desc.input->shaderView };
	dispatchDesc.currentResolution = { (nri::Dim_t)desc.currentWidth, (nri::Dim_t)desc.currentHeight };
	// The NRI DLSS paths expect jitter pointing from the pixel center back toward the unjittered sample.
	dispatchDesc.cameraJitter = { -desc.cameraJitter[0], -desc.cameraJitter[1] };
	// The shared PT motion buffer is already written in pixel units, so the upscaler path keeps mvScale at identity.
	dispatchDesc.mvScale = { 1.0f, 1.0f };
	dispatchDesc.flags = desc.resetHistory ? nri::DispatchUpscaleBits::RESET_HISTORY : nri::DispatchUpscaleBits::NONE;

	if (kind == NRIMainUpscalerKind::DLSR)
	{
		if (desc.motion == nullptr || desc.depth == nullptr)
		{
			return false;
		}

		dispatchDesc.guides.upscaler.mv = { desc.motion->texture, desc.motion->shaderView };
		dispatchDesc.guides.upscaler.depth = { desc.depth->texture, desc.depth->shaderView };
		if (desc.exposure != nullptr)
		{
			dispatchDesc.guides.upscaler.exposure = { desc.exposure->texture, desc.exposure->shaderView };
		}
		if (desc.reactive != nullptr)
			dispatchDesc.guides.upscaler.reactive = { desc.reactive->texture, desc.reactive->shaderView };
	}
	else if (kind == NRIMainUpscalerKind::DLRR)
	{
		if (desc.motion == nullptr || desc.depth == nullptr || desc.normalRoughness == nullptr ||
			desc.diffuseAlbedo == nullptr || desc.specularAlbedo == nullptr || desc.specularHitDistance == nullptr)
		{
			return false;
		}

		dispatchDesc.guides.denoiser.mv = { desc.motion->texture, desc.motion->shaderView };
		dispatchDesc.guides.denoiser.depth = { desc.depth->texture, desc.depth->shaderView };
		dispatchDesc.guides.denoiser.normalRoughness = { desc.normalRoughness->texture, desc.normalRoughness->shaderView };
		dispatchDesc.guides.denoiser.diffuseAlbedo = { desc.diffuseAlbedo->texture, desc.diffuseAlbedo->shaderView };
		dispatchDesc.guides.denoiser.specularAlbedo = { desc.specularAlbedo->texture, desc.specularAlbedo->shaderView };
		dispatchDesc.guides.denoiser.specularMvOrHitT = { desc.specularHitDistance->texture, desc.specularHitDistance->shaderView };
		if (desc.exposure != nullptr)
		{
			dispatchDesc.guides.denoiser.exposure = { desc.exposure->texture, desc.exposure->shaderView };
		}
		if (desc.reactive != nullptr)
			dispatchDesc.guides.denoiser.reactive = { desc.reactive->texture, desc.reactive->shaderView };
		std::memcpy(dispatchDesc.settings.dlrr.viewToClipMatrix, desc.viewToClipMatrix, sizeof(dispatchDesc.settings.dlrr.viewToClipMatrix));
		std::memcpy(dispatchDesc.settings.dlrr.worldToViewMatrix, desc.worldToViewMatrix, sizeof(dispatchDesc.settings.dlrr.worldToViewMatrix));
	}

	frameBuffer.mUpscaler.CmdDispatchUpscale(*desc.commandBuffer, *upscaler, dispatchDesc);
	return true;
}

bool NRIUpscalerContext::DispatchPostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc)
{
	nri::Upscaler* upscaler = SelectPostSharpen(kind, mNis.instance);
	if (upscaler == nullptr || desc.commandBuffer == nullptr || desc.input == nullptr || desc.output == nullptr)
	{
		return false;
	}

	nri::DispatchUpscaleDesc dispatchDesc = {};
	dispatchDesc.output = { desc.output->texture, desc.output->storageView };
	dispatchDesc.input = { desc.input->texture, desc.input->shaderView };
	dispatchDesc.currentResolution = { (nri::Dim_t)desc.currentWidth, (nri::Dim_t)desc.currentHeight };
	dispatchDesc.cameraJitter = { desc.cameraJitter[0], desc.cameraJitter[1] };
	// The shared PT motion buffer is already written in pixel units, so the upscaler path keeps mvScale at identity.
	dispatchDesc.mvScale = { 1.0f, 1.0f };
	dispatchDesc.flags = desc.resetHistory ? nri::DispatchUpscaleBits::RESET_HISTORY : nri::DispatchUpscaleBits::NONE;
	dispatchDesc.settings.nis.sharpness = desc.sharpness;

	frameBuffer.mUpscaler.CmdDispatchUpscale(*desc.commandBuffer, *upscaler, dispatchDesc);
	return true;
}

void NRIUpscalerContext::DestroyUpscaler(NRIRenderDevice& frameBuffer, nri::Upscaler*& upscaler)
{
	if (upscaler != nullptr)
	{
		frameBuffer.mUpscaler.DestroyUpscaler(upscaler);
		upscaler = nullptr;
	}
}

void NRIUpscalerContext::Shutdown(NRIRenderDevice& frameBuffer)
{
	DestroyUpscaler(frameBuffer, mNis.instance);
	DestroyUpscaler(frameBuffer, mDlsr.instance);
	DestroyUpscaler(frameBuffer, mDlrr.instance);
	mNis = {};
	mDlsr = {};
	mDlrr = {};
}

bool NRIRenderer::IsMainUpscalerSupported(NRIMainUpscalerKind kind) const
{
	if (kind == NRIMainUpscalerKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIMainUpscalerKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, NRIToMainUpscalerType(kind));
}

bool NRIRenderer::IsPostSharpenSupported(NRIPostSharpenKind kind) const
{
	if (kind == NRIPostSharpenKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIPostSharpenKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, NRIToPostSharpenType(kind));
}

NRIMainUpscalerKind NRIRenderer::ResolveMainUpscalerKind(bool logFallback)
{
	NRISyncLegacyUpscalerConfig(logFallback);
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();
	NRIMainUpscalerKind resolved = requested;

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			resolved =
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			resolved = NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastMainUpscalerRequest != (int)nri_upscaler || mLastMainUpscalerResolved != resolved))
	{
		Printf("NRI main upscaler fallback: requested %s is unavailable on %s, using %s\n",
			NRIGetMainUpscalerName(requested),
			(const char*)nri_api,
			NRIGetMainUpscalerName(resolved));
		mLastMainUpscalerRequest = (int)nri_upscaler;
		mLastMainUpscalerResolved = resolved;
	}

	return resolved;
}

NRIPostSharpenKind NRIRenderer::ResolvePostSharpenKind(bool logFallback)
{
	NRISyncLegacyUpscalerConfig(logFallback);
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	NRIPostSharpenKind resolved = requested;

	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		resolved = NRIPostSharpenKind::Off;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastPostSharpenRequest != (int)nri_postsharpen || mLastPostSharpenResolved != resolved))
	{
		Printf("NRI post sharpen fallback: requested %s is unavailable on %s, using %s\n",
			NRIGetPostSharpenName(requested),
			(const char*)nri_api,
			NRIGetPostSharpenName(resolved));
		mLastPostSharpenRequest = (int)nri_postsharpen;
		mLastPostSharpenResolved = resolved;
	}

	return resolved;
}

const char* NRIRenderer::GetFrameTextureSlotName(FrameTextureSlot slot) const
{
	switch (slot)
	{
	case FrameTextureSlot::ViewZ: return "ViewZ";
	case FrameTextureSlot::Motion: return "Motion";
	case FrameTextureSlot::NormalRoughness: return "NormalRoughness";
	case FrameTextureSlot::BaseColorMetalness: return "BaseColorMetalness";
	case FrameTextureSlot::UnfilteredDiffuse: return "UnfilteredDiffuse";
	case FrameTextureSlot::UnfilteredSpecular: return "UnfilteredSpecular";
	case FrameTextureSlot::UnfilteredPenumbra: return "UnfilteredPenumbra";
	case FrameTextureSlot::DenoisedDiffuse: return "DenoisedDiffuse";
	case FrameTextureSlot::DenoisedSpecular: return "DenoisedSpecular";
	case FrameTextureSlot::DenoisedShadow: return "DenoisedShadow";
	case FrameTextureSlot::Composed: return "Composed";
	case FrameTextureSlot::TraceTransparentOutput: return "TraceTransparentOutput";
	case FrameTextureSlot::DirectLighting: return "DirectLighting";
	case FrameTextureSlot::DirectEmission: return "DirectEmission";
	case FrameTextureSlot::TaaHistoryPing: return "TaaHistoryPing";
	case FrameTextureSlot::TaaHistoryPong: return "TaaHistoryPong";
	case FrameTextureSlot::Validation: return "Validation";
	case FrameTextureSlot::SrInput: return "SrInput";
	case FrameTextureSlot::RrInput: return "RrInput";
	case FrameTextureSlot::UpscalerDepth: return "UpscalerDepth";
	case FrameTextureSlot::RrGuideDiffuseAlbedo: return "RrGuideDiffuseAlbedo";
	case FrameTextureSlot::RrGuideSpecularAlbedo: return "RrGuideSpecularAlbedo";
	case FrameTextureSlot::RrGuideSpecularHitDistance: return "RrGuideSpecularHitDistance";
	case FrameTextureSlot::RrGuideNormalRoughness: return "RrGuideNormalRoughness";
	case FrameTextureSlot::SmokeVolumeCurrent: return "SmokeVolumeCurrent";
	case FrameTextureSlot::SmokeVolumeCurrentMeta: return "SmokeVolumeCurrentMeta";
	case FrameTextureSlot::SmokeVolumeHistoryPing: return "SmokeVolumeHistoryPing";
	case FrameTextureSlot::SmokeVolumeHistoryPong: return "SmokeVolumeHistoryPong";
	case FrameTextureSlot::SmokeVolumeMetaPing: return "SmokeVolumeMetaPing";
	case FrameTextureSlot::SmokeVolumeMetaPong: return "SmokeVolumeMetaPong";
	case FrameTextureSlot::RrVolumeInput: return "RrVolumeInput";
	case FrameTextureSlot::VendorOutput: return "VendorOutput";
	case FrameTextureSlot::PostSharpenOutput: return "PostSharpenOutput";
	case FrameTextureSlot::PostVolumeOutput: return "PostVolumeOutput";
	case FrameTextureSlot::PostBloomOutput: return "PostBloomOutput";
	case FrameTextureSlot::BloomPyramid0: return "BloomPyramid0";
	case FrameTextureSlot::BloomPyramid1: return "BloomPyramid1";
	case FrameTextureSlot::BloomPyramid2: return "BloomPyramid2";
	case FrameTextureSlot::BloomPyramid3: return "BloomPyramid3";
	case FrameTextureSlot::BloomPyramid4: return "BloomPyramid4";
	case FrameTextureSlot::BloomPyramid5: return "BloomPyramid5";
	case FrameTextureSlot::BloomPyramid6: return "BloomPyramid6";
	case FrameTextureSlot::BloomPyramid7: return "BloomPyramid7";
	case FrameTextureSlot::Final: return "Final";
	case FrameTextureSlot::Count: return "Count";
	default: return "Unknown";
	}
}

NRIMainUpscalerKind NRIRenderer::GetSelectedMainUpscalerKind() const
{
	NRISyncLegacyUpscalerConfig(false);
	switch ((int)nri_upscaler)
	{
	default:
	case 0: return NRIMainUpscalerKind::Off;
	case 2: return NRIMainUpscalerKind::DLSR;
	case 3: return NRIMainUpscalerKind::DLRR;
	}
}

NRIPostSharpenKind NRIRenderer::GetSelectedPostSharpenKind() const
{
	NRISyncLegacyUpscalerConfig(false);
	switch ((int)nri_postsharpen)
	{
	default:
	case 0: return NRIPostSharpenKind::Off;
	case 1: return NRIPostSharpenKind::NIS;
	}
}

NRIMainUpscalerKind NRIRenderer::GetResolvedMainUpscalerKindForStatus() const
{
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			return
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			return NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	return requested;
}

NRIPostSharpenKind NRIRenderer::GetResolvedPostSharpenKindForStatus() const
{
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		return NRIPostSharpenKind::Off;
	}

	return requested;
}

bool NRIRenderer::ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const
{
	return NRIShouldRunAppTaa(kind);
}

nri::UpscalerMode NRIRenderer::GetSelectedUpscalerMode() const
{
	switch ((int)nri_upscalermode)
	{
	default:
	case 0: return nri::UpscalerMode::NATIVE;
	case 1: return nri::UpscalerMode::ULTRA_QUALITY;
	case 2: return nri::UpscalerMode::QUALITY;
	case 3: return nri::UpscalerMode::BALANCED;
	case 4: return nri::UpscalerMode::PERFORMANCE;
	case 5: return nri::UpscalerMode::ULTRA_PERFORMANCE;
	}
}

void NRIRenderer::FillMatrix(float* outMatrix, const VSMatrix& matrix) const
{
	const_cast<VSMatrix&>(matrix).copy(outMatrix);
}
