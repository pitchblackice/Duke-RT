#pragma once

#include "nri_resources.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <vector>

class NRIRenderDevice;

class NRIRenderer
{
public:
	explicit NRIRenderer(NRIRenderDevice* frameBuffer);
	~NRIRenderer();

	bool Initialize();
	void Shutdown();
	bool RenderScene(HWDrawInfo& di, int drawmode, bool portal);

private:
	struct CachedTexture
	{
		uint64_t key = 0;
		NRITextureResource resource;
	};

	bool CreatePipelineLayout();
	bool CreatePipelines();
	bool AllocateDescriptorSets();
	bool EnsureOutputTextures(uint32_t width, uint32_t height);
	bool EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials);
	bool EnsureSceneTextures(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials);
	bool UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool BuildAccelerationStructures(const nri_scene::GeometryData& geometry);
	bool DispatchPathTracing(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);

	void DestroyCachedTextures();
	void DestroyOutputTextures();
	void DestroySceneBuffers();
	void DestroyAccelerationStructures();
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	bool UpdateSamplerSet();
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors);
	bool UpdateOutputSet();

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::Pipeline* mTracePipeline = nullptr;
	nri::Pipeline* mCompositionPipeline = nullptr;
	nri::Pipeline* mFinalPipeline = nullptr;
	nri::DescriptorSet* mSamplerSet = nullptr;
	nri::DescriptorSet* mSceneTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;

	NRITextureResource mPaletteTexture;
	NRITextureResource mTraceTexture;
	NRITextureResource mComposedTexture;
	NRITextureResource mFinalTexture;

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mInstanceBuffer;
	NRIBufferResource mScratchBuffer;

	NRIAccelerationStructureResource mBottomLevelAS;
	NRIAccelerationStructureResource mTopLevelAS;

	std::vector<CachedTexture> mTextureCache;
	nri_scene::SceneDebugStats mLastStats = {};
	bool mHasLoggedStats = false;
};
