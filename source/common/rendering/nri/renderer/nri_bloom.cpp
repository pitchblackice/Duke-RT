#include "nri_bloom.h"

#include "nri_pass_dispatch.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"

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
		bool threshold)
	{
		if (desc.inputSlot == NRIRenderer::FrameTextureSlot::Count || desc.outputSlot == NRIRenderer::FrameTextureSlot::Count)
		{
			return false;
		}

		NRITextureResource& input = context.mTextures.Get(desc.inputSlot);
		NRITextureResource& secondaryInput = context.mTextures.Get(desc.secondaryInputSlot != NRIRenderer::FrameTextureSlot::Count ? desc.secondaryInputSlot : desc.inputSlot);
		NRITextureResource& output = context.mTextures.Get(desc.outputSlot);
		if (input.texture == nullptr || input.shaderView == nullptr || secondaryInput.shaderView == nullptr || output.texture == nullptr || output.storageView == nullptr)
		{
			return false;
		}

		context.mResources.TransitionTexture(input, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(secondaryInput, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(output, NRIComputeStorageState());

		const nri::Descriptor* inputs[2] = {
			input.shaderView,
			secondaryInput.shaderView
		};
		nri::UpdateDescriptorRangeDesc inputUpdate = {};
		inputUpdate.descriptorSet = context.mBloomInputSet;
		inputUpdate.rangeIndex = 0;
		inputUpdate.descriptors = inputs;
		inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
		context.mCommands.UpdateDescriptorRanges(&inputUpdate, 1);

		const nri::Descriptor* outputs[1] = { output.storageView };
		nri::UpdateDescriptorRangeDesc outputUpdate = {};
		outputUpdate.descriptorSet = context.mBloomOutputSet;
		outputUpdate.rangeIndex = 0;
		outputUpdate.descriptors = outputs;
		outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
		context.mCommands.UpdateDescriptorRanges(&outputUpdate, 1);

		const NRIBloomConstants constants = BuildBloomConstants(context, input, output, desc, threshold);
		context.mCommands.SetPipelineLayout(context.mBloomPipelineLayout);
		context.mCommands.SetRootConstants(&constants, sizeof(constants));
		context.mCommands.SetDescriptorSet(0, context.mBloomInputSet);
		context.mCommands.SetDescriptorSet(1, context.mBloomOutputSet);
		context.mCommands.SetPipeline(context.mPipelines.Get(pipelineSlot));
		context.mCommands.Dispatch(GetDispatchSize(output.width), GetDispatchSize(output.height), 1);
		BarrierStorageTexture(context, output);
		return true;
	}
}

bool DispatchBloom(NRIPassDispatchContext& context, const NRIBloomDispatchDesc& desc)
{
	if (desc.mode == NRIBloomDispatchDesc::Mode::Copy)
	{
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, desc, false);
	}

	const int debugMode = (int)nri_ptbloomdebug;
	if (debugMode == 1)
	{
		NRIBloomDispatchDesc debugDesc = desc;
		debugDesc.secondaryInputSlot = desc.inputSlot;
		debugDesc.levelIndex = 0;
		debugDesc.levelCount = 1;
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, debugDesc, false);
	}

	const uint32_t levelCount = ResolveBloomLevelCount(context);
	NRIRenderer::FrameTextureSlot previousSlot = desc.inputSlot;
	for (uint32_t level = 0; level < levelCount; ++level)
	{
		NRIBloomDispatchDesc downsampleDesc = desc;
		downsampleDesc.inputSlot = previousSlot;
		downsampleDesc.secondaryInputSlot = previousSlot;
		downsampleDesc.outputSlot = kBloomPyramidSlots[level];
		downsampleDesc.levelIndex = level;
		downsampleDesc.levelCount = levelCount;
		if (!DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomDownsample, downsampleDesc, level == 0))
		{
			return false;
		}
		previousSlot = kBloomPyramidSlots[level];
	}

	if (debugMode == 2)
	{
		NRIBloomDispatchDesc debugDesc = desc;
		debugDesc.inputSlot = kBloomPyramidSlots[0];
		debugDesc.secondaryInputSlot = kBloomPyramidSlots[0];
		debugDesc.levelIndex = 0;
		debugDesc.levelCount = levelCount;
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, debugDesc, false);
	}

	for (uint32_t level = levelCount - 1u; level > 0; --level)
	{
		NRIBloomDispatchDesc upsampleDesc = desc;
		upsampleDesc.inputSlot = kBloomPyramidSlots[level];
		upsampleDesc.secondaryInputSlot = kBloomPyramidSlots[level];
		upsampleDesc.outputSlot = kBloomPyramidSlots[level - 1u];
		upsampleDesc.levelIndex = level - 1u;
		upsampleDesc.levelCount = levelCount;
		if (!DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomUpsample, upsampleDesc, false))
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
		return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomCopy, debugDesc, false);
	}

	NRIBloomDispatchDesc compositeDesc = desc;
	compositeDesc.inputSlot = desc.inputSlot;
	compositeDesc.secondaryInputSlot = kBloomPyramidSlots[0];
	compositeDesc.levelIndex = 0;
	compositeDesc.levelCount = levelCount;
	return DispatchBloomPass(context, NRIRenderer::PipelineSlot::BloomComposite, compositeDesc, false);
}
