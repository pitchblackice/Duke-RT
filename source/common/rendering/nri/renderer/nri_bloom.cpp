#include "nri_bloom.h"

#include "nri_pass_dispatch.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"

#include <iterator>

namespace
{
	uint32_t GetDispatchSize(uint32_t size)
	{
		return (size + 7u) / 8u;
	}

	NRIBloomConstants BuildBloomConstants(
		const NRIPassDispatchContext& context,
		const NRITextureResource& input,
		const NRITextureResource& output,
		const NRIBloomDispatchDesc& desc)
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
		if (constants.Cutoff > 0.0f)
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
}

bool DispatchBloom(NRIPassDispatchContext& context, const NRIBloomDispatchDesc& desc)
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

	const NRIBloomConstants constants = BuildBloomConstants(context, input, output, desc);
	context.mCommands.SetPipelineLayout(context.mBloomPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	context.mCommands.SetDescriptorSet(0, context.mBloomInputSet);
	context.mCommands.SetDescriptorSet(1, context.mBloomOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::BloomCopy));
	context.mCommands.Dispatch(GetDispatchSize(output.width), GetDispatchSize(output.height), 1);
	return true;
}
