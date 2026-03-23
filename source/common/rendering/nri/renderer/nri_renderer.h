#pragma once

#include "nri_nrd.h"
#include "nri_resources.h"
#include "nri_upscaler.h"

#include "../scene/nri_map_world.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <array>
#include <vector>

class NRIRenderDevice;
struct MapRecord;

class NRIRenderer
{
public:
	explicit NRIRenderer(NRIRenderDevice* frameBuffer);
	~NRIRenderer();

	bool Initialize();
	void Shutdown();
	bool RenderScene(HWDrawInfo& di, int drawmode, bool portal);
	void ResetHistory();
	void PrintStatus() const;
	void PrintSceneBufferStatus() const;
	bool IsPathTracingSupported() const { return mPathTracingSupported; }
	const char* GetAvailabilityReason() const;

private:
	enum class FrameTextureSlot : uint32_t
	{
		ViewZ,
		Motion,
		NormalRoughness,
		BaseColorMetalness,
		UnfilteredDiffuse,
		UnfilteredSpecular,
		DenoisedDiffuse,
		DenoisedSpecular,
		Composed,
		ComposedSpecViewZ,
		TaaHistoryPing,
		TaaHistoryPong,
		Validation,
		DlssDiffuseAlbedo,
		DlssSpecularAlbedo,
		DlssSpecularHitDistance,
		DlssNormalRoughness,
		Upscaled,
		PreFinal,
		Final,
		Count
	};

	enum class PipelineSlot : uint32_t
	{
		TraceOpaque,
		Composition,
		Taa,
		RawPresent,
		FinalPresent,
		DlssBefore,
		DlssAfter,
		Final,
		Count
	};

	struct CachedTexture
	{
		uint64_t key = 0;
		NRITextureResource resource;
	};

	struct CachedSkyTexture
	{
		uint64_t key = 0;
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		NRITextureResource resource;
	};

	struct SkyState
	{
		nri_scene::PTSkyMode mode = nri_scene::PTSkyMode::None;
		nri_scene::PTSkySourceType sourceType = nri_scene::PTSkySourceType::None;
		FGameTexture* texture = nullptr;
		uint32_t faceMask = 0;
		bool flipTop = false;
	};

	struct SceneBufferDebugStats
	{
		const char* label = "";
		uint32_t growthCount = 0;
		uint32_t overwriteCount = 0;
		uint32_t uploadCount = 0;
		uint32_t growEventsLastFrame = 0;
		uint32_t overwriteEventsLastFrame = 0;
		uint64_t bytesUploadedLastFrame = 0;
		uint64_t peakUsedBytes = 0;
	};

	struct SurfaceProbeResult
	{
		bool valid = false;
		bool hit = false;
		uint32_t primitiveIndex = UINT32_MAX;
		uint32_t materialIndex = UINT32_MAX;
		uint32_t primitiveFlags = 0;
		float distance = 0.0f;
		float position[3] = {};
		float normal[3] = {};
		nri_scene::SurfaceProvenance provenance = {};
	};

	struct StaticMapSceneCache
	{
		bool valid = false;
		bool texturesResident = false;
		bool buffersResident = false;
		bool accelerationResident = false;
		uint64_t buildSerial = 0;
		uint32_t sceneBuildCount = 0;
		uint32_t gpuUploadCount = 0;
		uint32_t accelerationBuildCount = 0;
		uint32_t reuseCount = 0;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<nri_scene::MaterialData> gpuMaterials;
	};

	struct DynamicSceneFrameState
	{
		uint32_t spriteSurfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t modelCount = 0;
		uint32_t unsupportedModelCount = 0;
		uint32_t asBuildCount = 0;
	};

	bool CreatePipelineLayout();
	bool CreateTaaPipelineLayout();
	bool CreatePipelines();
	bool AllocateDescriptorSets();
	bool EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight);
	bool DispatchBootstrapView();
	bool UseFallbackSceneTextures(bool preserveExistingSky);
	bool EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials);
	bool EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky);
	bool EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky);
	bool EnsureStaticMapScene();
	bool UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool BuildAccelerationStructures(const nri_scene::GeometryData& geometry, uint32_t staticVertexCount, uint32_t staticIndexCount, uint32_t staticPrimitiveCount);
	bool DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int drawmode);
	bool DispatchTraceOpaque(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool DispatchDenoiser();
	bool DispatchComposition();
	bool DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot = FrameTextureSlot::Count);
	bool DispatchFinalPresent(FrameTextureSlot inputSlot);
	bool DispatchUpscaleChain();
	bool DispatchFinal();
	void RefreshMapWorld();
	bool CheckPathTracingSupport();
	void UpdatePerFrameState(HWDrawInfo& di);
	void ResetSceneBufferFrameStats();
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);
	void PrintMapWorldStatus() const;
	void PrintStaticMapSceneStatus() const;
	void PrintDynamicSceneStatus() const;
	void TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey);
	void UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, bool allowLogging);
	void PrintSurfaceProbeStatus() const;
	void LogFallback(const char* reason);
	void CopyFinalToActiveTarget();
	void CopyTexture(NRITextureResource& source, NRITextureResource& destination);
	void CopyTextureToActiveTarget(NRITextureResource& source);

	void DestroyCachedTextures();
	void DestroyFrameTextures();
	void DestroySceneBuffers();
	void DestroyAccelerationStructures();
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	bool UpdateSamplerSet();
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors);
	bool UpdateFrameTextureSet();
	bool UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 11>& descriptors);
	bool UpdateOutputSet();
	bool UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 12>& descriptors);
	bool CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format);
	void PrepareSceneTextureInputsForCompute();
	NRITextureResource& GetFrameTexture(FrameTextureSlot slot) { return mFrameTextures[(size_t)slot]; }
	const NRITextureResource& GetFrameTexture(FrameTextureSlot slot) const { return mFrameTextures[(size_t)slot]; }
	nri::Pipeline* GetPipeline(PipelineSlot slot) const { return mPipelines[(size_t)slot]; }
	NRIUpscalerKind GetSelectedUpscalerKind() const;
	NRIUpscalerKind ResolveUpscalerKind(bool logFallback);
	NRIUpscalerKind GetResolvedUpscalerKindForStatus() const;
	nri::UpscalerMode GetSelectedUpscalerMode() const;
	bool IsUpscalerSupported(NRIUpscalerKind kind) const;
	void FillMatrix(float* outMatrix, const VSMatrix& matrix) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::PipelineLayout* mTaaPipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)PipelineSlot::Count> mPipelines = {};
	nri::DescriptorSet* mSamplerSet = nullptr;
	nri::DescriptorSet* mSceneTextureSet = nullptr;
	nri::DescriptorSet* mFrameTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;
	nri::DescriptorSet* mCompositionFrameTextureSet = nullptr;
	nri::DescriptorSet* mCompositionOutputSet = nullptr;
	nri::DescriptorSet* mTaaFrameTextureSet = nullptr;
	nri::DescriptorSet* mTaaOutputSet = nullptr;

	NRITextureResource* GetActiveSkyTexture() { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }
	const NRITextureResource* GetActiveSkyTexture() const { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }

	NRITextureResource mPaletteTexture;
	std::array<NRITextureResource, (size_t)FrameTextureSlot::Count> mFrameTextures = {};

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mInstanceBuffer;
	NRIBufferResource mScratchBuffer;
	SceneBufferDebugStats mVertexBufferStats = { "Vertex" };
	SceneBufferDebugStats mIndexBufferStats = { "Index" };
	SceneBufferDebugStats mPrimitiveBufferStats = { "Primitive" };
	SceneBufferDebugStats mMaterialBufferStats = { "Material" };

	NRIAccelerationStructureResource mBottomLevelAS;
	NRIAccelerationStructureResource mDynamicBottomLevelAS;
	NRIAccelerationStructureResource mTopLevelAS;

	std::vector<CachedTexture> mTextureCache;
	std::vector<CachedSkyTexture> mSkyTextureCache;
	NRINrdContext mNrd;
	NRIUpscalerContext mUpscaler;
	nri_scene::PTMapWorld mMapWorld;
	StaticMapSceneCache mStaticMapScene;
	DynamicSceneFrameState mDynamicSceneLastFrame = {};
	nri_scene::SceneDebugStats mLastStats = {};
	std::array<nri::Descriptor*, 11> mFrameInputDescriptors = {};
	std::array<nri::Descriptor*, 12> mOutputDescriptors = {};
	uint32_t mFrameIndex = 0;
	uint32_t mRenderWidth = 0;
	uint32_t mRenderHeight = 0;
	uint32_t mOutputWidth = 0;
	uint32_t mOutputHeight = 0;
	nri::Format mOutputFormat = nri::Format::UNKNOWN;
	float mCurrentCameraPos[3] = {};
	float mCurrentCameraForward[3] = {};
	float mCurrentCameraRight[3] = {};
	float mCurrentCameraUp[3] = {};
	float mPreviousCameraPos[3] = {};
	float mPreviousCameraForward[3] = {};
	float mPreviousCameraRight[3] = {};
	float mPreviousCameraUp[3] = {};
	float mCurrentTanHalfFovX = 1.0f;
	float mCurrentTanHalfFovY = 1.0f;
	float mPreviousTanHalfFovX = 1.0f;
	float mPreviousTanHalfFovY = 1.0f;
	float mCurrentJitter[2] = {};
	float mPreviousJitter[2] = {};
	float mCurrentViewToClip[16] = {};
	float mPreviousViewToClip[16] = {};
	float mCurrentWorldToView[16] = {};
	float mPreviousWorldToView[16] = {};
	float mSkyColor[3] = { 0.38f, 0.48f, 0.65f };
	float mGroundColor[3] = { 0.08f, 0.08f, 0.08f };
	uint64_t mSkyTextureKey = 0;
	uint32_t mActiveSkyTextureIndex = UINT32_MAX;
	MapRecord* mSkyLevel = nullptr;
	SkyState mSkyState = {};
	SkyState mLastTracedSkyState = {};
	uint64_t mLastTracedSkyResolvedKey = 0;
	bool mHasTracedSkyState = false;
	bool mHasLoggedStats = false;
	bool mHasPreviousCameraState = false;
	bool mPathTracingSupported = true;
	bool mResetHistory = true;
	bool mUseUpscaledInFinal = false;
	bool mUseDenoisedCompositionInputs = false;
	bool mHasLoggedFallback = false;
	bool mUsedStaticMapSceneLastFrame = false;
	bool mUsedDynamicSceneLastFrame = false;
	bool mGpuSceneHasDynamicOverlay = false;
	bool mUploadedStaticMapSceneLastFrame = false;
	bool mBuiltStaticMapSceneASLastFrame = false;
	bool mBuiltDynamicSceneASLastFrame = false;
	uint64_t mObservedMapWorldBuildSerial = 0;
	uint64_t mStaticAccelerationBuildSerial = 0;
	SurfaceProbeResult mLastSurfaceProbe = {};
	SurfaceProbeResult mLastLoggedSurfaceProbe = {};
	int mLastUpscalerRequest = -1;
	NRIUpscalerKind mLastUpscalerResolved = NRIUpscalerKind::Off;
	FrameTextureSlot mHistoryInputSlot = FrameTextureSlot::TaaHistoryPing;
	FrameTextureSlot mHistoryOutputSlot = FrameTextureSlot::TaaHistoryPong;
	FrameTextureSlot mUpscaledInputSlot = FrameTextureSlot::Upscaled;
};
