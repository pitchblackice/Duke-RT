#pragma once

#include "nri_resources.h"

#include "NRD.h"
#include "NRDIntegration.h"

struct NRINrdDispatchDesc
{
	nri::CommandBuffer* commandBuffer = nullptr;
	NRITextureResource* motion = nullptr;
	NRITextureResource* viewZ = nullptr;
	NRITextureResource* normalRoughness = nullptr;
	NRITextureResource* baseColorMetalness = nullptr;
	NRITextureResource* unfilteredDiffuse = nullptr;
	NRITextureResource* unfilteredSpecular = nullptr;
	NRITextureResource* diffuse = nullptr;
	NRITextureResource* specular = nullptr;
	NRITextureResource* validation = nullptr;
	uint32_t resourceWidth = 0;
	uint32_t resourceHeight = 0;
	uint32_t frameIndex = 0;
	float cameraJitter[2] = {};
	float cameraJitterPrev[2] = {};
	float viewToClipMatrix[16] = {};
	float viewToClipMatrixPrev[16] = {};
	float worldToViewMatrix[16] = {};
	float worldToViewMatrixPrev[16] = {};
	bool resetHistory = false;
	bool enableValidation = false;
};

class NRINrdContext
{
public:
	bool EnsureReady(nri::Device& device, uint32_t width, uint32_t height, uint8_t queuedFrameNum);
	void NewFrame();
	bool Denoise(const NRINrdDispatchDesc& desc);
	void Shutdown();
	bool IsReady() const { return mInitialized; }

private:
	static nrd::Resource MakeResource(NRITextureResource& texture);

	nrd::Integration mIntegration;
	nrd::Identifier mDenoiser = 0;
	uint32_t mWidth = 0;
	uint32_t mHeight = 0;
	bool mInitialized = false;
};
