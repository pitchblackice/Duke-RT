#include "nri_bloom.h"

#include "nri_pass_dispatch.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"
#include "printf.h"

#include <iterator>

namespace
{
	constexpr NRIRenderer::FrameTextureSlot kBloomPyramidSlots[] = {
		NRIRenderer::FrameTextureSlot::BloomPyramid0,
		NRIRenderer::FrameTextureSlot::BloomPyramid1,
		NRIRenderer::FrameTextureSlot::BloomPyramid2,
		NRIRenderer::FrameTextureSlot::BloomPyramid3,
		NRIRenderer::FrameTextureSlot::BloomPyramid4,
		NRIRenderer::FrameTextureSlot::BloomPyramid5,
		NRIRenderer::FrameTextureSlot::BloomPyramid6,
		NRIRenderer::FrameTextureSlot::BloomPyramid7
	};

	uint32_t GetDispatchSize(uint32_t size)
	{
		return (size + 7u) / 8u;
	}

	const char* GetBloomPipelineName(NRIRenderer::PipelineSlot slot)
	{
		switch (slot)
		{
		case NRIRenderer::PipelineSlot::BloomCopy: return "BloomCopy";
		case NRIRenderer::PipelineSlot::BloomDownsample: return "BloomDownsample";
		case NRIRenderer::PipelineSlot::BloomUpsample: return "BloomUpsample";
		case NRIRenderer::PipelineSlot::BloomComposite: return "BloomComposite";
		default: return "Unknown";
		}
	}

	const char* GetBloomSlotName(NRIRenderer::FrameTextureSlot slot)
	{
		switch (slot)
		{
		case NRIRenderer::FrameTextureSlot::TaaHistoryPing: return "TaaHistoryPing";
		case NRIRenderer::FrameTextureSlot::TaaHistoryPong: return "TaaHistoryPong";
		case NRIRenderer::FrameTextureSlot::VendorOutput: return "VendorOutput";
		case NRIRenderer::FrameTextureSlot::PostSharpenOutput: return "PostSharpenOutput";
		case NRIRenderer::FrameTextureSlot::PostBloomOutput: return "PostBloomOutput";
		case NRIRenderer::FrameTextureSlot::BloomPyramid0: return "BloomPyramid0";
		case NRIRenderer::FrameTextureSlot::BloomPyramid1: return "BloomPyramid1";
		case NRIRenderer::FrameTextureSlot::BloomPyramid2: return "BloomPyramid2";
		case NRIRenderer::FrameTextureSlot::BloomPyramid3: return "BloomPyramid3";
		case NRIRenderer::FrameTextureSlot::BloomPyramid4: return "BloomPyramid4";
		case NRIRenderer::FrameTextureSlot::BloomPyramid5: return "BloomPyramid5";
		case NRIRenderer::FrameTextureSlot::BloomPyramid6: return "BloomPyramid6";
		case NRIRenderer::FrameTextureSlot::BloomPyramid7: return "BloomPyramid7";
		case NRIRenderer::FrameTextureSlot::Count: return "Count";
		default: return "Other";
		}
	}

	bool ShouldTraceBloom()
	{
		return (int)nri_ptbloomdebug > 0 && (int)nri_pttraceframes > 0;
	}

	uint32_t ResolveBloomLevelCount(const NRIPassDispatchContext& context)
	{
		uint32_t levelCount = (uint32_t)std::max(1, std::min((int)nri_ptbloomlevels, (int)std::size(kBloomPyramidSlots)));
		while (levelCount > 1)
		{
			const NRITextureResource& texture = context.mTextures.Get(kBloomPyramidSlots[levelCount - 1u]);
			if (texture.width > 1 || texture.height > 1)
			{
				break;
			}
			--levelCount;
		}
		return levelCount;
	}

	void BarrierStorageTexture(NRIPassDispatchContext& context, const NRITextureResource& texture)
	{
		if (texture.texture == nullptr || context.mCommands.core == nullptr || context.mCommands.commandBuffer == nullptr)
		{
			return;
		}

		nri::TextureBarrierDesc barrier = {};
		barrier.texture = texture.texture;
		barrier.before = NRIComputeStorageState();
		barrier.after = NRIComputeStorageState();
		barrier.mipNum = 1;
		barrier.layerNum = texture.layerNum;
		barrier.planes = nri::PlaneBits::COLOR;

		nri::BarrierDesc barriers = {};
		barriers.textures = &barrier;
		barriers.textureNum = 1;
		context.mCommands.core->CmdBarrier(*context.mCommands.commandBuffer, barriers);
	}

	NRIBloomConstants BuildBloomConstants(
		const NRIPassDispatchContext& context,
		const NRITextureResource& input,
		const NRITextureResource& output,
		const NRIBloomDispatchDesc& desc,
		bool threshold)
	{
		NRIBloomConstants constants = {};
		constants.InputWidth = input.width;
		constants.InputHeight = input.height;
		constants.OutputWidth = output.width;
		constants.OutputHeight = output.height;
		constants.Intensity = (float)nri_ptbloomintensity;
		constants.Sigma = (float)nri_ptbloomsigma;
		constants.Cutoff = (float)nri_ptbloomcutoff;
		constants.Fuzziness = (float)nri_ptbloomfuzziness;
		constants.FrameIndex = context.mFrame.frameIndex;
		constants.LevelIndex = desc.levelIndex;
		constants.LevelCount = desc.levelCount;
		constants.InputTexelSizeX = input.width > 0 ? 1.0f / (float)input.width : 0.0f;
		constants.InputTexelSizeY = input.height > 0 ? 1.0f / (float)input.height : 0.0f;
		if (threshold && constants.Cutoff > 0.0f)
		{
			constants.Flags |= NRI_BLOOM_FLAG_THRESHOLD;
		}
		if (nri_ptbloomenergyconstrained)
		{
			constants.Flags |= NRI_BLOOM_FLAG_ENERGY_CONSTRAINED;
		}
		if ((int)nri_ptbloomdebug > 0)
		{
			constants.Flags |= NRI_BLOOM_FLAG_DEBUG;
		}
		return constants;
	}

	bool DispatchBloomPass(
		NRIPassDispatchContext& context,
		NRIRenderer::PipelineSlot pipelineSlot,
		const NRIBloomDispatchDesc& desc,
		bool threshold,
		uint32_t descriptorIndex)
	{
		if (descriptorIndex >= NRIRenderer::BloomDescriptorSetCount)
		{
			if ((int)nri_ptbloomdebug > 0)
			{
				Printf(TEXTCOLOR_ORANGE "NRI bloom dispatch skipped: pipeline=%s descriptor=%u reason=descriptor-ring-exhausted\n",
					GetBloomPipelineName(pipelineSlot),
					descriptorIndex);
			}
			return false;
		}
		if (desc.inputSlot == NRIRenderer::FrameTextureSlot::Count || desc.outputSlot == NRIRenderer::FrameTextureSlot::Count)
		{
			if ((int)nri_ptbloomdebug > 0)
			{
				Printf(TEXTCOLOR_ORANGE "NRI bloom dispatch skipped: pipeline=%s input=%s output=%s reason=invalid-slot\n",
					GetBloomPipelineName(pipelineSlot),
					GetBloomSlotName(desc.inputSlot),
					GetBloomSlotName(desc.outputSlot));
			}
			return false;
		}

		NRITextureResource& input = context.mTextures.Get(desc.inputSlot);
		NRITextureResource& secondaryInput = context.mTextures.Get(desc.secondaryInputSlot != NRIRenderer::FrameTextureSlot::Count ? desc.secondaryInputSlot : desc.inputSlot);
		NRITextureResource& output = context.mTextures.Get(desc.outputSlot);
		if (input.texture == nullptr || input.shaderView == nullptr || secondaryInput.shaderView == nullptr || output.texture == nullptr || output.storageView == nullptr)
		{
			if ((int)nri_ptbloomdebug > 0)
			{
				Printf(TEXTCOLOR_ORANGE "NRI bloom dispatch skipped: frame=%u debug=%d pipeline=%s input=%s tex=%p srv=%p secondary=%s srv=%p output=%s tex=%p uav=%p reason=missing-resource\n",
					context.mFrame.frameIndex,
					(int)nri_ptbloomdebug,
					GetBloomPipelineName(pipelineSlot),
					GetBloomSlotName(desc.inputSlot),
					input.texture,
					input.shaderView,
					GetBloomSlotName(desc.secondaryInputSlot != NRIRenderer::FrameTextureSlot::Count ? desc.secondaryInputSlot : desc.inputSlot),
					secondaryInput.shaderView,
					GetBloomSlotName(desc.outputSlot),
					output.texture,
					output.storageView);
			}
			return false;
		}

		context.mResources.TransitionTexture(input, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(secondaryInput, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(output, NRIComputeStorageState());

		const nri::Descriptor* inputs[2] = {
			input.shaderView,
			secondaryInput.shaderView
		};
		nri::DescriptorSet* inputSet = context.mBloomInputSets[descriptorIndex];
		nri::DescriptorSet* outputSet = context.mBloomOutputSets[descriptorIndex];
		nri::UpdateDescriptorRangeDesc inputUpdate = {};
		inputUpdate.descriptorSet = inputSet;
		inputUpdate.rangeIndex = 0;
		inputUpdate.descriptors = inputs;
		inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
		context.mCommands.UpdateDescriptorRanges(&inputUpdate, 1);

		const nri::Descriptor* outputs[1] = { output.storageView };
		nri::UpdateDescriptorRangeDesc outputUpdate = {};
		outputUpdate.descriptorSet = outputSet;
		outputUpdate.rangeIndex = 0;
		outputUpdate.descriptors = outputs;
		outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
		context.mCommands.UpdateDescriptorRanges(&outputUpdate, 1);

		const NRIBloomConstants constants = BuildBloomConstants(context, input, output, desc, threshold);
		nri::Pipeline* pipeline = context.mPipelines.Get(pipelineSlot);
		if (ShouldTraceBloom())
		{
			Printf("NRI bloom dispatch: frame=%u debug=%d pipeline=%s pipe=%p descriptor=%u level=%u/%u input=%s %ux%u tex=%p srv=%p secondary=%s srv=%p output=%s %ux%u tex=%p uav=%p threshold=%d flags=0x%x cutoff=%.3f fuzz=%.3f intensity=%.3f sigma=%.3f groups=%ux%u\n",
				context.mFrame.frameIndex,
				(int)nri_ptbloomdebug,
				GetBloomPipelineName(pipelineSlot),
				pipeline,
				descriptorIndex,
				desc.levelIndex,
				desc.levelCount,
				GetBloomSlotName(desc.inputSlot),
				input.width,
				input.height,
				input.texture,
				input.shaderView,
				GetBloomSlotName(desc.secondaryInputSlot != NRIRenderer::FrameTextureSlot::Count ? desc.secondaryInputSlot : desc.inputSlot),
				secondaryInput.shaderView,
				GetBloomSlotName(desc.outputSlot),
				output.width,
				output.height,
				output.texture,
				output.storageView,
				threshold ? 1 : 0,
				constants.Flags,
				constants.Cutoff,
				constants.Fuzziness,
				constants.Intensity,
				constants.Sigma,
				GetDispatchSize(output.width),
				GetDispatchSize(output.height));
		}
		context.mCommands.SetPipelineLayout(context.mBloomPipelineLayout);
		context.mCommands.SetRootConstants(&constants, sizeof(constants));
		context.mCommands.SetDescriptorSet(0, inputSet);
		context.mCommands.SetDescriptorSet(1, outputSet);
		context.mCommands.SetPipeline(pipeline);
		context.mCommands.Dispatch(GetDispatchSize(output.width), GetDispatchSize(output.height), 1);
		BarrierStorageTexture(context, output);
		return true;
	}
}

bool DispatchBloom(NRIPassDispatchContext& context, const NRIBloomDispatchDesc& desc)
{
	if (desc.mode == NRIBloomDispatchDesc::Mode::Copy)
	{
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, desc, false, 0);
	}

	const int debugMode = (int)nri_ptbloomdebug;
	if (debugMode == 1)
	{
		NRIBloomDispatchDesc debugDesc = desc;
		debugDesc.secondaryInputSlot = desc.inputSlot;
		debugDesc.levelIndex = 0;
		debugDesc.levelCount = 1;
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, debugDesc, false, 0);
	}
	if (debugMode == 2)
	{
		NRIBloomDispatchDesc downsampleDesc = desc;
		downsampleDesc.secondaryInputSlot = desc.inputSlot;
		downsampleDesc.outputSlot = kBloomPyramidSlots[0];
		downsampleDesc.levelIndex = 0;
		downsampleDesc.levelCount = 1;
		if (!DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomDownsample, downsampleDesc, false, 0))
		{
			return false;
		}

		NRIBloomDispatchDesc copyDesc = desc;
		copyDesc.inputSlot = kBloomPyramidSlots[0];
		copyDesc.secondaryInputSlot = kBloomPyramidSlots[0];
		copyDesc.levelIndex = 0;
		copyDesc.levelCount = 1;
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, copyDesc, false, 1);
	}

	const uint32_t levelCount = ResolveBloomLevelCount(context);
	uint32_t descriptorIndex = 0;
	NRIRenderer::FrameTextureSlot previousSlot = desc.inputSlot;
	for (uint32_t level = 0; level < levelCount; ++level)
	{
		NRIBloomDispatchDesc downsampleDesc = desc;
		downsampleDesc.inputSlot = previousSlot;
		downsampleDesc.secondaryInputSlot = previousSlot;
		downsampleDesc.outputSlot = kBloomPyramidSlots[level];
		downsampleDesc.levelIndex = level;
		downsampleDesc.levelCount = levelCount;
		if (!DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomDownsample, downsampleDesc, level == 0, descriptorIndex++))
		{
			return false;
		}
		previousSlot = kBloomPyramidSlots[level];
	}

	for (uint32_t level = levelCount - 1u; level > 0; --level)
	{
		NRIBloomDispatchDesc upsampleDesc = desc;
		upsampleDesc.inputSlot = kBloomPyramidSlots[level];
		upsampleDesc.secondaryInputSlot = kBloomPyramidSlots[level];
		upsampleDesc.outputSlot = kBloomPyramidSlots[level - 1u];
		upsampleDesc.levelIndex = level - 1u;
		upsampleDesc.levelCount = levelCount;
		if (!DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomUpsample, upsampleDesc, false, descriptorIndex++))
		{
			return false;
		}
	}

	if (debugMode == 3)
	{
		NRIBloomDispatchDesc debugDesc = desc;
		debugDesc.inputSlot = kBloomPyramidSlots[0];
		debugDesc.secondaryInputSlot = kBloomPyramidSlots[0];
		debugDesc.levelIndex = 0;
		debugDesc.levelCount = levelCount;
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, debugDesc, false, descriptorIndex++);
	}

	NRIBloomDispatchDesc compositeDesc = desc;
	compositeDesc.inputSlot = desc.inputSlot;
	compositeDesc.secondaryInputSlot = kBloomPyramidSlots[0];
	compositeDesc.levelIndex = 0;
	compositeDesc.levelCount = levelCount;
	return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomComposite, compositeDesc, false, descriptorIndex++);
}
