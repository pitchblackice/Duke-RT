#include "nri_renderer.h"

#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

CVAR(Int, nri_ptdebug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 256;

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIComputeStage()
	{
		return nri::StageBits::COMPUTE_SHADER;
	}

	static nri::AccessStage NRIComputeShaderResourceAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
	}

	static nri::AccessStage NRIAccelerationStructureBuildInputAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureWriteAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	struct NRITraceConstants
	{
		float CameraPos[3] = {};
		uint32_t OutputWidth = 0;
		float CameraForward[3] = {};
		uint32_t OutputHeight = 0;
		float CameraRight[3] = {};
		float TanHalfFovX = 1.0f;
		float CameraUp[3] = {};
		float TanHalfFovY = 1.0f;
		float LightDirection[3] = { 0.3f, 0.85f, -0.4f };
		uint32_t PrimitiveCount = 0;
		float SkyColor[3] = { 0.38f, 0.48f, 0.65f };
		uint32_t DebugMode = 0;
		float GroundColor[3] = { 0.08f, 0.08f, 0.08f };
		uint32_t MaterialCount = 0;
	};

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void TransformPoint(const VSMatrix& matrix, float x, float y, float z, float out[4])
	{
		float point[4] = { x, y, z, 1.0f };
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, out);
	}

	static bool StatsDiffer(const nri_scene::SceneDebugStats& a, const nri_scene::SceneDebugStats& b)
	{
		return memcmp(&a, &b, sizeof(a)) != 0;
	}
}

NRIRenderer::NRIRenderer(NRIRenderDevice* frameBuffer)
	: mFrameBuffer(frameBuffer)
{
}

NRIRenderer::~NRIRenderer()
{
	Shutdown();
}

bool NRIRenderer::Initialize()
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	return CreatePipelineLayout() && AllocateDescriptorSets() && UpdateSamplerSet() && CreatePipelines();
}

void NRIRenderer::Shutdown()
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return;
	}

	DestroyAccelerationStructures();
	DestroySceneBuffers();
	DestroyOutputTextures();
	mFrameBuffer->DestroyTextureResource(mPaletteTexture);
	DestroyCachedTextures();

	if (mTracePipeline != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipeline(mTracePipeline);
		mTracePipeline = nullptr;
	}

	if (mCompositionPipeline != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipeline(mCompositionPipeline);
		mCompositionPipeline = nullptr;
	}

	if (mFinalPipeline != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipeline(mFinalPipeline);
		mFinalPipeline = nullptr;
	}

	if (mPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}

	mSamplerSet = nullptr;
	mSceneTextureSet = nullptr;
	mOutputSet = nullptr;
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if (drawmode != DM_MAINVIEW || portal || mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	nri_scene::SceneView sceneView;
	if (!nri_scene::CaptureScene(di, sceneView))
	{
		return false;
	}

	LogBridgeStats(sceneView.stats);

	nri_scene::GeometryData geometry;
	nri_scene::BuildGeometry(sceneView, geometry);
	if (geometry.primitives.empty())
	{
		return false;
	}

	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::BuildMaterials(sceneView, materialBridge);
	std::vector<nri_scene::MaterialData> gpuMaterials;

	if (!Initialize() ||
		!EnsureOutputTextures(mFrameBuffer->mActiveTarget->width, mFrameBuffer->mActiveTarget->height) ||
		!EnsurePaletteTexture(materialBridge) ||
		!EnsureSceneTextures(materialBridge, gpuMaterials) ||
		!UploadSceneBuffers(geometry, gpuMaterials) ||
		!BuildAccelerationStructures(geometry) ||
		!DispatchPathTracing(di, geometry, gpuMaterials))
	{
		return false;
	}

	return true;
}

bool NRIRenderer::CreatePipelineLayout()
{
	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = 1;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIComputeStage();

	nri::DescriptorRangeDesc sceneTextureRange = {};
	sceneTextureRange.baseRegisterIndex = 0;
	sceneTextureRange.descriptorNum = 1 + NRI_MAX_SCENE_TEXTURES;
	sceneTextureRange.descriptorType = nri::DescriptorType::TEXTURE;
	sceneTextureRange.shaderStages = NRIComputeStage();

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 3;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();

	nri::DescriptorSetDesc descriptorSets[3] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &sceneTextureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[2].registerSpace = 2;
	descriptorSets[2].ranges = &outputRange;
	descriptorSets[2].rangeNum = 1;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::RootDescriptorDesc rootDescriptors[5] = {};
	for (uint32_t i = 0; i < 5; ++i)
	{
		rootDescriptors[i].registerIndex = i;
		rootDescriptors[i].shaderStages = NRIComputeStage();
	}
	rootDescriptors[0].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;
	rootDescriptors[1].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	rootDescriptors[2].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	rootDescriptors[3].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	rootDescriptors[4].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 4;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.rootDescriptors = rootDescriptors;
	desc.rootDescriptorNum = (uint32_t)std::size(rootDescriptors);
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::AllocateDescriptorSets()
{
	return
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &mSamplerSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 1, &mSceneTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 2, &mOutputSet, 1, 0) == nri::Result::SUCCESS;
}

bool NRIRenderer::UpdateSamplerSet()
{
	const nri::Descriptor* descriptor = mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampLinear];
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSamplerSet;
	update.rangeIndex = 0;
	update.descriptors = &descriptor;
	update.descriptorNum = 1;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors)
{
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSceneTextureSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors.data();
	update.descriptorNum = (uint32_t)descriptors.size();
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateOutputSet()
{
	const nri::Descriptor* descriptors[3] = { mTraceTexture.storageView, mComposedTexture.storageView, mFinalTexture.storageView };
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mOutputSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors;
	update.descriptorNum = 3;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::CreatePipelines()
{
	auto createPipeline = [this](const char* fileName, nri::Pipeline*& outPipeline)
	{
		std::vector<uint8_t> shaderBlob;
		if (!mFrameBuffer->LoadShaderBlob(fileName, shaderBlob))
		{
			return false;
		}

		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = shaderBlob.data();
		shader.size = shaderBlob.size();
		shader.entryPointName = "main";

		nri::ComputePipelineDesc pipelineDesc = {};
		pipelineDesc.pipelineLayout = mPipelineLayout;
		pipelineDesc.shader = shader;
		return mFrameBuffer->mCore.CreateComputePipeline(*mFrameBuffer->mDevice, pipelineDesc, outPipeline) == nri::Result::SUCCESS;
	};

	const bool d3d12 = mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	return
		createPipeline(d3d12 ? "TraceOpaque.cs.dxil" : "TraceOpaque.cs.spirv", mTracePipeline) &&
		createPipeline(d3d12 ? "Composition.cs.dxil" : "Composition.cs.spirv", mCompositionPipeline) &&
		createPipeline(d3d12 ? "Final.cs.dxil" : "Final.cs.spirv", mFinalPipeline);
}

bool NRIRenderer::EnsureOutputTextures(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	const bool upToDate =
		mTraceTexture.texture != nullptr &&
		mTraceTexture.width == width &&
		mTraceTexture.height == height &&
		mComposedTexture.width == width &&
		mComposedTexture.height == height &&
		mFinalTexture.width == width &&
		mFinalTexture.height == height;

	if (upToDate)
	{
		return true;
	}

	DestroyOutputTextures();

	if (!mFrameBuffer->CreateOwnedTexture(mTraceTexture, width, height, nri::Format::RGBA16_SFLOAT, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE)) ||
		!mFrameBuffer->CreateOwnedTexture(mComposedTexture, width, height, nri::Format::RGBA16_SFLOAT, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE)) ||
		!mFrameBuffer->CreateOwnedTexture(mFinalTexture, width, height, nri::Format::BGRA8_UNORM, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE)))
	{
		return false;
	}

	return UpdateOutputSet();
}

bool NRIRenderer::EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials)
{
	if (mPaletteTexture.texture != nullptr &&
		mPaletteTexture.width == materials.paletteWidth &&
		mPaletteTexture.height == materials.paletteHeight)
	{
		return true;
	}

	mFrameBuffer->DestroyTextureResource(mPaletteTexture);
	if (!mFrameBuffer->CreateOwnedTexture(mPaletteTexture, materials.paletteWidth, materials.paletteHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE))
	{
		return false;
	}

	return mFrameBuffer->UploadTextureData(mPaletteTexture, materials.paletteLookup.data(), materials.paletteWidth, materials.paletteHeight, materials.paletteWidth * 4u);
}

bool NRIRenderer::EnsureSceneTextures(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials)
{
	outGpuMaterials = materials.materials;
	std::vector<nri::Descriptor*> descriptors(1 + NRI_MAX_SCENE_TEXTURES, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mPaletteTexture.shaderView;

	for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)materials.textures.size(), NRI_MAX_SCENE_TEXTURES); ++i)
	{
		const auto& upload = materials.textures[i];
		auto it = std::find_if(mTextureCache.begin(), mTextureCache.end(), [&upload](const CachedTexture& entry) { return entry.key == upload.key; });
		if (it == mTextureCache.end())
		{
			CachedTexture cacheEntry = {};
			cacheEntry.key = upload.key;
			const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
			const uint32_t rowPitch = upload.indexed ? upload.width : upload.width * 4u;
			if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, upload.width, upload.height, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
				!mFrameBuffer->UploadTextureData(cacheEntry.resource, upload.pixels.data(), upload.width, upload.height, rowPitch))
			{
				return false;
			}

			mTextureCache.push_back(cacheEntry);
			it = mTextureCache.end() - 1;
		}

		descriptors[1 + i] = it->resource.shaderView;
	}

	for (auto& material : outGpuMaterials)
	{
		if (material.textureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.textureIndex = 0;
		}
	}

	return UpdateSceneTextureSet(descriptors);
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;

	if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.size = desc.size;
	resource.stride = stride;

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BufferUploadDesc uploadDesc = {};
	uploadDesc.data = data;
	uploadDesc.buffer = resource.buffer;
	uploadDesc.after = after;
	return mFrameBuffer->mHelper.UploadData(*mFrameBuffer->mGraphicsQueue, nullptr, 0, &uploadDesc, 1) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.size = desc.size;
	resource.stride = stride;
	return true;
}

bool NRIRenderer::UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	DestroySceneBuffers();

	if (!CreateStructuredBuffer(mVertexBuffer, geometry.vertices.data(), geometry.vertices.size() * sizeof(nri_scene::SceneVertex), sizeof(nri_scene::SceneVertex), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) ||
		!CreateStructuredBuffer(mIndexBuffer, geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), sizeof(uint32_t), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) ||
		!CreateStructuredBuffer(mPrimitiveBuffer, geometry.primitives.data(), geometry.primitives.size() * sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()) ||
		!CreateStructuredBuffer(mMaterialBuffer, materials.data(), materials.size() * sizeof(nri_scene::MaterialData), sizeof(nri_scene::MaterialData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	return true;
}

bool NRIRenderer::BuildAccelerationStructures(const nri_scene::GeometryData& geometry)
{
	DestroyAccelerationStructures();

	nri::BottomLevelGeometryDesc blasGeometry = {};
	blasGeometry.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
	blasGeometry.type = nri::BottomLevelGeometryType::TRIANGLES;
	blasGeometry.triangles.vertexBuffer = mVertexBuffer.buffer;
	blasGeometry.triangles.vertexOffset = 0;
	blasGeometry.triangles.vertexNum = (uint32_t)geometry.vertices.size();
	blasGeometry.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
	blasGeometry.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
	blasGeometry.triangles.indexBuffer = mIndexBuffer.buffer;
	blasGeometry.triangles.indexOffset = 0;
	blasGeometry.triangles.indexNum = (uint32_t)geometry.indices.size();
	blasGeometry.triangles.indexType = nri::IndexType::UINT32;

	nri::AccelerationStructureDesc blasDesc = {};
	blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
	blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	blasDesc.geometries = &blasGeometry;
	blasDesc.geometryOrInstanceNum = 1;
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, mBottomLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = 1;
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, mTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	const uint64_t maxScratchSize = std::max(
		mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mBottomLevelAS.accelerationStructure),
		mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mTopLevelAS.accelerationStructure));

	if (!CreateBufferWithoutView(mScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	nri::TopLevelInstance instance = {};
	instance.transform[0][0] = 1.0f;
	instance.transform[1][1] = 1.0f;
	instance.transform[2][2] = 1.0f;
	instance.instanceId = 0;
	instance.mask = 0xFF;
	instance.shaderBindingTableLocalOffset = 0;
	instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
	instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mBottomLevelAS.accelerationStructure);

	if (!CreateStructuredBuffer(mInstanceBuffer, &instance, sizeof(instance), sizeof(instance), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT, NRIAccelerationStructureBuildInputAccess()))
	{
		return false;
	}

	if (mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*mTopLevelAS.accelerationStructure, mTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildBottomLevelAccelerationStructureDesc blasBuild = {};
	blasBuild.dst = mBottomLevelAS.accelerationStructure;
	blasBuild.geometries = &blasGeometry;
	blasBuild.geometryNum = 1;
	blasBuild.scratchBuffer = mScratchBuffer.buffer;
	blasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &blasBuild, 1);

	nri::BufferBarrierDesc blasBarrier = {};
	blasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mBottomLevelAS.accelerationStructure);
	blasBarrier.before = NRIAccelerationStructureWriteAccess();
	blasBarrier.after = NRIAccelerationStructureReadAccess();
	nri::BarrierDesc blasBarrierDesc = {};
	blasBarrierDesc.buffers = &blasBarrier;
	blasBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = mTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = 1;
	tlasBuild.instanceBuffer = mInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = mScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIComputeShaderResourceAccess();

	nri::BufferBarrierDesc sceneBarriers[2] = {};
	sceneBarriers[0].buffer = mVertexBuffer.buffer;
	sceneBarriers[0].before = NRIAccelerationStructureBuildInputAccess();
	sceneBarriers[0].after = NRIComputeShaderResourceAccess();
	sceneBarriers[1].buffer = mIndexBuffer.buffer;
	sceneBarriers[1].before = NRIAccelerationStructureBuildInputAccess();
	sceneBarriers[1].after = NRIComputeShaderResourceAccess();

	nri::BufferBarrierDesc barriers[] = { tlasBarrier, sceneBarriers[0], sceneBarriers[1] };
	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers;
	barrierDesc.bufferNum = (uint32_t)std::size(barriers);
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	return true;
}

bool NRIRenderer::DispatchPathTracing(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	NRITraceConstants constants = {};
	constants.CameraPos[0] = di.VPUniforms.mCameraPos.X;
	constants.CameraPos[1] = di.VPUniforms.mCameraPos.Y;
	constants.CameraPos[2] = di.VPUniforms.mCameraPos.Z;
	constants.OutputWidth = mFinalTexture.width;
	constants.OutputHeight = mFinalTexture.height;
	constants.PrimitiveCount = (uint32_t)geometry.primitives.size();
	constants.MaterialCount = (uint32_t)materials.size();
	constants.DebugMode = (uint32_t)nri_ptdebug;

	VSMatrix inverseView;
	if (!di.VPUniforms.mViewMatrix.inverseMatrix(inverseView))
	{
		return false;
	}

	float origin[4] = {};
	float rightPoint[4] = {};
	float upPoint[4] = {};
	float forwardPoint[4] = {};
	TransformPoint(inverseView, 0.0f, 0.0f, 0.0f, origin);
	TransformPoint(inverseView, 1.0f, 0.0f, 0.0f, rightPoint);
	TransformPoint(inverseView, 0.0f, 1.0f, 0.0f, upPoint);
	TransformPoint(inverseView, 0.0f, 0.0f, -1.0f, forwardPoint);

	for (int i = 0; i < 3; ++i)
	{
		constants.CameraRight[i] = rightPoint[i] - origin[i];
		constants.CameraUp[i] = upPoint[i] - origin[i];
		constants.CameraForward[i] = forwardPoint[i] - origin[i];
	}

	Normalize3(constants.CameraRight);
	Normalize3(constants.CameraUp);
	Normalize3(constants.CameraForward);
	Normalize3(constants.LightDirection);

	const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
	constants.TanHalfFovX = tanHalfFovX;
	constants.TanHalfFovY = tanHalfFovX * ((float)mFinalTexture.height / (float)mFinalTexture.width);

	mFrameBuffer->TransitionTexture(mTraceTexture, NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(mComposedTexture, NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(mFinalTexture, NRIComputeStorageState());

	auto setBindings = [this, &constants]()
	{
		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 0, mTopLevelAS.descriptor, 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 1, mVertexBuffer.shaderView, 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 2, mIndexBuffer.shaderView, 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 3, mPrimitiveBuffer.shaderView, 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 4, mMaterialBuffer.shaderView, 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mOutputSet, nri::BindPoint::COMPUTE });
	};

	const nri::DispatchDesc dispatchDesc = { GetDispatchSize(mFinalTexture.width), GetDispatchSize(mFinalTexture.height), 1 };

	setBindings();
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *mTracePipeline);
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, dispatchDesc);

	nri::TextureBarrierDesc traceBarrier = {};
	traceBarrier.texture = mTraceTexture.texture;
	traceBarrier.before = NRIComputeStorageState();
	traceBarrier.after = NRIComputeStorageState();
	traceBarrier.mipNum = 1;
	traceBarrier.layerNum = 1;
	traceBarrier.planes = nri::PlaneBits::COLOR;
	nri::BarrierDesc traceBarrierDesc = {};
	traceBarrierDesc.textures = &traceBarrier;
	traceBarrierDesc.textureNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, traceBarrierDesc);

	setBindings();
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *mCompositionPipeline);
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, dispatchDesc);

	nri::TextureBarrierDesc composedBarrier = {};
	composedBarrier.texture = mComposedTexture.texture;
	composedBarrier.before = NRIComputeStorageState();
	composedBarrier.after = NRIComputeStorageState();
	composedBarrier.mipNum = 1;
	composedBarrier.layerNum = 1;
	composedBarrier.planes = nri::PlaneBits::COLOR;
	nri::BarrierDesc composedBarrierDesc = {};
	composedBarrierDesc.textures = &composedBarrier;
	composedBarrierDesc.textureNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, composedBarrierDesc);

	setBindings();
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *mFinalPipeline);
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, dispatchDesc);

	mFrameBuffer->TransitionTexture(mFinalTexture, NRICopySourceState());
	mFrameBuffer->TransitionTexture(*mFrameBuffer->mActiveTarget, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *mFrameBuffer->mActiveTarget->texture, nullptr, *mFinalTexture.texture, nullptr);
	return true;
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!mHasLoggedStats || StatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::DestroyCachedTextures()
{
	for (auto& texture : mTextureCache)
	{
		mFrameBuffer->DestroyTextureResource(texture.resource);
	}
	mTextureCache.clear();
}

void NRIRenderer::DestroyOutputTextures()
{
	mFrameBuffer->DestroyTextureResource(mTraceTexture);
	mFrameBuffer->DestroyTextureResource(mComposedTexture);
	mFrameBuffer->DestroyTextureResource(mFinalTexture);
}

void NRIRenderer::DestroySceneBuffers()
{
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	DestroyBufferResource(mInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
}

void NRIRenderer::DestroyAccelerationStructures()
{
	DestroyAccelerationStructureResource(mBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);
}

void NRIRenderer::DestroyBufferResource(NRIBufferResource& resource)
{
	if (resource.shaderView != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}

	if (resource.buffer != nullptr)
	{
		mFrameBuffer->mCore.DestroyBuffer(resource.buffer);
		resource.buffer = nullptr;
	}

	resource.size = 0;
	resource.stride = 0;
}

void NRIRenderer::DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource)
{
	if (resource.descriptor != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.descriptor);
		resource.descriptor = nullptr;
	}

	if (resource.accelerationStructure != nullptr)
	{
		mFrameBuffer->mRayTracing.DestroyAccelerationStructure(resource.accelerationStructure);
		resource.accelerationStructure = nullptr;
	}
}
