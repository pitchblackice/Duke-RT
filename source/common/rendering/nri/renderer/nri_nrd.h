#pragma once

#include "nri_resources.h"

#include "NRD.h"
#include "NRDSettings.h"
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
	float fastHistoryClampingSigmaScale = 1.25f;
	float diffusePrepassBlurRadius = 0.0f;
	float specularPrepassBlurRadius = 4.0f;
	float minBlurRadius = 0.0f;
	float maxBlurRadius = 4.0f;
	uint32_t maxAccumulatedFrameNum = 31;
	uint32_t maxFastAccumulatedFrameNum = 7;
	uint32_t maxStabilizedFrameNum = 31;
	uint32_t hitDistanceReconstructionMode = 0;
	bool resetHistory = false;
	bool enableAntiFirefly = true;
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
	nrd::ReblurSettings mReblurSettings = {};
	uint32_t mWidth = 0;
	uint32_t mHeight = 0;
	bool mInitialized = false;
	bool mHasReblurSettings = false;
};
