#include "nri_upscaler.h"

#include "../system/nri_renderdevice.h"
#include "printf.h"

#include <cstring>

namespace
{
	nri::Upscaler* SelectUpscaler(NRIUpscalerKind kind, nri::Upscaler* nis, nri::Upscaler* dlsr, nri::Upscaler* dlrr)
	{
		switch (kind)
		{
		case NRIUpscalerKind::NIS: return nis;
		case NRIUpscalerKind::DLSR: return dlsr;
		case NRIUpscalerKind::DLRR: return dlrr;
		default: return nullptr;
		}
	}

	nri::UpscalerType ToUpscalerType(NRIUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIUpscalerKind::NIS: return nri::UpscalerType::NIS;
		case NRIUpscalerKind::DLSR: return nri::UpscalerType::DLSR;
		case NRIUpscalerKind::DLRR: return nri::UpscalerType::DLRR;
		default: return nri::UpscalerType::NIS;
		}
	}
}

bool NRIUpscalerContext::EnsureUpscaler(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight)
{
	if (kind == NRIUpscalerKind::Off)
	{
		return true;
	}

	nri::Upscaler*& slot =
		kind == NRIUpscalerKind::NIS ? mNis :
		kind == NRIUpscalerKind::DLSR ? mDlsr :
		mDlrr;

	if (slot != nullptr && mMode == mode && mUpscaleWidth == upscaleWidth && mUpscaleHeight == upscaleHeight)
	{
		return true;
	}

	DestroyUpscaler(frameBuffer, slot);

	const nri::UpscalerType type = ToUpscalerType(kind);
	if (!frameBuffer.mUpscaler.IsUpscalerSupported(*frameBuffer.mDevice, type))
	{
		return false;
	}

	nri::UpscalerDesc upscalerDesc = {};
	upscalerDesc.upscaleResolution = { (nri::Dim_t)upscaleWidth, (nri::Dim_t)upscaleHeight };
	upscalerDesc.type = type;
	upscalerDesc.mode = mode;
	upscalerDesc.commandBuffer = frameBuffer.mCommandBuffer;
	upscalerDesc.flags = nri::UpscalerBits::HDR;
	if (kind == NRIUpscalerKind::DLSR || kind == NRIUpscalerKind::DLRR)
	{
		upscalerDesc.flags = (nri::UpscalerBits)((uint32_t)nri::UpscalerBits::HDR | (uint32_t)nri::UpscalerBits::DEPTH_LINEAR);
	}

	if (frameBuffer.mUpscaler.CreateUpscaler(*frameBuffer.mDevice, upscalerDesc, slot) != nri::Result::SUCCESS)
	{
		slot = nullptr;
		return false;
	}

	mMode = mode;
	mUpscaleWidth = upscaleWidth;
	mUpscaleHeight = upscaleHeight;
	return true;
}

bool NRIUpscalerContext::EnsureReady(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight)
{
	return EnsureUpscaler(frameBuffer, kind, mode, upscaleWidth, upscaleHeight);
}

bool NRIUpscalerContext::Dispatch(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, const NRIUpscalerDispatchDesc& desc)
{
	nri::Upscaler* upscaler = SelectUpscaler(kind, mNis, mDlsr, mDlrr);
	if (upscaler == nullptr || desc.commandBuffer == nullptr || desc.input == nullptr || desc.output == nullptr)
	{
		return false;
	}

	nri::DispatchUpscaleDesc dispatchDesc = {};
	dispatchDesc.output = { desc.output->texture, desc.output->storageView };
	dispatchDesc.input = { desc.input->texture, desc.input->shaderView };
	dispatchDesc.currentResolution = { (nri::Dim_t)desc.currentWidth, (nri::Dim_t)desc.currentHeight };
	dispatchDesc.cameraJitter = { desc.cameraJitter[0], desc.cameraJitter[1] };
	dispatchDesc.mvScale = { 1.0f, 1.0f };
	dispatchDesc.flags = desc.resetHistory ? nri::DispatchUpscaleBits::RESET_HISTORY : nri::DispatchUpscaleBits::NONE;

	if (kind == NRIUpscalerKind::NIS)
	{
		dispatchDesc.settings.nis.sharpness = desc.sharpness;
	}
	else if (kind == NRIUpscalerKind::DLSR)
	{
		if (desc.motion == nullptr || desc.depth == nullptr)
		{
			return false;
		}

		dispatchDesc.guides.upscaler.mv = { desc.motion->texture, desc.motion->shaderView };
		dispatchDesc.guides.upscaler.depth = { desc.depth->texture, desc.depth->shaderView };
	}
	else if (kind == NRIUpscalerKind::DLRR)
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
	DestroyUpscaler(frameBuffer, mNis);
	DestroyUpscaler(frameBuffer, mDlsr);
	DestroyUpscaler(frameBuffer, mDlrr);
	mUpscaleWidth = 0;
	mUpscaleHeight = 0;
}
