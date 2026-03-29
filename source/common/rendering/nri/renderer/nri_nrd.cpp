#include "nri_nrd.h"

#include "printf.h"
#include "NRDIntegration.hpp"

#include <algorithm>
#include <cstring>

namespace
{
	const nrd::DenoiserDesc gDenoisers[] = {
		{ nrd::Identifier(1), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR },
		{ nrd::Identifier(2), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR },
		{ nrd::Identifier(3), nrd::Denoiser::SIGMA_SHADOW }
	};

	static bool UseRelax(NRINrdDenoiserMode mode)
	{
		return mode == NRINrdDenoiserMode::Relax;
	}

	static nrd::HitDistanceReconstructionMode ClampHitDistanceReconstructionMode(uint32_t value)
	{
		switch (value)
		{
		case 1: return nrd::HitDistanceReconstructionMode::AREA_3X3;
		case 2: return nrd::HitDistanceReconstructionMode::AREA_5X5;
		default: return nrd::HitDistanceReconstructionMode::OFF;
		}
	}

	static nrd::ReblurSettings BuildReblurSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::ReblurSettings settings = {};
		settings.maxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.maxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.maxAccumulatedFrameNum);
		settings.maxStabilizedFrameNum = std::min(desc.maxStabilizedFrameNum, settings.maxAccumulatedFrameNum);
		settings.hitDistanceReconstructionMode = ClampHitDistanceReconstructionMode(desc.hitDistanceReconstructionMode);
		settings.enableAntiFirefly = desc.enableAntiFirefly;
		settings.fastHistoryClampingSigmaScale = desc.fastHistoryClampingSigmaScale;
		settings.diffusePrepassBlurRadius = desc.diffusePrepassBlurRadius;
		settings.specularPrepassBlurRadius = desc.specularPrepassBlurRadius;
		settings.minBlurRadius = desc.minBlurRadius;
		settings.maxBlurRadius = std::max(desc.maxBlurRadius, settings.minBlurRadius);

		// Keep the sample-style material floors, but expose the dominant spatial controls live
		// because current Raze gameplay still denoises a mixed direct+indirect signal.
		settings.minMaterialForDiffuse = 1.0f;
		settings.minMaterialForSpecular = 2.0f;
		settings.usePrepassOnlyForSpecularMotionEstimation = settings.specularPrepassBlurRadius > 0.0f;

		return settings;
	}

	static nrd::RelaxSettings BuildRelaxSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::RelaxSettings settings = {};
		settings.diffuseMaxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.specularMaxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.diffuseMaxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.diffuseMaxAccumulatedFrameNum);
		settings.specularMaxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.specularMaxAccumulatedFrameNum);
		settings.hitDistanceReconstructionMode = ClampHitDistanceReconstructionMode(desc.hitDistanceReconstructionMode);
		settings.enableAntiFirefly = desc.enableAntiFirefly;
		settings.fastHistoryClampingSigmaScale = desc.fastHistoryClampingSigmaScale;
		settings.diffusePrepassBlurRadius = desc.diffusePrepassBlurRadius;
		settings.specularPrepassBlurRadius = desc.specularPrepassBlurRadius;
		settings.minMaterialForDiffuse = 1.0f;
		settings.minMaterialForSpecular = 2.0f;
		return settings;
	}

	static nrd::SigmaSettings BuildSigmaSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::SigmaSettings settings = {};
		settings.maxStabilizedFrameNum = std::min(desc.sigmaMaxStabilizedFrameNum, nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
		settings.planeDistanceSensitivity = desc.sigmaPlaneDistanceSensitivity;
		settings.lightDirection[0] = desc.lightDirection[0];
		settings.lightDirection[1] = desc.lightDirection[1];
		settings.lightDirection[2] = desc.lightDirection[2];
		return settings;
	}

	static bool SigmaSettingsEqual(const nrd::SigmaSettings& a, const nrd::SigmaSettings& b)
	{
		return
			a.maxStabilizedFrameNum == b.maxStabilizedFrameNum &&
			a.planeDistanceSensitivity == b.planeDistanceSensitivity &&
			a.lightDirection[0] == b.lightDirection[0] &&
			a.lightDirection[1] == b.lightDirection[1] &&
			a.lightDirection[2] == b.lightDirection[2];
	}
}

nrd::Resource NRINrdContext::MakeResource(NRITextureResource& texture)
{
	nrd::Resource resource = {};
	resource.nri.texture = texture.texture;
	resource.state = texture.state;
	resource.userArg = &texture;
	return resource;
}

bool NRINrdContext::EnsureReady(nri::Device& device, uint32_t width, uint32_t height, uint8_t queuedFrameNum)
{
	if (mInitialized && mWidth == width && mHeight == height)
	{
		return true;
	}

	Shutdown();

	nrd::InstanceCreationDesc instanceCreationDesc = {};
	instanceCreationDesc.denoisers = gDenoisers;
	instanceCreationDesc.denoisersNum = (uint32_t)std::size(gDenoisers);

	nrd::IntegrationCreationDesc integrationDesc = {};
	strcpy_s(integrationDesc.name, "RazeNRI");
	integrationDesc.resourceWidth = (uint16_t)width;
	integrationDesc.resourceHeight = (uint16_t)height;
	integrationDesc.queuedFrameNum = queuedFrameNum;
	integrationDesc.autoWaitForIdle = false;

	if (mIntegration.Recreate(integrationDesc, instanceCreationDesc, &device) != nrd::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "NRI NRD integration creation failed.\n");
		return false;
	}

	mReblurDenoiser = gDenoisers[0].identifier;
	mRelaxDenoiser = gDenoisers[1].identifier;
	mSigmaDenoiser = gDenoisers[2].identifier;
	mWidth = width;
	mHeight = height;
	mInitialized = true;
	mHasReblurSettings = false;
	mHasRelaxSettings = false;
	mHasSigmaSettings = false;
	mLastDenoiserMode = NRINrdDenoiserMode::Reblur;
	return true;
}

void NRINrdContext::NewFrame()
{
	if (mInitialized)
	{
		mIntegration.NewFrame();
	}
}

bool NRINrdContext::Denoise(const NRINrdDispatchDesc& desc)
{
	if (!mInitialized || desc.commandBuffer == nullptr || desc.motion == nullptr || desc.viewZ == nullptr ||
		desc.normalRoughness == nullptr || desc.baseColorMetalness == nullptr ||
		desc.unfilteredDiffuse == nullptr || desc.unfilteredSpecular == nullptr ||
		desc.diffuse == nullptr || desc.specular == nullptr || desc.validation == nullptr)
	{
		return false;
	}
	if (desc.enableSigmaShadow && (desc.unfilteredPenumbra == nullptr || desc.shadow == nullptr))
	{
		return false;
	}

	const bool useRelax = UseRelax(desc.denoiserMode);
	const bool denoiserChanged = desc.denoiserMode != mLastDenoiserMode;
	bool settingsChanged = false;
	nrd::Identifier activeDenoiser = useRelax ? mRelaxDenoiser : mReblurDenoiser;
	bool sigmaSettingsChanged = false;

	if (desc.enableSigmaShadow)
	{
		const nrd::SigmaSettings sigmaSettings = BuildSigmaSettings(desc);
		sigmaSettingsChanged =
			!mHasSigmaSettings ||
			!SigmaSettingsEqual(mSigmaSettings, sigmaSettings);

		// Match the sample more closely here: SIGMA settings are pushed every frame,
		// while history restart is still keyed off the explicit field comparison above.
		if (mIntegration.SetDenoiserSettings(mSigmaDenoiser, &sigmaSettings) != nrd::Result::SUCCESS)
		{
			return false;
		}

		mSigmaSettings = sigmaSettings;
		mHasSigmaSettings = true;
	}

	if (useRelax)
	{
		const nrd::RelaxSettings relaxSettings = BuildRelaxSettings(desc);
		settingsChanged =
			!mHasRelaxSettings ||
			std::memcmp(&mRelaxSettings, &relaxSettings, sizeof(relaxSettings)) != 0;

		if (settingsChanged)
		{
			if (mIntegration.SetDenoiserSettings(activeDenoiser, &relaxSettings) != nrd::Result::SUCCESS)
			{
				return false;
			}

			mRelaxSettings = relaxSettings;
			mHasRelaxSettings = true;
		}
	}
	else
	{
		const nrd::ReblurSettings reblurSettings = BuildReblurSettings(desc);
		settingsChanged =
			!mHasReblurSettings ||
			std::memcmp(&mReblurSettings, &reblurSettings, sizeof(reblurSettings)) != 0;

		if (settingsChanged)
		{
			if (mIntegration.SetDenoiserSettings(activeDenoiser, &reblurSettings) != nrd::Result::SUCCESS)
			{
				return false;
			}

			mReblurSettings = reblurSettings;
			mHasReblurSettings = true;
		}
	}

	mLastDenoiserMode = desc.denoiserMode;

	nrd::CommonSettings commonSettings = {};
	std::memcpy(commonSettings.viewToClipMatrix, desc.viewToClipMatrix, sizeof(commonSettings.viewToClipMatrix));
	std::memcpy(commonSettings.viewToClipMatrixPrev, desc.viewToClipMatrixPrev, sizeof(commonSettings.viewToClipMatrixPrev));
	std::memcpy(commonSettings.worldToViewMatrix, desc.worldToViewMatrix, sizeof(commonSettings.worldToViewMatrix));
	std::memcpy(commonSettings.worldToViewMatrixPrev, desc.worldToViewMatrixPrev, sizeof(commonSettings.worldToViewMatrixPrev));
	// Raze feeds NRD the sample-shaped 2.5D screen-space contract:
	// - motion.xy written in pixel units by TraceOpaque
	// - motion.z written as viewZPrev - viewZ
	// NRD converts xy back to normalized screen space through motionVectorScale.
	commonSettings.motionVectorScale[0] = 1.0f / (float)desc.resourceWidth;
	commonSettings.motionVectorScale[1] = 1.0f / (float)desc.resourceHeight;
	commonSettings.motionVectorScale[2] = 1.0f;
	commonSettings.cameraJitter[0] = desc.cameraJitter[0];
	commonSettings.cameraJitter[1] = desc.cameraJitter[1];
	commonSettings.cameraJitterPrev[0] = desc.cameraJitterPrev[0];
	commonSettings.cameraJitterPrev[1] = desc.cameraJitterPrev[1];
	commonSettings.resourceSize[0] = (uint16_t)desc.resourceWidth;
	commonSettings.resourceSize[1] = (uint16_t)desc.resourceHeight;
	commonSettings.resourceSizePrev[0] = (uint16_t)desc.resourceWidth;
	commonSettings.resourceSizePrev[1] = (uint16_t)desc.resourceHeight;
	commonSettings.rectSize[0] = (uint16_t)desc.resourceWidth;
	commonSettings.rectSize[1] = (uint16_t)desc.resourceHeight;
	commonSettings.rectSizePrev[0] = (uint16_t)desc.resourceWidth;
	commonSettings.rectSizePrev[1] = (uint16_t)desc.resourceHeight;
	commonSettings.viewZScale = 1.0f;
	commonSettings.denoisingRange = 100000.0f;
	commonSettings.disocclusionThreshold = 0.01f;
	commonSettings.disocclusionThresholdAlternate = 0.05f;
	commonSettings.frameIndex = desc.frameIndex;
	commonSettings.accumulationMode = (desc.resetHistory || denoiserChanged || settingsChanged || sigmaSettingsChanged) ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	// Camera motion is already represented by the matrices above, so motion vectors stay in screen space here.
	commonSettings.isMotionVectorInWorldSpace = false;
	commonSettings.isBaseColorMetalnessAvailable = true;
	commonSettings.enableValidation = desc.enableValidation;

	if (mIntegration.SetCommonSettings(commonSettings) != nrd::Result::SUCCESS)
	{
		return false;
	}

	nrd::ResourceSnapshot resourceSnapshot = {};
	resourceSnapshot.restoreInitialState = false;
	resourceSnapshot.SetResource(nrd::ResourceType::IN_MV, MakeResource(*desc.motion));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_VIEWZ, MakeResource(*desc.viewZ));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, MakeResource(*desc.normalRoughness));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_BASECOLOR_METALNESS, MakeResource(*desc.baseColorMetalness));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, MakeResource(*desc.unfilteredDiffuse));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, MakeResource(*desc.unfilteredSpecular));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, MakeResource(*desc.diffuse));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, MakeResource(*desc.specular));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_VALIDATION, MakeResource(*desc.validation));
	if (desc.enableSigmaShadow)
	{
		resourceSnapshot.SetResource(nrd::ResourceType::IN_PENUMBRA, MakeResource(*desc.unfilteredPenumbra));
		resourceSnapshot.SetResource(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, MakeResource(*desc.shadow));
		const nrd::Identifier denoisers[] = { mSigmaDenoiser, activeDenoiser };
		mIntegration.Denoise(denoisers, 2, *desc.commandBuffer, resourceSnapshot);
	}
	else
	{
		mIntegration.Denoise(&activeDenoiser, 1, *desc.commandBuffer, resourceSnapshot);
	}

	for (size_t i = 0; i < resourceSnapshot.uniqueNum; ++i)
	{
		auto* texture = static_cast<NRITextureResource*>(resourceSnapshot.unique[i].userArg);
		if (texture != nullptr)
		{
			texture->state = resourceSnapshot.unique[i].state;
		}
	}

	return true;
}

void NRINrdContext::Shutdown()
{
	if (!mInitialized)
	{
		return;
	}

	mIntegration.Destroy();
	mInitialized = false;
	mWidth = 0;
	mHeight = 0;
	mReblurDenoiser = 0;
	mRelaxDenoiser = 0;
	mSigmaDenoiser = 0;
	mReblurSettings = {};
	mRelaxSettings = {};
	mSigmaSettings = {};
	mHasReblurSettings = false;
	mHasRelaxSettings = false;
	mHasSigmaSettings = false;
	mLastDenoiserMode = NRINrdDenoiserMode::Reblur;
}
