#pragma once

#include "nri_nrd.h"
#include "nri_resources.h"
#include "nri_upscaler.h"

#include "../scene/nri_map_builder.h"
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
	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId);
	bool RemoveRuntimePointLight(uint32_t id);
	void ClearRuntimePointLights();
	void PrintRuntimePointLights() const;
	uint32_t GetRuntimePointLightCount() const;
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
		UnfilteredPenumbra,
		DenoisedDiffuse,
		DenoisedSpecular,
		DenoisedShadow,
		Composed,
		DirectLighting,
		DirectEmission,
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

	struct RuntimePointLightData
	{
		uint32_t id = 0;
		float position[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		float radius = 0.0f;
	};

	struct RuntimePointLightGpuData
	{
		float position[3] = {};
		float radius = 0.0f;
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
	};

	struct StaticMapSceneCache
	{
		struct ChunkCache
		{
			uint32_t chunkIndex = UINT32_MAX;
			uint32_t vertexOffset = 0;
			uint32_t vertexCount = 0;
			uint32_t indexOffset = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveOffset = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialOffset = 0;
			uint32_t materialCount = 0;
			NRIAccelerationStructureResource accelerationStructure;
		};

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
		std::vector<ChunkCache> chunks;
		uint32_t tlasInstanceCount = 0;
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

	struct RuntimeMapMutationCache
	{
		struct ChunkReplacement
		{
			nri_scene::PTMapChunkMutationBaseline baseline;
			uint64_t baselineSignature = 0;
			uint64_t liveSignature = 0;
			uint64_t lastTraceSignature = UINT64_MAX;
			uint32_t reasonMask = 0;
			uint32_t sectionDirtyCount = 0;
			uint32_t lastTraceReasonMask = UINT32_MAX;
			uint32_t traceCount = 0;
			bool active = false;
			bool valid = false;
			bool sectorDirty = false;
			bool dragged = false;
			bool blindSpot = false;
			bool lastTraceActive = false;
			bool lastTraceBlindSpot = false;
			uint32_t surfaceCount = 0;
			uint32_t triangleCount = 0;
			nri_scene::GeometryData geometry;
			nri_scene::MaterialBridgeData materialBridge;
		};

		std::vector<ChunkReplacement> chunks;
		std::vector<uint8_t> replacedChunkMask;
	};

	struct RuntimeMapMutationFrameState
	{
		bool active = false;
		uint32_t dirtyChunkCount = 0;
		uint32_t replacedChunkCount = 0;
		uint32_t rebuiltChunkCount = 0;
		uint32_t heldChunkCount = 0;
		uint32_t blindSpotChunkCount = 0;
		uint32_t sectorGeometryChunkCount = 0;
		uint32_t sectorMaterialChunkCount = 0;
		uint32_t wallGeometryChunkCount = 0;
		uint32_t wallMaterialChunkCount = 0;
		uint32_t sectorDirtyChunkCount = 0;
		uint32_t sectionDirtyChunkCount = 0;
		uint32_t draggedChunkCount = 0;
		uint32_t replacementSurfaceCount = 0;
		uint32_t replacementTriangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeSpaceLinkFrameState
	{
		bool active = false;
		bool geoEffectActive = false;
		bool topologyChanged = false;
		bool queryAttempted = false;
		bool queryRejected = false;
		int32_t candidateSectorIndex = -1;
		int32_t candidateSectorLotag = -1;
		int32_t sourceSectorIndex = -1;
		int32_t reportedGeoCount = 0;
		uint32_t viewRootSectorCount = 0;
		uint32_t visibleSectorCount = 0;
		uint32_t providerSectorCount = 0;
		uint32_t geoProviderCount = 0;
		uint32_t providerGroupCount = 0;
		uint32_t localSpaceMatchedProviderCount = 0;
		uint32_t visibleMatchedProviderCount = 0;
		uint32_t linkCount = 0;
		uint32_t translatedChunkCount = 0;
		uint32_t orphanLocalSpaceCount = 0;
		uint32_t unresolvedRuntimePortalCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
	};

	struct RuntimeChunkTranslationState
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
	};

	struct RuntimeLinkTraceState
	{
		bool valid = false;
		int32_t candidateSectorIndex = -1;
		int32_t sourceSectorIndex = -1;
		bool geoEffectActive = false;
		uint32_t visibleTaggedSectorCount = 0;
		uint32_t visible848SectorCount = 0;
		uint32_t visibleTeleportSectorCount = 0;
		uint32_t taggedVisibleSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 8> taggedVisibleSectors = {};
		uint32_t nearbyControlSectorStoredCount = 0;
		std::array<RuntimeTaggedSectorDebugInfo, 12> nearbyControlSectors = {};
		RuntimeLinkDebugState game = {};
	};

	struct SceneInstanceData
	{
		uint32_t primitiveOffset = 0;
		uint32_t dataSource = 0;
		uint32_t reserved0 = 0;
		uint32_t reserved1 = 0;
	};

	enum SceneDataBufferMask : uint32_t
	{
		SceneDataBufferMask_None = 0,
		SceneDataBufferMask_Static = 1 << 0,
		SceneDataBufferMask_Dynamic = 1 << 1,
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
	bool UploadSceneBuffers(
		NRIBufferResource& vertexBuffer,
		NRIBufferResource& indexBuffer,
		NRIBufferResource& primitiveBuffer,
		NRIBufferResource& materialBuffer,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials);
	bool BuildStaticMapAccelerationStructures();
	bool BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask);
	bool BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry);
	bool BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	bool BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	void BuildRuntimePointLightUpload(std::vector<RuntimePointLightGpuData>& outLights) const;
	bool UpdateSceneDataSet(
		const NRIBufferResource& staticVertexBuffer,
		const NRIBufferResource& staticIndexBuffer,
		const NRIBufferResource& staticPrimitiveBuffer,
		const NRIBufferResource& staticMaterialBuffer,
		const NRIBufferResource& dynamicVertexBuffer,
		const NRIBufferResource& dynamicIndexBuffer,
		const NRIBufferResource& dynamicPrimitiveBuffer,
		const NRIBufferResource& dynamicMaterialBuffer,
		const std::vector<SceneInstanceData>& sceneInstances,
		uint32_t staticPrimitiveCount,
		uint32_t dynamicPrimitiveCount,
		uint32_t staticMaterialCount,
		uint32_t dynamicMaterialCount);
	void BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask = nullptr) const;
	bool RestoreStaticTopLevelScene();
	bool DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int drawmode);
	bool DispatchTraceOpaque(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials);
	bool DispatchDenoiser();
	bool DispatchComposition();
	bool DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot = FrameTextureSlot::Count, FrameTextureSlot tertiarySlot = FrameTextureSlot::Count);
	bool DispatchFinalPresent(FrameTextureSlot inputSlot);
	bool DispatchUpscaleChain();
	bool DispatchFinal();
	void RefreshMapWorld();
	bool CheckPathTracingSupport();
	void UpdatePerFrameState(HWDrawInfo& di);
	void ResetSceneBufferFrameStats();
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);
	void PrintMapWorldStatus() const;
	void PrintPortalTraversalStatus() const;
	void PrintStaticMapSceneStatus() const;
	void PrintDynamicSceneStatus() const;
	void PrintTemporalStatus() const;
	void PrintRuntimeMapMutationStatus() const;
	void PrintRuntimeSpaceLinkStatus() const;
	void ArmTemporalTraceBudget(const char* reason);
	void TraceTemporalState(const char* stage, NRIUpscalerKind resolvedUpscaler, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
	void TraceRuntimeLinkEvents(HWDrawInfo& di);
	void TraceRuntimeMapMutationChunk(const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement);
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
	void DestroyStaticMapSceneCache();
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);
	const NRIBufferResource& GetActiveVertexBuffer() const;
	const NRIBufferResource& GetActiveIndexBuffer() const;
	const NRIBufferResource& GetActivePrimitiveBuffer() const;
	const NRIBufferResource& GetActiveMaterialBuffer() const;
	void BindSceneRootDescriptors();

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	bool UpdateSamplerSet();
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors);
	bool UpdateFrameTextureSet();
	bool UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 14>& descriptors);
	bool UpdateOutputSet();
	bool UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 15>& descriptors);
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
	const char* GetFrameTextureSlotName(FrameTextureSlot slot) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::PipelineLayout* mTaaPipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)PipelineSlot::Count> mPipelines = {};
	nri::DescriptorSet* mSamplerSet = nullptr;
	nri::DescriptorSet* mSceneTextureSet = nullptr;
	nri::DescriptorSet* mSceneDataSet = nullptr;
	nri::DescriptorSet* mFrameTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;
	nri::DescriptorSet* mCompositionFrameTextureSet = nullptr;
	nri::DescriptorSet* mCompositionOutputSet = nullptr;
	nri::DescriptorSet* mTaaFrameTextureSet = nullptr;
	nri::DescriptorSet* mTaaOutputSet = nullptr;
	nri::DescriptorSet* mPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mPresentOutputSet = nullptr;

	NRITextureResource* GetActiveSkyTexture() { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }
	const NRITextureResource* GetActiveSkyTexture() const { return mActiveSkyTextureIndex < mSkyTextureCache.size() ? &mSkyTextureCache[mActiveSkyTextureIndex].resource : nullptr; }

	NRITextureResource mPaletteTexture;
	std::array<NRITextureResource, (size_t)FrameTextureSlot::Count> mFrameTextures = {};

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mStaticVertexBuffer;
	NRIBufferResource mStaticIndexBuffer;
	NRIBufferResource mStaticPrimitiveBuffer;
	NRIBufferResource mStaticMaterialBuffer;
	NRIBufferResource mTlasInstanceBuffer;
	NRIBufferResource mSceneInstanceBuffer;
	NRIBufferResource mPortalBuffer;
	NRIBufferResource mRuntimeLightBuffer;
	NRIBufferResource mScratchBuffer;
	NRIBufferResource mTopLevelScratchBuffer;
	SceneBufferDebugStats mVertexBufferStats = { "Vertex" };
	SceneBufferDebugStats mIndexBufferStats = { "Index" };
	SceneBufferDebugStats mPrimitiveBufferStats = { "Primitive" };
	SceneBufferDebugStats mMaterialBufferStats = { "Material" };
	SceneBufferDebugStats mPortalBufferStats = { "Portal" };
	SceneBufferDebugStats mRuntimeLightBufferStats = { "RuntimeLight" };

	NRIAccelerationStructureResource mDynamicBottomLevelAS;
	NRIAccelerationStructureResource mTopLevelAS;

	std::vector<CachedTexture> mTextureCache;
	std::vector<CachedSkyTexture> mSkyTextureCache;
	NRINrdContext mNrd;
	NRIUpscalerContext mUpscaler;
	nri_scene::PTMapWorld mMapWorld;
	StaticMapSceneCache mStaticMapScene;
	RuntimeMapMutationCache mRuntimeMapMutations;
	DynamicSceneFrameState mDynamicSceneLastFrame = {};
	RuntimeMapMutationFrameState mRuntimeMapLastFrame = {};
	RuntimeSpaceLinkFrameState mRuntimeSpaceLinkLastFrame = {};
	RuntimeLinkTraceState mLastRuntimeLinkTraceState = {};
	std::vector<RuntimeChunkTranslationState> mRuntimeChunkTranslationHistory;
	nri_scene::SceneDebugStats mLastStats = {};
	std::vector<RuntimePointLightData> mRuntimePointLights;
	std::array<nri::Descriptor*, 14> mFrameInputDescriptors = {};
	std::array<nri::Descriptor*, 15> mOutputDescriptors = {};
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
	bool mHasRuntimeLinkTraceState = false;
	bool mResetHistory = true;
	bool mUseUpscaledInFinal = false;
	bool mUseDenoisedCompositionInputs = false;
	bool mUseSplitShadowDenoiser = false;
	bool mHasLoggedFallback = false;
	bool mUsedStaticMapSceneLastFrame = false;
	bool mUsedDynamicSceneLastFrame = false;
	bool mGpuSceneHasDynamicOverlay = false;
	bool mUploadedStaticMapSceneLastFrame = false;
	bool mBuiltStaticMapSceneASLastFrame = false;
	bool mBuiltDynamicSceneASLastFrame = false;
	uint64_t mObservedMapWorldBuildSerial = 0;
	uint64_t mStaticAccelerationBuildSerial = 0;
	uint32_t mActiveTlasInstanceCount = 0;
	uint32_t mBoundStaticPrimitiveCount = 0;
	uint32_t mBoundDynamicPrimitiveCount = 0;
	uint32_t mBoundStaticMaterialCount = 0;
	uint32_t mBoundDynamicMaterialCount = 0;
	uint32_t mBoundPortalCount = 0;
	uint32_t mBoundRuntimeLightCount = 0;
	uint32_t mNextRuntimePointLightId = 1;
	SurfaceProbeResult mLastSurfaceProbe = {};
	SurfaceProbeResult mLastLoggedSurfaceProbe = {};
	int mLastDebugMode = -1;
	int mLastUpscalerRequest = -1;
	NRIUpscalerKind mLastUpscalerResolved = NRIUpscalerKind::Off;
	NRIUpscalerKind mLastTemporalHistoryUpscaler = NRIUpscalerKind::Off;
	FrameTextureSlot mHistoryInputSlot = FrameTextureSlot::TaaHistoryPing;
	FrameTextureSlot mHistoryOutputSlot = FrameTextureSlot::TaaHistoryPong;
	FrameTextureSlot mUpscaledInputSlot = FrameTextureSlot::Upscaled;
};
