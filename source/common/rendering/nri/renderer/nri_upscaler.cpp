#include "nri_upscaler.h"

#include "../system/nri_renderdevice.h"
#include "printf.h"

#include <cstring>

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
		slot.upscaleHeight == upscaleHeight)
	{
		return true;
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

	if (frameBuffer.mUpscaler.CreateUpscaler(*frameBuffer.mDevice, upscalerDesc, slot.instance) != nri::Result::SUCCESS)
	{
		slot.instance = nullptr;
		return false;
	}

	slot.mode = mode;
	slot.upscaleWidth = upscaleWidth;
	slot.upscaleHeight = upscaleHeight;
	return true;
}

bool NRIUpscalerContext::EnsureMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight)
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
	const nri::UpscalerBits flags = (nri::UpscalerBits)((uint32_t)nri::UpscalerBits::HDR | (uint32_t)nri::UpscalerBits::DEPTH_LINEAR);
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
	dispatchDesc.cameraJitter = { desc.cameraJitter[0], desc.cameraJitter[1] };
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
