#include "nri_exposure.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_shader_contracts.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace
{
	constexpr uint32_t NRI_AUTO_EXPOSURE_DEBUG_MAGIC = 0x45585033u;

	float AsFloat(uint32_t value)
	{
		float result = 0.0f;
		std::memcpy(&result, &value, sizeof(result));
		return result;
	}

	float ClampAutoExposureDeltaTimeSeconds(float seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0f)
		{
			return 1.0f / 60.0f;
		}

		return std::clamp(seconds, 1.0f / 240.0f, 0.25f);
	}

	bool ShouldEmitAutoExposureTemporalTraceLogs()
	{
		return !!nri_pttemporaltrace && nri_pttraceframes > 0;
	}
}


const char* GetNRIAutoExposureMeteringModeName(NRIAutoExposureMeteringMode mode)
{
	switch (mode)
	{
	case NRIAutoExposureMeteringMode::FullFrame: return "full";
	case NRIAutoExposureMeteringMode::CenterWeighted: return "center";
	case NRIAutoExposureMeteringMode::BrightTailSuppressed: return "bright-tail";
	default: return "unknown";
	}
}

NRIAutoExposureSettings GetNRIAutoExposureSettings(float fallbackManualExposure, bool hdrControlsActive)
{
	NRIAutoExposureSettings settings = {};
	settings.hdrControlsActive = hdrControlsActive;
	settings.enabled = hdrControlsActive ? !!nri_pthdrautoexposure : !!nri_ptautoexposure;
	settings.freeze = !!nri_ptautoexposurefreeze;
	settings.stats = !!nri_ptautoexposurestats;
	settings.meteringMode = (NRIAutoExposureMeteringMode)std::clamp((int)nri_ptautoexposuremetering, 0, 2);
	settings.histogramBinCount = (uint32_t)std::clamp((int)nri_ptautoexposurebins, 16, 256);
	settings.sampleStep = (uint32_t)std::clamp((int)nri_ptautoexposuresamplestep, 1, 8);
	settings.targetLuminance = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuretarget : (float)nri_ptautoexposuretarget,
		0.02f,
		1.0f);
	settings.minExposure = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuremin : (float)nri_ptautoexposuremin,
		0.03125f,
		8.0f);
	settings.maxExposure = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposuremax : (float)nri_ptautoexposuremax,
		0.125f,
		32.0f);
	if (settings.maxExposure < settings.minExposure)
	{
		settings.maxExposure = settings.minExposure;
	}
	settings.exposureBias = std::clamp(
		hdrControlsActive ? (float)nri_pthdrautoexposurebias : (float)nri_ptautoexposurebias,
		0.125f,
		8.0f);
	settings.lowPercentile = std::clamp((float)nri_ptautoexposurelowpercentile, 0.0f, 99.0f);
	settings.highPercentile = std::clamp((float)nri_ptautoexposurehighpercentile, 1.0f, 100.0f);
	if (settings.highPercentile <= settings.lowPercentile)
	{
		settings.highPercentile = std::min(settings.lowPercentile + 1.0f, 100.0f);
	}
	if (settings.meteringMode == NRIAutoExposureMeteringMode::BrightTailSuppressed)
	{
		settings.highPercentile = std::min(settings.highPercentile, 95.0f);
		if (settings.highPercentile <= settings.lowPercentile)
		{
			settings.lowPercentile = std::max(settings.highPercentile - 1.0f, 0.0f);
		}
	}
	settings.adaptUpSpeed = std::clamp((float)nri_ptautoexposureadaptup, 0.0f, 16.0f);
	settings.adaptDownSpeed = std::clamp((float)nri_ptautoexposureadaptdown, 0.0f, 16.0f);
	settings.fallbackManualExposure = std::max(fallbackManualExposure, 0.0f);
	return settings;
}

const char* GetNRIAutoExposureResetReasonForSettingsChange(
	const NRIAutoExposureSettings& previous,
	const NRIAutoExposureSettings& current)
{
	if (previous.hdrControlsActive != current.hdrControlsActive)
	{
		return "auto-exposure-control-block-change";
	}
	if (previous.enabled != current.enabled)
	{
		return current.enabled ? "auto-exposure-enabled" : "auto-exposure-disabled";
	}
	if (previous.histogramBinCount != current.histogramBinCount ||
		previous.sampleStep != current.sampleStep ||
		previous.meteringMode != current.meteringMode ||
		previous.lowPercentile != current.lowPercentile ||
		previous.highPercentile != current.highPercentile)
	{
		return "auto-exposure-metering-change";
	}
	if (previous.minExposure != current.minExposure ||
		previous.maxExposure != current.maxExposure)
	{
		return "auto-exposure-clamp-change";
	}
	if (previous.targetLuminance != current.targetLuminance ||
		previous.exposureBias != current.exposureBias)
	{
		return "auto-exposure-target-change";
	}

	return nullptr;
}

uint64_t NRIExposurePassAccess::GetMemoryBytes(NRIRenderer& renderer)
{
	return
		renderer.mExposure.GetMutableExposureStateTexture(0).memorySize +
		renderer.mExposure.GetMutableExposureStateTexture(1).memorySize +
		renderer.mExposure.GetMutableHistogramBuffer().memorySize +
		renderer.mExposure.GetMutableDebugBuffer().memorySize +
		renderer.mExposure.GetMutableDebugReadbackBuffer().memorySize;
}

bool NRIExposurePassAccess::CreateStorageBuffer(NRIRenderer& renderer, NRIBufferResource& resource, uint64_t byteSize, uint32_t stride)
{
	renderer.DestroyBufferResource(resource);
	nri::BufferDesc desc = {};
	desc.size = byteSize;
	desc.structureStride = stride;
	desc.usage = NRIResourceFlags(
		nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
		nri::BufferUsageBits::SHADER_RESOURCE);
	if (renderer.mFrameBuffer->mCore.CreateCommittedBuffer(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	renderer.mFrameBuffer->mCore.GetBufferMemoryDesc(*resource.buffer, nri::MemoryLocation::DEVICE, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
	resource.usedSize = byteSize;
	resource.stride = stride;

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (renderer.mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		renderer.DestroyBufferResource(resource);
		return false;
	}

	return true;
}

bool NRIExposurePassAccess::EnsureStatsReadback(NRIRenderer& renderer, const NRIAutoExposureSettings& settings)
{
	NRIBufferResource& readbackBuffer = renderer.mExposure.GetMutableDebugReadbackBuffer();
	if (settings.stats)
	{
		if (readbackBuffer.buffer == nullptr)
		{
			if (!renderer.CreateBufferWithoutViewAtLocation(
				readbackBuffer,
				(uint64_t)NRI_EXPOSURE_DEBUG_WORD_COUNT * sizeof(uint32_t),
				sizeof(uint32_t),
				nri::BufferUsageBits::NONE,
				nri::MemoryLocation::HOST_READBACK))
			{
				return false;
			}
			renderer.mExposure.MarkResourcesAllocated(renderer.mRenderWidth, renderer.mRenderHeight, GetMemoryBytes(renderer));
		}
	}
	else if (readbackBuffer.buffer != nullptr)
	{
		renderer.DestroyBufferResource(readbackBuffer);
		renderer.mPendingAutoExposureStatsFrame = 0;
		renderer.mExposure.ClearDebugReadback();
		renderer.mExposure.MarkResourcesAllocated(renderer.mRenderWidth, renderer.mRenderHeight, GetMemoryBytes(renderer));
	}

	return true;
}

bool NRIExposurePassAccess::EnsureResources(NRIRenderer& renderer, const NRIAutoExposureSettings& settings)
{
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;

	renderer.mExposure.SetSettings(settings);
	if (!renderer.mExposure.ShouldAllocateResources())
	{
		DestroyResources(renderer);
		return true;
	}

	if (renderer.mExposure.HasRequiredResources() && renderer.mExposure.MatchesRenderSize(renderer.mRenderWidth, renderer.mRenderHeight))
	{
		return EnsureStatsReadback(renderer, settings);
	}

	DestroyResources(renderer);
	NRITextureResource& exposureState0 = renderer.mExposure.GetMutableExposureStateTexture(0);
	NRITextureResource& exposureState1 = renderer.mExposure.GetMutableExposureStateTexture(1);
	NRIBufferResource& histogramBuffer = renderer.mExposure.GetMutableHistogramBuffer();
	NRIBufferResource& debugBuffer = renderer.mExposure.GetMutableDebugBuffer();
	const nri::TextureUsageBits usage = NRIResourceFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE);
	if (!renderer.mFrameBuffer->CreateOwnedTexture(exposureState0, 1, 1, nri::Format::RGBA32_SFLOAT, usage) ||
		!renderer.mFrameBuffer->CreateOwnedTexture(exposureState1, 1, 1, nri::Format::RGBA32_SFLOAT, usage) ||
		!CreateStorageBuffer(renderer, histogramBuffer, (uint64_t)NRI_EXPOSURE_MAX_HISTOGRAM_BINS * sizeof(uint32_t), sizeof(uint32_t)) ||
		!CreateStorageBuffer(renderer, debugBuffer, (uint64_t)NRI_EXPOSURE_DEBUG_WORD_COUNT * sizeof(uint32_t), sizeof(uint32_t)) ||
		!EnsureStatsReadback(renderer, settings) ||
		!UpdateDescriptorSets(renderer, (uint32_t)FrameTextureSlot::TraceTransparentOutput))
	{
		DestroyResources(renderer);
		return false;
	}

	renderer.mExposure.MarkResourcesAllocated(renderer.mRenderWidth, renderer.mRenderHeight, GetMemoryBytes(renderer));
	renderer.RequestAutoExposureReset("auto-exposure-resources-created");
	return true;
}

void NRIExposurePassAccess::DestroyResources(NRIRenderer& renderer)
{
	renderer.mFrameBuffer->DestroyTextureResource(renderer.mExposure.GetMutableExposureStateTexture(0));
	renderer.mFrameBuffer->DestroyTextureResource(renderer.mExposure.GetMutableExposureStateTexture(1));
	renderer.DestroyBufferResource(renderer.mExposure.GetMutableHistogramBuffer());
	renderer.DestroyBufferResource(renderer.mExposure.GetMutableDebugBuffer());
	renderer.DestroyBufferResource(renderer.mExposure.GetMutableDebugReadbackBuffer());
	renderer.mPendingAutoExposureStatsFrame = 0;
	renderer.mAutoExposureInputSourceSlot = NRIRenderer::FrameTextureSlot::Count;
	renderer.mExposure.MarkResourcesDestroyed();
}

bool NRIExposurePassAccess::UpdateDescriptorSets(NRIRenderer& renderer, uint32_t sourceSlotValue)
{
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;

	const FrameTextureSlot sourceSlot = (FrameTextureSlot)sourceSlotValue;
	const NRITextureResource& source = renderer.GetFrameTexture(sourceSlot);
	if (source.shaderView == nullptr ||
		!renderer.mExposure.HasRequiredResources() ||
		renderer.mExposureInputSets[0] == nullptr ||
		renderer.mExposureInputSets[1] == nullptr ||
		renderer.mExposureOutputSets[0] == nullptr ||
		renderer.mExposureOutputSets[1] == nullptr)
	{
		return false;
	}

	for (uint32_t currentIndex = 0; currentIndex < 2; ++currentIndex)
	{
		const uint32_t previousIndex = 1u - currentIndex;
		const nri::Descriptor* inputDescriptors[NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM] = {
			source.shaderView,
			renderer.mExposure.GetMutableExposureStateTexture(previousIndex).shaderView
		};
		nri::UpdateDescriptorRangeDesc inputUpdate = {};
		inputUpdate.descriptorSet = renderer.mExposureInputSets[currentIndex];
		inputUpdate.rangeIndex = 0;
		inputUpdate.descriptors = inputDescriptors;
		inputUpdate.descriptorNum = NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM;
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

		const nri::Descriptor* outputTextureDescriptor = renderer.mExposure.GetMutableExposureStateTexture(currentIndex).storageView;
		nri::UpdateDescriptorRangeDesc outputTextureUpdate = {};
		outputTextureUpdate.descriptorSet = renderer.mExposureOutputSets[currentIndex];
		outputTextureUpdate.rangeIndex = 0;
		outputTextureUpdate.descriptors = &outputTextureDescriptor;
		outputTextureUpdate.descriptorNum = NRI_EXPOSURE_OUTPUT_TEXTURE_DESCRIPTOR_NUM;
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&outputTextureUpdate, 1);

		const nri::Descriptor* outputBufferDescriptors[NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM] = {
			renderer.mExposure.GetMutableHistogramBuffer().shaderView,
			renderer.mExposure.GetMutableDebugBuffer().shaderView
		};
		nri::UpdateDescriptorRangeDesc outputBufferUpdate = {};
		outputBufferUpdate.descriptorSet = renderer.mExposureOutputSets[currentIndex];
		outputBufferUpdate.rangeIndex = 1;
		outputBufferUpdate.descriptors = outputBufferDescriptors;
		outputBufferUpdate.descriptorNum = NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM;
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&outputBufferUpdate, 1);
	}

	renderer.mAutoExposureInputSourceSlot = sourceSlot;
	return true;
}

bool NRIExposurePassAccess::Dispatch(NRIRenderer& renderer, uint32_t sourceSlotValue)
{
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;
	using PipelineSlot = NRIRenderer::PipelineSlot;

	const FrameTextureSlot sourceSlot = (FrameTextureSlot)sourceSlotValue;
	const NRIAutoExposureSettings& settings = renderer.mExposure.GetSettings();
	if (!renderer.mExposure.ShouldAllocateResources())
	{
		return true;
	}

	if (!renderer.mExposure.HasRequiredResources())
	{
		return false;
	}

	if (renderer.mAutoExposureInputSourceSlot != sourceSlot && !UpdateDescriptorSets(renderer, sourceSlotValue))
	{
		return false;
	}

	const uint32_t currentIndex = renderer.mFrameIndex & 1u;
	const uint32_t previousIndex = 1u - currentIndex;
	NRITextureResource& source = renderer.GetFrameTexture(sourceSlot);
	NRITextureResource& previousExposureState = renderer.mExposure.GetMutableExposureStateTexture(previousIndex);
	NRITextureResource& currentExposureState = renderer.mExposure.GetMutableExposureStateTexture(currentIndex);
	NRIBufferResource& histogramBuffer = renderer.mExposure.GetMutableHistogramBuffer();
	NRIBufferResource& debugBuffer = renderer.mExposure.GetMutableDebugBuffer();
	if (source.texture == nullptr ||
		previousExposureState.texture == nullptr ||
		currentExposureState.texture == nullptr ||
		histogramBuffer.buffer == nullptr ||
		debugBuffer.buffer == nullptr)
	{
		return false;
	}

	NRIExposureConstants constants = {};
	constants.RenderWidth = renderer.mRenderWidth;
	constants.RenderHeight = renderer.mRenderHeight;
	constants.FrameIndex = renderer.mFrameIndex;
	const bool resetExposure = renderer.mExposure.ConsumeResetRequest((uint64_t)renderer.mFrameIndex);
	constants.Flags =
		(settings.freeze ? NRI_EXPOSURE_FLAG_FREEZE : 0u) |
		(resetExposure ? NRI_EXPOSURE_FLAG_RESET : 0u);
	constants.HistogramBinCount = std::clamp(settings.histogramBinCount, 1u, NRI_EXPOSURE_MAX_HISTOGRAM_BINS);
	constants.SampleStep = std::max(settings.sampleStep, 1u);
	constants.MeteringMode = (uint32_t)settings.meteringMode;
	const float exposureDeltaTimeSeconds =
		renderer.mHasPendingFrameGenerationRealFrameTime ?
		renderer.mPendingFrameGenerationRealFrameTimeMs * 0.001f :
		1.0f / 60.0f;
	constants.DeltaTimeSeconds = ClampAutoExposureDeltaTimeSeconds(exposureDeltaTimeSeconds);
	constants.LogLuminanceMin = NRI_EXPOSURE_LOG_LUMINANCE_MIN;
	constants.LogLuminanceMax = NRI_EXPOSURE_LOG_LUMINANCE_MAX;
	constants.InvLogLuminanceRange = 1.0f / (constants.LogLuminanceMax - constants.LogLuminanceMin);
	constants.TargetLuminance = settings.targetLuminance;
	constants.MinExposure = settings.minExposure;
	constants.MaxExposure = settings.maxExposure;
	constants.ExposureBias = settings.exposureBias;
	constants.LowPercentile = settings.lowPercentile;
	constants.HighPercentile = settings.highPercentile;
	constants.FallbackManualExposure = settings.fallbackManualExposure;
	constants.AdaptUpSpeed = settings.adaptUpSpeed;
	constants.AdaptDownSpeed = settings.adaptDownSpeed;

	renderer.mFrameBuffer->TransitionTexture(source, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(previousExposureState, NRIComputeShaderResourceState());
	renderer.mFrameBuffer->TransitionTexture(currentExposureState, NRIComputeStorageState());

	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = histogramBuffer.buffer;
	beforeBarriers[0].before = {};
	beforeBarriers[0].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	beforeBarriers[1].buffer = debugBuffer.buffer;
	beforeBarriers[1].before = {};
	beforeBarriers[1].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = (uint32_t)std::size(beforeBarriers);
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, beforeDesc);

	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *renderer.mExposurePipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, renderer.mExposureInputSets[currentIndex], nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, renderer.mExposureOutputSets[currentIndex], nri::BindPoint::COMPUTE });

	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(PipelineSlot::ExposureHistogramClear));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { 1, 1, 1 });

	nri::BufferBarrierDesc buildBarriers[2] = {};
	buildBarriers[0].buffer = histogramBuffer.buffer;
	buildBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	buildBarriers[0].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	buildBarriers[1].buffer = debugBuffer.buffer;
	buildBarriers[1].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	buildBarriers[1].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc buildDesc = {};
	buildDesc.buffers = buildBarriers;
	buildDesc.bufferNum = (uint32_t)std::size(buildBarriers);
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, buildDesc);

	const uint32_t sampledWidth = (renderer.mRenderWidth + constants.SampleStep - 1u) / constants.SampleStep;
	const uint32_t sampledHeight = (renderer.mRenderHeight + constants.SampleStep - 1u) / constants.SampleStep;
	if (ShouldEmitAutoExposureTemporalTraceLogs())
	{
		const NRIAutoExposureStatus& status = renderer.mExposure.GetStatus();
		Printf("NRI PT auto exposure trace: stage=dispatch frame=%u source=%s meter_mode=%s reset=%s freeze=%s stats=%s bins=%u sample_step=%u sampled=%ux%u dispatch=%ux%u percentiles=%.2f..%.2f hist_log_range=%.1f..%.1f target_lum=%.3f range=%.3f..%.3f bias=%.3f dt=%.4f last_valid=%s last_frame=%llu last_metered_log_lum=%.3f last_target=%.3f last_adapted=%.3f last_adapted_ev=%.3f reset_serial=%llu reset_reason=%s\n",
			renderer.mFrameIndex,
			renderer.GetFrameTextureSlotName(sourceSlot),
			GetNRIAutoExposureMeteringModeName(settings.meteringMode),
			resetExposure ? "yes" : "no",
			settings.freeze ? "yes" : "no",
			settings.stats ? "yes" : "no",
			constants.HistogramBinCount,
			constants.SampleStep,
			sampledWidth,
			sampledHeight,
			(sampledWidth + 15u) / 16u,
			(sampledHeight + 15u) / 16u,
			settings.lowPercentile,
			settings.highPercentile,
			constants.LogLuminanceMin,
			constants.LogLuminanceMax,
			settings.targetLuminance,
			settings.minExposure,
			settings.maxExposure,
			settings.exposureBias,
			constants.DeltaTimeSeconds,
			status.debugValid ? "yes" : "no",
			(unsigned long long)status.debugFrameIndex,
			status.meteredLogLuminance,
			status.targetExposure,
			status.adaptedExposure,
			std::log2(std::max(status.adaptedExposure, 1.0e-6f)),
			(unsigned long long)status.resetSerial,
			status.resetReason[0] != '\0' ? status.resetReason : "none");
	}
	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(PipelineSlot::ExposureHistogramBuild));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { (sampledWidth + 15u) / 16u, (sampledHeight + 15u) / 16u, 1 });

	nri::BufferBarrierDesc resolveBarriers[2] = {};
	resolveBarriers[0].buffer = histogramBuffer.buffer;
	resolveBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	resolveBarriers[0].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	resolveBarriers[1].buffer = debugBuffer.buffer;
	resolveBarriers[1].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	resolveBarriers[1].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc resolveDesc = {};
	resolveDesc.buffers = resolveBarriers;
	resolveDesc.bufferNum = (uint32_t)std::size(resolveBarriers);
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, resolveDesc);

	renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *renderer.GetPipeline(PipelineSlot::ExposureResolve));
	renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { 1, 1, 1 });

	if (settings.stats)
	{
		CopyStatsForReadback(renderer, (uint64_t)renderer.mFrameIndex);
	}

	return true;
}

void NRIExposurePassAccess::CopyStatsForReadback(NRIRenderer& renderer, uint64_t frameNumber)
{
	if (!renderer.mExposure.GetSettings().stats ||
		renderer.mFrameBuffer == nullptr ||
		renderer.mFrameBuffer->mCommandBuffer == nullptr ||
		renderer.mExposure.GetMutableDebugBuffer().buffer == nullptr ||
		renderer.mExposure.GetMutableDebugReadbackBuffer().buffer == nullptr)
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_EXPOSURE_DEBUG_WORD_COUNT * sizeof(uint32_t);
	NRIBufferResource& debugBuffer = renderer.mExposure.GetMutableDebugBuffer();
	NRIBufferResource& readbackBuffer = renderer.mExposure.GetMutableDebugReadbackBuffer();

	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = debugBuffer.buffer;
	beforeBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	beforeBarriers[0].after = NRIResourceCopySourceAccess();
	beforeBarriers[1].buffer = readbackBuffer.buffer;
	beforeBarriers[1].before = {};
	beforeBarriers[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = (uint32_t)std::size(beforeBarriers);
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, beforeDesc);

	renderer.mFrameBuffer->mCore.CmdCopyBuffer(
		*renderer.mFrameBuffer->mCommandBuffer,
		*readbackBuffer.buffer,
		0,
		*debugBuffer.buffer,
		0,
		byteSize);

	renderer.mPendingAutoExposureStatsFrame = frameNumber;
}

void NRIExposurePassAccess::ReadbackStats(NRIRenderer& renderer)
{
	if (!renderer.mExposure.GetSettings().stats ||
		renderer.mPendingAutoExposureStatsFrame == 0 ||
		renderer.mExposure.GetMutableDebugReadbackBuffer().buffer == nullptr)
	{
		return;
	}

	renderer.WaitForCommandsTracked("auto_exposure_stats_readback");
	const uint64_t byteSize = (uint64_t)NRI_EXPOSURE_DEBUG_WORD_COUNT * sizeof(uint32_t);
	const void* mapped = renderer.mFrameBuffer->mCore.MapBuffer(*renderer.mExposure.GetMutableDebugReadbackBuffer().buffer, 0, byteSize);
	if (mapped == nullptr)
	{
		renderer.mPendingAutoExposureStatsFrame = 0;
		renderer.mExposure.ClearDebugReadback();
		return;
	}

	uint32_t words[NRI_EXPOSURE_DEBUG_WORD_COUNT] = {};
	std::memcpy(words, mapped, (size_t)byteSize);
	renderer.mFrameBuffer->mCore.UnmapBuffer(*renderer.mExposure.GetMutableDebugReadbackBuffer().buffer);
	if (words[0] == NRI_EXPOSURE_DEBUG_MAGIC)
	{
		renderer.mExposure.MarkDebugReadback(renderer.mPendingAutoExposureStatsFrame, words, NRI_EXPOSURE_DEBUG_WORD_COUNT);
		if (ShouldEmitAutoExposureTemporalTraceLogs())
		{
			const NRIAutoExposureSettings& settings = renderer.mExposure.GetSettings();
			const NRIAutoExposureStatus& status = renderer.mExposure.GetStatus();
			Printf("NRI PT auto exposure trace: stage=readback frame=%llu current_frame=%u valid=%s source=%s meter_mode=%s weighted_samples=%u bins=%u..%u log_lum=%.3f..%.3f metered_log_lum=%.3f metered_lum=%.6f target=%.3f adapted=%.3f previous=%.3f fallback=%.3f target_ev=%.3f adapted_ev=%.3f flags=0x%x bin_count=%u percentiles=%.2f..%.2f reset_pending=%s reset_serial=%llu reset_reason=%s\n",
				(unsigned long long)status.debugFrameIndex,
				renderer.mFrameIndex,
				status.debugValid ? "yes" : "no",
				renderer.GetFrameTextureSlotName(renderer.mAutoExposureInputSourceSlot),
				GetNRIAutoExposureMeteringModeName(settings.meteringMode),
				status.sampleCount,
				status.lowBin,
				status.highBin,
				status.lowLogLuminance,
				status.highLogLuminance,
				status.meteredLogLuminance,
				std::exp2(status.meteredLogLuminance),
				status.targetExposure,
				status.adaptedExposure,
				AsFloat(words[12]),
				AsFloat(words[13]),
				std::log2(std::max(status.targetExposure, 1.0e-6f)),
				std::log2(std::max(status.adaptedExposure, 1.0e-6f)),
				words[14],
				words[15],
				settings.lowPercentile,
				settings.highPercentile,
				status.resetPending ? "yes" : "no",
				(unsigned long long)status.resetSerial,
				status.resetReason[0] != '\0' ? status.resetReason : "none");
		}
	}
	else
	{
		renderer.mExposure.ClearDebugReadback();
		if (ShouldEmitAutoExposureTemporalTraceLogs())
		{
			Printf("NRI PT auto exposure trace: stage=readback frame=%llu current_frame=%u valid=no reason=bad-magic magic=0x%x source=%s meter_mode=%s\n",
				(unsigned long long)renderer.mPendingAutoExposureStatsFrame,
				renderer.mFrameIndex,
				words[0],
				renderer.GetFrameTextureSlotName(renderer.mAutoExposureInputSourceSlot),
				GetNRIAutoExposureMeteringModeName(renderer.mExposure.GetSettings().meteringMode));
		}
	}
	renderer.mPendingAutoExposureStatsFrame = 0;
}

bool EnsureNRIRendererAutoExposureResources(NRIRenderer& renderer, const NRIAutoExposureSettings& settings)
{
	return NRIExposurePassAccess::EnsureResources(renderer, settings);
}

void DestroyNRIRendererAutoExposureResources(NRIRenderer& renderer)
{
	NRIExposurePassAccess::DestroyResources(renderer);
}

bool UpdateNRIRendererAutoExposureDescriptorSets(NRIRenderer& renderer, uint32_t sourceSlot)
{
	return NRIExposurePassAccess::UpdateDescriptorSets(renderer, sourceSlot);
}

bool DispatchNRIRendererAutoExposure(NRIRenderer& renderer, uint32_t sourceSlot)
{
	return NRIExposurePassAccess::Dispatch(renderer, sourceSlot);
}

void CopyNRIRendererAutoExposureStatsForReadback(NRIRenderer& renderer, uint64_t frameNumber)
{
	NRIExposurePassAccess::CopyStatsForReadback(renderer, frameNumber);
}

void ReadbackNRIRendererAutoExposureStats(NRIRenderer& renderer)
{
	NRIExposurePassAccess::ReadbackStats(renderer);
}

bool NRIExposureController::MatchesRenderSize(uint32_t renderWidth, uint32_t renderHeight) const
{
	return
		mStatus.renderWidth == std::max(renderWidth, 1u) &&
		mStatus.renderHeight == std::max(renderHeight, 1u);
}

void NRIExposureController::MarkResourcesAllocated(uint32_t renderWidth, uint32_t renderHeight, uint64_t memoryBytes)
{
	mStatus.resourcesAllocated = HasRequiredResources();
	mStatus.histogramAllocated = HasHistogramResources();
	mStatus.debugBufferAllocated = HasDebugBuffer();
	mStatus.debugReadbackAllocated = HasDebugReadbackBuffer();
	mStatus.renderWidth = std::max(renderWidth, 1u);
	mStatus.renderHeight = std::max(renderHeight, 1u);
	mStatus.memoryBytes = memoryBytes;
	mStatus.allocationSerial++;
}

void NRIExposureController::MarkResourcesDestroyed()
{
	ResetStatus();
}

void NRIExposureController::MarkDebugReadback(uint64_t frameIndex, const uint32_t* words, uint32_t wordCount)
{
	if (words == nullptr || wordCount < NRI_AUTO_EXPOSURE_DEBUG_WORD_COUNT || words[0] != NRI_AUTO_EXPOSURE_DEBUG_MAGIC)
	{
		ClearDebugReadback();
		return;
	}

	mStatus.debugValid = true;
	mStatus.debugFrameIndex = frameIndex;
	mStatus.sampleCount = words[2];
	mStatus.lowBin = words[5];
	mStatus.highBin = words[6];
	mStatus.lowLogLuminance = AsFloat(words[7]);
	mStatus.highLogLuminance = AsFloat(words[8]);
	mStatus.meteredLogLuminance = AsFloat(words[9]);
	mStatus.targetExposure = AsFloat(words[10]);
	mStatus.adaptedExposure = AsFloat(words[11]);
	if (!std::isfinite(mStatus.lowLogLuminance) ||
		!std::isfinite(mStatus.highLogLuminance) ||
		!std::isfinite(mStatus.meteredLogLuminance) ||
		!std::isfinite(mStatus.targetExposure) ||
		!std::isfinite(mStatus.adaptedExposure))
	{
		ClearDebugReadback();
	}
}

void NRIExposureController::ClearDebugReadback()
{
	mStatus.debugValid = false;
	mStatus.debugFrameIndex = 0;
	mStatus.sampleCount = 0;
	mStatus.lowBin = 0;
	mStatus.highBin = 0;
	mStatus.lowLogLuminance = 0.0f;
	mStatus.highLogLuminance = 0.0f;
	mStatus.meteredLogLuminance = 0.0f;
	mStatus.targetExposure = 1.0f;
	mStatus.adaptedExposure = 1.0f;
}

void NRIExposureController::RequestReset(const char* reason, uint64_t frameIndex)
{
	mStatus.resetPending = true;
	mStatus.resetSerial++;
	mStatus.resetRequestFrame = frameIndex;
	const char* safeReason = reason != nullptr && *reason != '\0' ? reason : "unspecified";
	std::strncpy(mStatus.resetReason, safeReason, sizeof(mStatus.resetReason) - 1u);
	mStatus.resetReason[sizeof(mStatus.resetReason) - 1u] = '\0';
}

bool NRIExposureController::ConsumeResetRequest(uint64_t frameIndex)
{
	if (!mStatus.resetPending)
	{
		return false;
	}

	mStatus.resetPending = false;
	mStatus.resetConsumedFrame = frameIndex;
	return true;
}

const NRITextureResource* NRIExposureController::GetExposureStateTexture(uint32_t index) const
{
	if (index >= 2 || mExposureState[index].texture == nullptr)
	{
		return nullptr;
	}

	return &mExposureState[index];
}

bool NRIExposureController::HasRequiredResources() const
{
	return HasExposureStateTextures() && HasHistogramResources() && HasDebugBuffer();
}

bool NRIExposureController::HasExposureStateTextures() const
{
	return
		mExposureState[0].texture != nullptr &&
		mExposureState[0].shaderView != nullptr &&
		mExposureState[0].storageView != nullptr &&
		mExposureState[1].texture != nullptr &&
		mExposureState[1].shaderView != nullptr &&
		mExposureState[1].storageView != nullptr;
}

bool NRIExposureController::HasHistogramResources() const
{
	return mHistogramBuffer.buffer != nullptr && mHistogramBuffer.shaderView != nullptr;
}

bool NRIExposureController::HasDebugBuffer() const
{
	return mDebugBuffer.buffer != nullptr && mDebugBuffer.shaderView != nullptr;
}

bool NRIExposureController::HasDebugReadbackBuffer() const
{
	return mDebugReadbackBuffer.buffer != nullptr;
}

void NRIExposureController::ResetStatus()
{
	mStatus.resourcesAllocated = false;
	mStatus.histogramAllocated = false;
	mStatus.debugBufferAllocated = false;
	mStatus.debugReadbackAllocated = false;
	mStatus.resetPending = false;
	mStatus.renderWidth = 0;
	mStatus.renderHeight = 0;
	mStatus.memoryBytes = 0;
	mStatus.resetRequestFrame = 0;
	mStatus.resetConsumedFrame = 0;
	mStatus.resetReason[0] = '\0';
	ClearDebugReadback();
}