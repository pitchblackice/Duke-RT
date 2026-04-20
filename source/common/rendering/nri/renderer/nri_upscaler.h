#pragma once

#include "../system/nri_local.h"

class NRIRenderDevice;

enum class NRIMainUpscalerKind : uint32_t
{
	Off = 0,
	DLSR = 2,
	DLRR = 3,
};

enum class NRIPostSharpenKind : uint32_t
{
	Off = 0,
	NIS = 1,
};

struct NRIUpscalerDispatchDesc
{
	nri::CommandBuffer* commandBuffer = nullptr;
	NRITextureResource* input = nullptr;
	NRITextureResource* output = nullptr;
	NRITextureResource* motion = nullptr;
	NRITextureResource* depth = nullptr;
	NRITextureResource* normalRoughness = nullptr;
	NRITextureResource* diffuseAlbedo = nullptr;
	NRITextureResource* specularAlbedo = nullptr;
	NRITextureResource* specularHitDistance = nullptr;
	uint32_t currentWidth = 0;
	uint32_t currentHeight = 0;
	float cameraJitter[2] = {};
	float viewToClipMatrix[16] = {};
	float worldToViewMatrix[16] = {};
	float sharpness = 0.2f;
	bool resetHistory = false;
};

class NRIUpscalerContext
{
public:
	bool EnsureMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight);
	bool DispatchMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc);
	bool EnsurePostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, uint32_t upscaleWidth, uint32_t upscaleHeight);
	bool DispatchPostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc);
	void Shutdown(NRIRenderDevice& frameBuffer);

private:
	struct UpscalerSlotState
	{
		nri::Upscaler* instance = nullptr;
		nri::UpscalerMode mode = nri::UpscalerMode::QUALITY;
		uint32_t upscaleWidth = 0;
		uint32_t upscaleHeight = 0;
	};

	bool EnsureUpscaler(
		NRIRenderDevice& frameBuffer,
		UpscalerSlotState& slot,
		nri::UpscalerType type,
		nri::UpscalerMode mode,
		uint32_t upscaleWidth,
		uint32_t upscaleHeight,
		nri::UpscalerBits flags);
	void DestroyUpscaler(NRIRenderDevice& frameBuffer, nri::Upscaler*& upscaler);

	UpscalerSlotState mNis = {};
	UpscalerSlotState mDlsr = {};
	UpscalerSlotState mDlrr = {};
};
