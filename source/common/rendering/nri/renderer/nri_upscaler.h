#pragma once

#include "../system/nri_local.h"

class NRIRenderDevice;

enum class NRIUpscalerKind : uint32_t
{
	Off = 0,
	NIS = 1,
	DLSR = 2,
	DLRR = 3,
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
	bool EnsureReady(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight);
	bool Dispatch(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, const NRIUpscalerDispatchDesc& desc);
	void Shutdown(NRIRenderDevice& frameBuffer);

private:
	bool EnsureUpscaler(NRIRenderDevice& frameBuffer, NRIUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight);
	void DestroyUpscaler(NRIRenderDevice& frameBuffer, nri::Upscaler*& upscaler);

	nri::Upscaler* mNis = nullptr;
	nri::Upscaler* mDlsr = nullptr;
	nri::Upscaler* mDlrr = nullptr;
	nri::UpscalerMode mMode = nri::UpscalerMode::QUALITY;
	uint32_t mUpscaleWidth = 0;
	uint32_t mUpscaleHeight = 0;
};
