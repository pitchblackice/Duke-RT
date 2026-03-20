#include "nri_renderstate.h"

#include "../system/nri_hwbuffer.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "hw_material.h"
#include "printf.h"
#include "textures.h"

#include <cstring>

namespace
{
	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIShaderStages()
	{
		return (nri::StageBits)((uint32_t)nri::StageBits::VERTEX_SHADER | (uint32_t)nri::StageBits::FRAGMENT_SHADER);
	}

	static nri::BlendFactor ToBlendFactor(int alpha)
	{
		switch (alpha)
		{
		default:
		case STYLEALPHA_Zero: return nri::BlendFactor::ZERO;
		case STYLEALPHA_One: return nri::BlendFactor::ONE;
		case STYLEALPHA_Src: return nri::BlendFactor::SRC_ALPHA;
		case STYLEALPHA_InvSrc: return nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
		case STYLEALPHA_SrcCol: return nri::BlendFactor::SRC_COLOR;
		case STYLEALPHA_InvSrcCol: return nri::BlendFactor::ONE_MINUS_SRC_COLOR;
		case STYLEALPHA_DstCol: return nri::BlendFactor::DST_COLOR;
		case STYLEALPHA_InvDstCol: return nri::BlendFactor::ONE_MINUS_DST_COLOR;
		case STYLEALPHA_Dst: return nri::BlendFactor::DST_ALPHA;
		case STYLEALPHA_InvDst: return nri::BlendFactor::ONE_MINUS_DST_ALPHA;
		}
	}

	static nri::BlendOp ToBlendOp(int op)
	{
		switch (op)
		{
		default:
		case STYLEOP_Add: return nri::BlendOp::ADD;
		case STYLEOP_Sub: return nri::BlendOp::SUBTRACT;
		case STYLEOP_RevSub: return nri::BlendOp::REVERSE_SUBTRACT;
		}
	}

	static nri::Topology ToTopology(int dt)
	{
		switch (dt)
		{
		case DT_Points: return nri::Topology::POINT_LIST;
		case DT_Lines: return nri::Topology::LINE_LIST;
		case DT_TriangleStrip: return nri::Topology::TRIANGLE_STRIP;
		case DT_Triangles:
		default: return nri::Topology::TRIANGLE_LIST;
		}
	}

	static nri::ColorWriteBits ToColorMask(int mask)
	{
		uint32_t out = 0;
		if (mask & 0x1) out |= (uint32_t)nri::ColorWriteBits::R;
		if (mask & 0x2) out |= (uint32_t)nri::ColorWriteBits::G;
		if (mask & 0x4) out |= (uint32_t)nri::ColorWriteBits::B;
		if (mask & 0x8) out |= (uint32_t)nri::ColorWriteBits::A;
		return (nri::ColorWriteBits)out;
	}

	static nri::Format ToVertexFormat(int format)
	{
		switch (format)
		{
		case VFmt_Float4: return nri::Format::RGBA32_SFLOAT;
		case VFmt_Float3: return nri::Format::RGB32_SFLOAT;
		case VFmt_Float2: return nri::Format::RG32_SFLOAT;
		case VFmt_Float: return nri::Format::R32_SFLOAT;
		case VFmt_Byte4: return nri::Format::RGBA8_UNORM;
		case VFmt_Byte4_UInt: return nri::Format::RGBA8_UINT;
		case VFmt_Packed_A2R10G10B10: return nri::Format::R10_G10_B10_A2_UNORM;
		default: return nri::Format::UNKNOWN;
		}
	}

	static const char* ToSemanticName(int location)
	{
		switch (location)
		{
		default:
		case VATTR_VERTEX: return "POSITION";
		case VATTR_TEXCOORD: return "TEXCOORD";
		case VATTR_COLOR: return "COLOR";
		case VATTR_LIGHTMAP: return "TEXCOORD";
		}
	}

	static uint32_t ToSemanticIndex(int location)
	{
		return location == VATTR_LIGHTMAP ? 1u : 0u;
	}
}

NRIRenderState::NRIRenderState(NRIRenderDevice* fb)
	: mFrameBuffer(fb)
{
	Reset();
}

void NRIRenderState::ClearScreen()
{
	mClearTargets |= CT_Color;
	mNeedsClear = true;
}

void NRIRenderState::Draw(int dt, int index, int count, bool)
{
	Apply(dt, false, index, count);
}

void NRIRenderState::DrawIndexed(int dt, int index, int count, bool)
{
	Apply(dt, true, index, count);
}

bool NRIRenderState::SetDepthClamp(bool on)
{
	return on;
}

void NRIRenderState::SetDepthMask(bool)
{
}

void NRIRenderState::SetDepthFunc(int)
{
}

void NRIRenderState::SetDepthRange(float, float)
{
}

void NRIRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	mColorMask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}

void NRIRenderState::SetStencil(int, int, int)
{
}

void NRIRenderState::SetCulling(int)
{
}

void NRIRenderState::EnableClipDistance(int, bool)
{
}

void NRIRenderState::Clear(int targets)
{
	if (targets & CT_Color)
	{
		EndRendering();
		mNeedsClear = true;
		mClearTargets |= CT_Color;
	}
}

void NRIRenderState::EnableStencil(bool)
{
}

void NRIRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
}

void NRIRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
}

void NRIRenderState::EnableDepthTest(bool)
{
}

void NRIRenderState::EnableMultisampling(bool)
{
}

void NRIRenderState::EnableLineSmooth(bool)
{
}

void NRIRenderState::EnableDrawBuffers(int, bool)
{
}

void NRIRenderState::BeginFrame()
{
	mLastVertexBuffer = nullptr;
	mLastIndexBuffer = nullptr;
	mLastVertexStream = {};
	mLastIndexStream = {};
	mRendering = false;
	mNeedsClear = true;
	mClearTargets = CT_Color;
}

void NRIRenderState::EndFrame()
{
	EndRendering();
}

void NRIRenderState::Apply(int dt, bool indexed, int firstIndex, int count)
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return;
	}

	auto* vertexBuffer = static_cast<NRIHardwareVertexBuffer*>(mVertexBuffer);
	if (vertexBuffer == nullptr || vertexBuffer->Data() == nullptr || vertexBuffer->GetStride() == 0)
	{
		return;
	}

	if (indexed && mIndexBuffer == nullptr)
	{
		return;
	}

	nri::Pipeline* pipeline = GetPipeline(dt);
	if (pipeline == nullptr)
	{
		return;
	}

	StreamedBuffer vertexStream = StreamVertices(vertexBuffer);
	if (vertexStream.buffer == nullptr)
	{
		return;
	}

	BeginRenderingIfNeeded();

	NRIShaderConstants constants = {};
	FillShaderConstants(constants);

	nri::Viewport viewport = {};
	viewport.x = (float)mViewportX;
	viewport.y = (float)mViewportY;
	viewport.width = (float)(mViewportWidth > 0 ? mViewportWidth : (int)mFrameBuffer->mActiveTarget->width);
	viewport.height = (float)(mViewportHeight > 0 ? mViewportHeight : (int)mFrameBuffer->mActiveTarget->height);
	viewport.depthMin = 0.0f;
	viewport.depthMax = 1.0f;

	nri::Rect scissor = {};
	scissor.x = (int16_t)(mScissorX >= 0 ? mScissorX : 0);
	scissor.y = (int16_t)(mScissorY >= 0 ? mScissorY : 0);
	scissor.width = (uint32_t)(mScissorWidth >= 0 ? mScissorWidth : (mViewportWidth > 0 ? mViewportWidth : (int)mFrameBuffer->mActiveTarget->width));
	scissor.height = (uint32_t)(mScissorHeight >= 0 ? mScissorHeight : (mViewportHeight > 0 ? mViewportHeight : (int)mFrameBuffer->mActiveTarget->height));

	nri::VertexBufferDesc vertexDesc = {};
	vertexDesc.buffer = vertexStream.buffer;
	vertexDesc.offset = vertexStream.offset + (uint64_t)mVertexOffsets[0];
	vertexDesc.stride = (uint32_t)vertexStream.stride;

	nri::DescriptorSet* textureSet = mFrameBuffer->mWhiteTextureSet;
	if (mTextureEnabled && mMaterial.mMaterial != nullptr)
	{
		static bool loggedTextureBinding = false;
		MaterialLayerInfo* layer = nullptr;
		auto* hwTexture = static_cast<NRIHardwareTexture*>(mMaterial.mMaterial->GetLayer(0, mMaterial.mTranslation, &layer));
		if (hwTexture != nullptr && layer != nullptr && layer->layerTexture != nullptr)
		{
			hwTexture->EnsureTexture(layer->layerTexture, mMaterial.mTranslation, layer->scaleFlags);
			textureSet = hwTexture->GetResource().textureSet;
			if (!loggedTextureBinding)
			{
				const auto& resource = hwTexture->GetResource();
				Printf("NRI 2D texture bind: layer_tex=%p size=%ux%u format=%u shader_view=%p texture_set=%p textured_flag=%s\n",
					layer->layerTexture,
					resource.width,
					resource.height,
					(unsigned int)resource.format,
					resource.shaderView,
					resource.textureSet,
					(constants.Flags & NRI2D_Textured) != 0 ? "yes" : "no");
				loggedTextureBinding = true;
			}
		}
	}

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::GRAPHICS, *mFrameBuffer->mPipelineLayout);
	mFrameBuffer->mCore.CmdSetViewports(*mFrameBuffer->mCommandBuffer, &viewport, 1);
	mFrameBuffer->mCore.CmdSetScissors(*mFrameBuffer->mCommandBuffer, &scissor, 1);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mFrameBuffer->GetSamplerSet(GetSamplerMode()), nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, textureSet, nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *pipeline);
	mFrameBuffer->mCore.CmdSetVertexBuffers(*mFrameBuffer->mCommandBuffer, 0, &vertexDesc, 1);

	if (indexed)
	{
		auto* indexBuffer = static_cast<NRIHardwareIndexBuffer*>(mIndexBuffer);
		StreamedBuffer indexStream = StreamIndices(indexBuffer, 0, (uint32_t)indexBuffer->Size() / 4u);
		if (indexStream.buffer == nullptr)
		{
			return;
		}

		mFrameBuffer->mCore.CmdSetIndexBuffer(*mFrameBuffer->mCommandBuffer, *indexStream.buffer, indexStream.offset, nri::IndexType::UINT32);
		mFrameBuffer->mCore.CmdDrawIndexed(*mFrameBuffer->mCommandBuffer, { (uint32_t)count, 1, (uint32_t)firstIndex, 0, 0 });
	}
	else
	{
		mFrameBuffer->mCore.CmdDraw(*mFrameBuffer->mCommandBuffer, { (uint32_t)count, 1, (uint32_t)firstIndex, 0 });
	}
}

NRIRenderState::StreamedBuffer NRIRenderState::StreamVertices(NRIHardwareVertexBuffer* vertexBuffer)
{
	if (vertexBuffer == nullptr || vertexBuffer->Data() == nullptr || vertexBuffer->Size() == 0)
	{
		return {};
	}

	if (mLastVertexBuffer == vertexBuffer && mLastVertexStream.generation == vertexBuffer->Generation())
	{
		return mLastVertexStream;
	}

	nri::DataSize dataChunk = { vertexBuffer->Data(), vertexBuffer->Size() };
	nri::StreamBufferDataDesc streamDesc = {};
	streamDesc.dataChunks = &dataChunk;
	streamDesc.dataChunkNum = 1;
	streamDesc.placementAlignment = (uint32_t)std::max<size_t>(vertexBuffer->GetStride(), 16);

	nri::BufferOffset bufferOffset = mFrameBuffer->mStreamer.StreamBufferData(*mFrameBuffer->mStreamerInstance, streamDesc);

	mLastVertexBuffer = vertexBuffer;
	mLastVertexStream.buffer = bufferOffset.buffer;
	mLastVertexStream.offset = bufferOffset.offset;
	mLastVertexStream.stride = (uint32_t)vertexBuffer->GetStride();
	mLastVertexStream.generation = vertexBuffer->Generation();
	return mLastVertexStream;
}

NRIRenderState::StreamedBuffer NRIRenderState::StreamIndices(NRIHardwareIndexBuffer* indexBuffer, uint32_t, uint32_t)
{
	if (indexBuffer == nullptr || indexBuffer->Data() == nullptr || indexBuffer->Size() == 0)
	{
		return {};
	}

	if (mLastIndexBuffer == indexBuffer && mLastIndexStream.generation == indexBuffer->Generation())
	{
		return mLastIndexStream;
	}

	nri::DataSize dataChunk = { indexBuffer->Data(), indexBuffer->Size() };
	nri::StreamBufferDataDesc streamDesc = {};
	streamDesc.dataChunks = &dataChunk;
	streamDesc.dataChunkNum = 1;
	streamDesc.placementAlignment = 16;

	nri::BufferOffset bufferOffset = mFrameBuffer->mStreamer.StreamBufferData(*mFrameBuffer->mStreamerInstance, streamDesc);

	mLastIndexBuffer = indexBuffer;
	mLastIndexStream.buffer = bufferOffset.buffer;
	mLastIndexStream.offset = bufferOffset.offset;
	mLastIndexStream.stride = 4;
	mLastIndexStream.generation = indexBuffer->Generation();
	return mLastIndexStream;
}

nri::Pipeline* NRIRenderState::GetPipeline(int dt)
{
	auto* vertexBuffer = static_cast<NRIHardwareVertexBuffer*>(mVertexBuffer);
	if (vertexBuffer == nullptr || mFrameBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return nullptr;
	}

	PipelineKey key = {};
	key.blendOp = mRenderStyle.BlendOp;
	key.srcBlend = mRenderStyle.SrcAlpha;
	key.dstBlend = mRenderStyle.DestAlpha;
	key.topology = (uint32_t)ToTopology(dt);
	key.format = (uint32_t)mFrameBuffer->mActiveTarget->format;
	key.colorMask = (uint32_t)mColorMask;
	key.vertexLayout = vertexBuffer->GetLayoutHash();

	auto it = mPipelines.find(key);
	if (it != mPipelines.end())
	{
		return it->second;
	}

	nri::VertexAttributeDesc attributes[VATTR_MAX] = {};
	for (int i = 0; i < vertexBuffer->GetAttributeCount(); ++i)
	{
		const FVertexBufferAttribute& attr = vertexBuffer->GetAttributes()[i];
		attributes[i].d3d.semanticName = ToSemanticName(attr.location);
		attributes[i].d3d.semanticIndex = ToSemanticIndex(attr.location);
		attributes[i].vk.location = (uint32_t)i;
		attributes[i].offset = attr.offset;
		attributes[i].format = ToVertexFormat(attr.format);
		attributes[i].streamIndex = 0;
	}

	nri::VertexStreamDesc streamDesc = {};
	streamDesc.bindingSlot = 0;
	streamDesc.stepRate = nri::VertexStreamStepRate::PER_VERTEX;

	nri::VertexInputDesc vertexInput = {};
	vertexInput.attributes = attributes;
	vertexInput.attributeNum = (uint8_t)vertexBuffer->GetAttributeCount();
	vertexInput.streams = &streamDesc;
	vertexInput.streamNum = 1;

	size_t vsSize = 0;
	size_t psSize = 0;
	const void* vsBytecode = mFrameBuffer->GetVertexShaderBytecode(vsSize);
	const void* psBytecode = mFrameBuffer->GetPixelShaderBytecode(psSize);
	if (vsBytecode == nullptr || psBytecode == nullptr)
	{
		return nullptr;
	}

	nri::ShaderDesc shaders[2] = {};
	shaders[0].stage = nri::StageBits::VERTEX_SHADER;
	shaders[0].bytecode = vsBytecode;
	shaders[0].size = vsSize;
	shaders[0].entryPointName = "main";
	shaders[1].stage = nri::StageBits::FRAGMENT_SHADER;
	shaders[1].bytecode = psBytecode;
	shaders[1].size = psSize;
	shaders[1].entryPointName = "main";

	nri::InputAssemblyDesc inputAssembly = {};
	inputAssembly.topology = ToTopology(dt);
	inputAssembly.primitiveRestart = nri::PrimitiveRestart::DISABLED;

	nri::RasterizationDesc rasterization = {};
	rasterization.fillMode = nri::FillMode::SOLID;
	rasterization.cullMode = nri::CullMode::NONE;

	nri::BlendDesc blend = {};
	blend.srcFactor = ToBlendFactor(mRenderStyle.SrcAlpha);
	blend.dstFactor = ToBlendFactor(mRenderStyle.DestAlpha);
	blend.op = ToBlendOp(mRenderStyle.BlendOp);

	nri::ColorAttachmentDesc colorAttachment = {};
	colorAttachment.format = mFrameBuffer->mActiveTarget->format;
	colorAttachment.colorBlend = blend;
	colorAttachment.alphaBlend = blend;
	colorAttachment.colorWriteMask = ToColorMask(mColorMask);
	colorAttachment.blendEnabled = !(mRenderStyle.SrcAlpha == STYLEALPHA_One && mRenderStyle.DestAlpha == STYLEALPHA_Zero && mRenderStyle.BlendOp == STYLEOP_Add);

	nri::OutputMergerDesc outputMerger = {};
	outputMerger.colors = &colorAttachment;
	outputMerger.colorNum = 1;
	outputMerger.depth.compareOp = nri::CompareOp::NONE;
	outputMerger.depth.write = false;
	outputMerger.depthStencilFormat = nri::Format::UNKNOWN;

	nri::GraphicsPipelineDesc pipelineDesc = {};
	pipelineDesc.pipelineLayout = mFrameBuffer->mPipelineLayout;
	pipelineDesc.vertexInput = &vertexInput;
	pipelineDesc.inputAssembly = inputAssembly;
	pipelineDesc.rasterization = rasterization;
	pipelineDesc.outputMerger = outputMerger;
	pipelineDesc.shaders = shaders;
	pipelineDesc.shaderNum = 2;

	nri::Pipeline* pipeline = nullptr;
	if (mFrameBuffer->mCore.CreateGraphicsPipeline(*mFrameBuffer->mDevice, pipelineDesc, pipeline) != nri::Result::SUCCESS)
	{
		return nullptr;
	}

	mPipelines.emplace(key, pipeline);
	return pipeline;
}

void NRIRenderState::BeginRenderingIfNeeded()
{
	if (mRendering || mFrameBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return;
	}

	NRITextureResource& target = *mFrameBuffer->mActiveTarget;
	if (target.colorAttachmentView == nullptr)
	{
		return;
	}

	mFrameBuffer->PrepareTargetForRendering(target, mNeedsClear);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = target.colorAttachmentView;
	colorAttachment.loadOp = mNeedsClear ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = mNeedsClear ? mFrameBuffer->mSceneClearColor[0] : 0.0f;
	colorAttachment.clearValue.color.f.y = mNeedsClear ? mFrameBuffer->mSceneClearColor[1] : 0.0f;
	colorAttachment.clearValue.color.f.z = mNeedsClear ? mFrameBuffer->mSceneClearColor[2] : 0.0f;
	colorAttachment.clearValue.color.f.w = mNeedsClear ? mFrameBuffer->mSceneClearColor[3] : 1.0f;

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mFrameBuffer->mCore.CmdBeginRendering(*mFrameBuffer->mCommandBuffer, renderingDesc);
	mRendering = true;
	mNeedsClear = false;
}

void NRIRenderState::EndRendering()
{
	if (!mRendering || mFrameBuffer == nullptr)
	{
		return;
	}

	mFrameBuffer->mCore.CmdEndRendering(*mFrameBuffer->mCommandBuffer);
	mRendering = false;
}

void NRIRenderState::FillShaderConstants(NRIShaderConstants& constants) const
{
	const int viewportWidth = mViewportWidth > 0 ? mViewportWidth : (mFrameBuffer->mActiveTarget != nullptr ? (int)mFrameBuffer->mActiveTarget->width : 1);
	const int viewportHeight = mViewportHeight > 0 ? mViewportHeight : (mFrameBuffer->mActiveTarget != nullptr ? (int)mFrameBuffer->mActiveTarget->height : 1);

	constants.InvViewportSize[0] = viewportWidth > 0 ? 1.0f / viewportWidth : 1.0f;
	constants.InvViewportSize[1] = viewportHeight > 0 ? 1.0f / viewportHeight : 1.0f;
	constants.ScreenFade = mStreamData.uDynLightColor.W;
	constants.Flags = 0;

	if (mTextureEnabled && mMaterial.mMaterial != nullptr)
	{
		constants.Flags |= NRI2D_Textured;
	}

	if (mTextureMode == TM_ALPHATEXTURE || (mRenderStyle.Flags & STYLEF_RedIsAlpha))
	{
		constants.Flags |= NRI2D_AlphaFromRed;
	}

	if (mTextureMode == TM_INVERSE || mTextureMode == TM_INVERTOPAQUE || (mRenderStyle.Flags & STYLEF_InvertSource))
	{
		constants.Flags |= NRI2D_Invert;
	}

	constants.ObjectColor[0] = mStreamData.uObjectColor.r;
	constants.ObjectColor[1] = mStreamData.uObjectColor.g;
	constants.ObjectColor[2] = mStreamData.uObjectColor.b;
	constants.ObjectColor[3] = mStreamData.uObjectColor.a;

	constants.AddColor[0] = mStreamData.uAddColor.r;
	constants.AddColor[1] = mStreamData.uAddColor.g;
	constants.AddColor[2] = mStreamData.uAddColor.b;
	constants.AddColor[3] = mStreamData.uAddColor.a;

	constants.VertexColor[0] = mStreamData.uVertexColor.X;
	constants.VertexColor[1] = mStreamData.uVertexColor.Y;
	constants.VertexColor[2] = mStreamData.uVertexColor.Z;
	constants.VertexColor[3] = mStreamData.uVertexColor.W;

	std::memcpy(constants.ModelMatrix, mModelMatrix.get(), sizeof(constants.ModelMatrix));
	std::memcpy(constants.TexMatrix, mTextureMatrix.get(), sizeof(constants.TexMatrix));
}

NRISamplerMode NRIRenderState::GetSamplerMode() const
{
	return mFrameBuffer->GetSamplerMode(mMaterial.mClampMode);
}
