#include "nri_persistent_voxel_services.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_render_geometry_helpers.h"
#include "nri_voxel_compute_preload.h"
#include "nri_voxel_compute_meshing.h"
#include "../system/nri_renderdevice.h"
#include "../scene/nri_material_bridge.h"
#include "hw_voxels.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "mapinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace
{
	struct PersistentVoxelPalettePayload
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> bytes;
	};

	struct PersistentVoxelMaterialClosureServiceState
	{
		uint64_t buildSerial = 0;
		NRIPersistentVoxelMaterialClosureRegistry registry;
		NRIPersistentVoxelMaterialClosureTelemetry telemetry = {};
		std::unordered_map<uint64_t, PersistentVoxelPalettePayload> palettes;
	};

	std::unordered_map<NRIRenderer*, PersistentVoxelMaterialClosureServiceState> gPersistentVoxelMaterialClosureStates;

	uint64_t HashPersistentVoxelClosureBytes(const void* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	NRIPersistentVoxelTextureClosureState ConvertPersistentVoxelTextureClosureState(
		NRISceneTextureClosureState state)
	{
		switch (state)
		{
		case NRISceneTextureClosureState::Ready: return NRIPersistentVoxelTextureClosureState::Ready;
		case NRISceneTextureClosureState::NotRequired: return NRIPersistentVoxelTextureClosureState::NotRequired;
		case NRISceneTextureClosureState::Deferred: return NRIPersistentVoxelTextureClosureState::Deferred;
		case NRISceneTextureClosureState::Failed: return NRIPersistentVoxelTextureClosureState::Failed;
		default: return NRIPersistentVoxelTextureClosureState::Pending;
		}
	}

	NRIPersistentVoxelTextureClosureFailure ConvertPersistentVoxelTextureClosureFailure(
		NRISceneTextureClosureFailure failure)
	{
		switch (failure)
		{
		case NRISceneTextureClosureFailure::DynamicTexture: return NRIPersistentVoxelTextureClosureFailure::DynamicTexture;
		case NRISceneTextureClosureFailure::PayloadUnavailable: return NRIPersistentVoxelTextureClosureFailure::PayloadUnavailable;
		case NRISceneTextureClosureFailure::ResourceCreation: return NRIPersistentVoxelTextureClosureFailure::ResourceCreation;
		case NRISceneTextureClosureFailure::DescriptorUnavailable: return NRIPersistentVoxelTextureClosureFailure::DescriptorUnavailable;
		case NRISceneTextureClosureFailure::ResidencyLost: return NRIPersistentVoxelTextureClosureFailure::ResidencyLost;
		default: return NRIPersistentVoxelTextureClosureFailure::None;
		}
	}

	PersistentVoxelMaterialClosureServiceState& GetPersistentVoxelMaterialClosureState(NRIRenderer& renderer)
	{
		return gPersistentVoxelMaterialClosureStates[&renderer];
	}

	void EnsurePersistentVoxelMaterialClosureBuildSerial(NRIRenderer& renderer, uint64_t buildSerial)
	{
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		if (state.buildSerial == buildSerial)
		{
			return;
		}
		state = {};
		state.buildSerial = buildSerial;
		state.telemetry.buildSerial = buildSerial;
		state.registry.Reset(buildSerial);
	}

	NRIPersistentVoxelMaterialRowSeed BuildPersistentVoxelMaterialRowSeed(const nri_scene::MaterialData& material)
	{
		NRIPersistentVoxelMaterialRowSeed seed = {};
		seed.textureIndex = material.textureIndex;
		seed.paletteIndex = material.paletteIndex;
		seed.flags = material.flags;
		seed.materialClass = material.materialClass;
		seed.lightingFlags = material.lightingFlags;
		seed.normalTextureIndex = material.normalTextureIndex;
		seed.metallicTextureIndex = material.metallicTextureIndex;
		seed.roughnessTextureIndex = material.roughnessTextureIndex;
		seed.sectorIndex = material.sectorIndex;
		seed.emissiveTextureIndex = material.emissiveTextureIndex;
		seed.lightLevel = material.lightLevel;
		seed.alpha = material.alpha;
		seed.roughnessHint = material.roughnessHint;
		seed.metalnessHint = material.metalnessHint;
		std::copy(std::begin(material.emissiveColor), std::end(material.emissiveColor), std::begin(seed.emissiveColor));
		seed.emissiveIntensity = material.emissiveIntensity;
		seed.emissiveMaskScale = material.emissiveMaskScale;
		seed.emissiveMode = material.emissiveMode;
		seed.emissiveReserved = material.emissiveReserved;
		return seed;
	}

	NRIPersistentVoxelMaterialLightingSeed BuildPersistentVoxelMaterialLightingSeed(
		const nri_scene::MaterialLightingMetadata& metadata)
	{
		NRIPersistentVoxelMaterialLightingSeed seed = {};
		seed.materialKey = metadata.materialKey;
		seed.textureContentKey = metadata.textureContentKey;
		seed.glowmapContentKey = metadata.glowmapContentKey;
		seed.normalContentKey = metadata.normalContentKey;
		seed.metallicContentKey = metadata.metallicContentKey;
		seed.roughnessContentKey = metadata.roughnessContentKey;
		seed.textureId = metadata.textureId;
		seed.baseTextureId = metadata.baseTextureId;
		seed.textureIndex = metadata.textureIndex;
		seed.glowmapTextureIndex = metadata.glowmapTextureIndex;
		seed.normalTextureIndex = metadata.normalTextureIndex;
		seed.metallicTextureIndex = metadata.metallicTextureIndex;
		seed.roughnessTextureIndex = metadata.roughnessTextureIndex;
		seed.emissiveTextureIndex = metadata.emissiveTextureIndex;
		seed.paletteIndex = metadata.paletteIndex;
		seed.materialFlags = metadata.materialFlags;
		seed.lightingFlags = metadata.lightingFlags;
		seed.materialClass = metadata.materialClass;
		seed.emissiveMode = metadata.emissiveMode;
		seed.emissiveStableFrames = metadata.emissiveStableFrames;
		seed.sourceType = (uint32_t)metadata.sourceType;
		seed.sectorIndex = metadata.sectorIndex;
		seed.actorIndex = metadata.actorIndex;
		seed.actorOverlayRuleCount = std::min<uint32_t>(metadata.actorOverlayRuleCount, (uint32_t)std::size(seed.actorOverlayRuleIds));
		std::copy_n(metadata.actorOverlayRuleIds, seed.actorOverlayRuleCount, seed.actorOverlayRuleIds);
		seed.shade = metadata.shade;
		seed.alpha = metadata.alpha;
		seed.lightLevel = metadata.lightLevel;
		std::copy(std::begin(metadata.averageColor), std::end(metadata.averageColor), std::begin(seed.averageColor));
		std::copy(std::begin(metadata.glowColor), std::end(metadata.glowColor), std::begin(seed.glowColor));
		std::copy(std::begin(metadata.emissiveColor), std::end(metadata.emissiveColor), std::begin(seed.emissiveColor));
		seed.emissiveIntensity = metadata.emissiveIntensity;
		seed.emissiveMaskScale = metadata.emissiveMaskScale;
		seed.visibleFullbrightBoost = metadata.visibleFullbrightBoost;
		return seed;
	}

	nri_scene::MaterialData BuildPersistentVoxelMaterialData(const NRIPersistentVoxelMaterialRowSeed& seed)
	{
		nri_scene::MaterialData material = {};
		material.textureIndex = seed.textureIndex;
		material.paletteIndex = seed.paletteIndex;
		material.flags = seed.flags;
		material.materialClass = seed.materialClass;
		material.lightingFlags = seed.lightingFlags;
		material.normalTextureIndex = seed.normalTextureIndex;
		material.metallicTextureIndex = seed.metallicTextureIndex;
		material.roughnessTextureIndex = seed.roughnessTextureIndex;
		material.sectorIndex = seed.sectorIndex;
		material.emissiveTextureIndex = seed.emissiveTextureIndex;
		material.lightLevel = seed.lightLevel;
		material.alpha = seed.alpha;
		material.roughnessHint = seed.roughnessHint;
		material.metalnessHint = seed.metalnessHint;
		std::copy(std::begin(seed.emissiveColor), std::end(seed.emissiveColor), std::begin(material.emissiveColor));
		material.emissiveIntensity = seed.emissiveIntensity;
		material.emissiveMaskScale = seed.emissiveMaskScale;
		material.emissiveMode = seed.emissiveMode;
		material.emissiveReserved = seed.emissiveReserved;
		return material;
	}

	nri_scene::MaterialLightingMetadata BuildPersistentVoxelMaterialLightingMetadata(
		const NRIPersistentVoxelMaterialLightingSeed& seed)
	{
		nri_scene::MaterialLightingMetadata metadata = {};
		metadata.texture = TexMan.GameByIndex((int)seed.textureId);
		if (metadata.texture != nullptr && !metadata.texture->isValid())
		{
			metadata.texture = nullptr;
		}
		metadata.materialKey = seed.materialKey;
		metadata.textureContentKey = seed.textureContentKey;
		metadata.glowmapContentKey = seed.glowmapContentKey;
		metadata.normalContentKey = seed.normalContentKey;
		metadata.metallicContentKey = seed.metallicContentKey;
		metadata.roughnessContentKey = seed.roughnessContentKey;
		metadata.textureId = seed.textureId;
		metadata.baseTextureId = seed.baseTextureId;
		metadata.textureIndex = seed.textureIndex;
		metadata.glowmapTextureIndex = seed.glowmapTextureIndex;
		metadata.normalTextureIndex = seed.normalTextureIndex;
		metadata.metallicTextureIndex = seed.metallicTextureIndex;
		metadata.roughnessTextureIndex = seed.roughnessTextureIndex;
		metadata.emissiveTextureIndex = seed.emissiveTextureIndex;
		metadata.paletteIndex = seed.paletteIndex;
		metadata.materialFlags = seed.materialFlags;
		metadata.lightingFlags = seed.lightingFlags;
		metadata.materialClass = seed.materialClass;
		metadata.emissiveMode = seed.emissiveMode;
		metadata.emissiveStableFrames = seed.emissiveStableFrames;
		metadata.sourceType = (nri_scene::SurfaceSourceType)seed.sourceType;
		metadata.sectorIndex = seed.sectorIndex;
		metadata.actorIndex = seed.actorIndex;
		metadata.actorOverlayRuleCount = std::min<uint32_t>(
			seed.actorOverlayRuleCount,
			(uint32_t)std::size(metadata.actorOverlayRuleIds));
		std::copy_n(seed.actorOverlayRuleIds, metadata.actorOverlayRuleCount, metadata.actorOverlayRuleIds);
		metadata.shade = seed.shade;
		metadata.alpha = seed.alpha;
		metadata.lightLevel = seed.lightLevel;
		std::copy(std::begin(seed.averageColor), std::end(seed.averageColor), std::begin(metadata.averageColor));
		std::copy(std::begin(seed.glowColor), std::end(seed.glowColor), std::begin(metadata.glowColor));
		std::copy(std::begin(seed.emissiveColor), std::end(seed.emissiveColor), std::begin(metadata.emissiveColor));
		metadata.emissiveIntensity = seed.emissiveIntensity;
		metadata.emissiveMaskScale = seed.emissiveMaskScale;
		metadata.visibleFullbrightBoost = seed.visibleFullbrightBoost;
		return metadata;
	}
}

class NRIPersistentVoxelServiceFactory
{
public:
	static void ResetMaterialClosure(NRIRenderer& renderer, uint64_t buildSerial)
	{
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		state = {};
		state.buildSerial = buildSerial;
		state.telemetry.buildSerial = buildSerial;
		state.registry.Reset(buildSerial);
	}

	static void RecordMaterialBuild(NRIRenderer& renderer)
	{
		EnsurePersistentVoxelMaterialClosureBuildSerial(renderer, renderer.mMapWorld.buildSerial);
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		const bool preloadPending =
			renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->IsPathTracingLevelPreloadPending();
		if (preloadPending)
		{
			state.telemetry.preloadRuleClassifications++;
		}
		else
		{
			state.telemetry.runtimeBroadMaterialBuilds++;
		}
	}

	static void BuildMaterialsTracked(
		NRIRenderer& renderer,
		nri_scene::SceneView& sceneView,
		nri_scene::MaterialBridgeData& materials,
		const char* label,
		bool clockMaterialBuild)
	{
		RecordMaterialBuild(renderer);
		if (clockMaterialBuild)
		{
			Clocker materialClock(NriPTMaterialBuild);
			renderer.BuildMaterialsWithActorOverrides(sceneView, materials, label);
			return;
		}
		renderer.BuildMaterialsWithActorOverrides(sceneView, materials, label);
	}

	static void RecordTextureClosure(
		NRIRenderer& renderer,
		const NRISceneTextureClosureResult& result)
	{
		EnsurePersistentVoxelMaterialClosureBuildSerial(renderer, renderer.mMapWorld.buildSerial);
		NRIPersistentVoxelMaterialClosureTelemetry& telemetry =
			GetPersistentVoxelMaterialClosureState(renderer).telemetry;
		telemetry.textureRequests++;
		if (result.realized)
		{
			telemetry.textureRealizations++;
			telemetry.textureRealizedBytes += result.estimatedBytes;
			telemetry.textureRealizeMs += result.realizeMs;
		}
		else if (result.reused)
		{
			telemetry.textureReuses++;
		}
		if (result.state == NRISceneTextureClosureState::Deferred)
		{
			telemetry.textureDeferred++;
		}
		else if (result.state == NRISceneTextureClosureState::Failed)
		{
			telemetry.textureFailures++;
		}
	}

	static bool PrewarmTextureTracked(
		NRIRenderer& renderer,
		const nri_scene::TextureUpload& upload,
		double* outRealizeMs = nullptr)
	{
		if (renderer.mFrameBuffer == nullptr)
		{
			return false;
		}
		NRISceneTextureClosureResult result = {};
		const bool ready = renderer.mSceneTextures.EnsurePreloadClosure(*renderer.mFrameBuffer, upload, result);
		RecordTextureClosure(renderer, result);
		if (outRealizeMs != nullptr)
		{
			*outRealizeMs += result.realizeMs;
		}
		return ready && result.IsReady();
	}

	static bool RegisterMaterialClosure(
		NRIRenderer& renderer,
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		const nri_scene::MaterialBridgeData& materials,
		NRIPersistentVoxelMaterialClosureSource source,
		NRIPersistentVoxelMaterialClosureResult& outResult)
	{
		EnsurePersistentVoxelMaterialClosureBuildSerial(renderer, buildSerial);
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		NRIPersistentVoxelMaterialSeed seed = {};
		seed.materialKey = materialKey;
		seed.validatedSignature = validatedSignature;
		seed.paletteWidth = materials.paletteWidth;
		seed.paletteHeight = materials.paletteHeight;
		seed.paletteSignature = HashPersistentVoxelClosureBytes(
			materials.paletteLookup.empty() ? nullptr : materials.paletteLookup.data(),
			materials.paletteLookup.size());
		seed.materials.reserve(materials.materials.size());
		for (const nri_scene::MaterialData& material : materials.materials)
		{
			seed.materials.push_back(BuildPersistentVoxelMaterialRowSeed(material));
		}
		seed.lighting.reserve(materials.lightMetadata.size());
		for (const nri_scene::MaterialLightingMetadata& metadata : materials.lightMetadata)
		{
			seed.lighting.push_back(BuildPersistentVoxelMaterialLightingSeed(metadata));
			seed.actorSensitive =
				seed.actorSensitive || metadata.actorIndex >= 0 || metadata.actorOverlayRuleCount != 0;
		}

		if (renderer.mFrameBuffer == nullptr)
		{
			outResult = BuildNRIPersistentVoxelMaterialClosureResult(seed);
			outResult.state = NRIPersistentVoxelMaterialClosureState::Failed;
			return false;
		}
		seed.textures.reserve(materials.textures.size());
		for (uint32_t textureIndex = 0; textureIndex < materials.textures.size(); ++textureIndex)
		{
			const nri_scene::TextureUpload& upload = materials.textures[textureIndex];
			NRISceneTextureClosureResult textureResult = {};
			renderer.mSceneTextures.EnsurePreloadClosure(*renderer.mFrameBuffer, upload, textureResult);
			RecordTextureClosure(renderer, textureResult);
			NRIPersistentVoxelTextureDependency dependency = {};
			dependency.key = upload.key;
			dependency.estimatedBytes = textureResult.estimatedBytes;
			dependency.materialTextureIndex = textureIndex;
			dependency.residencyIndex = textureResult.residencyIndex;
			dependency.width = upload.width;
			dependency.height = upload.height;
			dependency.state = ConvertPersistentVoxelTextureClosureState(textureResult.state);
			dependency.failure = ConvertPersistentVoxelTextureClosureFailure(textureResult.failure);
			dependency.indexed = upload.indexed;
			dependency.descriptorReady = textureResult.descriptorReady;
			seed.textures.push_back(dependency);
		}

		seed.ruleResultId = HashNRIPersistentVoxelMaterialRuleResult(seed);
		seed.payloadSignature = HashNRIPersistentVoxelMaterialSeed(seed);
		if (!materials.paletteLookup.empty() && seed.paletteSignature != 0)
		{
			PersistentVoxelPalettePayload& palette = state.palettes[seed.paletteSignature];
			palette.width = materials.paletteWidth;
			palette.height = materials.paletteHeight;
			palette.bytes = materials.paletteLookup;
		}
		const bool ready = state.registry.Register(std::move(seed), outResult);
		if (source == NRIPersistentVoxelMaterialClosureSource::PreloadKnown)
		{
			state.telemetry.preloadSeeds++;
			if (outResult.actorSensitive)
			{
				state.telemetry.preloadActorSensitiveSeeds++;
			}
			if (outResult.state == NRIPersistentVoxelMaterialClosureState::Ready)
			{
				state.telemetry.preloadSeedsReady++;
			}
			else if (outResult.state == NRIPersistentVoxelMaterialClosureState::Failed)
			{
				state.telemetry.preloadSeedsFailed++;
			}
			else
			{
				state.telemetry.preloadSeedsDeferred++;
			}
			if (outResult.reusedSeed)
			{
				state.telemetry.preloadSeedReuses++;
			}
		}
		return ready;
	}

	static bool TryReuseMaterialClosure(
		NRIRenderer& renderer,
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		NRIPersistentVoxelMaterialClosureSource source,
		nri_scene::MaterialBridgeData& outMaterials,
		NRIPersistentVoxelMaterialClosureResult& outResult)
	{
		EnsurePersistentVoxelMaterialClosureBuildSerial(renderer, buildSerial);
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		const NRIPersistentVoxelMaterialSeed* seed =
			state.registry.FindReady(buildSerial, materialKey, validatedSignature, outResult);
		if (seed == nullptr)
		{
			return false;
		}

		for (size_t textureIndex = 0; textureIndex < seed->textures.size(); ++textureIndex)
		{
			const NRIPersistentVoxelTextureDependency& dependency = seed->textures[textureIndex];
			if (dependency.state == NRIPersistentVoxelTextureClosureState::NotRequired)
			{
				continue;
			}
			NRISceneTextureClosureResult textureResult = {};
			if (!renderer.mSceneTextures.QueryPreloadClosure(dependency.key, textureResult))
			{
				outResult.state = NRIPersistentVoxelMaterialClosureState::Stale;
				outResult.textures[textureIndex].state = NRIPersistentVoxelTextureClosureState::Failed;
				outResult.textures[textureIndex].failure =
					ConvertPersistentVoxelTextureClosureFailure(textureResult.failure);
				outResult.textures[textureIndex].descriptorReady = false;
				state.telemetry.textureFailures++;
				return false;
			}
			outResult.textures[textureIndex].residencyIndex = textureResult.residencyIndex;
			outResult.textures[textureIndex].descriptorReady = textureResult.descriptorReady;
			state.telemetry.textureRequests++;
			state.telemetry.textureReuses++;
		}

		outMaterials = {};
		outMaterials.materials.reserve(seed->materials.size());
		for (const NRIPersistentVoxelMaterialRowSeed& material : seed->materials)
		{
			outMaterials.materials.push_back(BuildPersistentVoxelMaterialData(material));
		}
		outMaterials.lightMetadata.reserve(seed->lighting.size());
		for (const NRIPersistentVoxelMaterialLightingSeed& metadata : seed->lighting)
		{
			outMaterials.lightMetadata.push_back(BuildPersistentVoxelMaterialLightingMetadata(metadata));
		}
		outMaterials.textures.resize(seed->textures.size());
		for (const NRIPersistentVoxelTextureDependency& dependency : seed->textures)
		{
			if (dependency.materialTextureIndex >= outMaterials.textures.size())
			{
				outMaterials.textures.resize((size_t)dependency.materialTextureIndex + 1u);
			}
			nri_scene::TextureUpload& upload = outMaterials.textures[dependency.materialTextureIndex];
			upload.key = dependency.key;
			upload.width = dependency.width;
			upload.height = dependency.height;
			upload.indexed = dependency.indexed;
		}
		const auto palette = state.palettes.find(seed->paletteSignature);
		if (palette != state.palettes.end())
		{
			outMaterials.paletteWidth = palette->second.width;
			outMaterials.paletteHeight = palette->second.height;
			outMaterials.paletteLookup = palette->second.bytes;
		}
		else
		{
			outMaterials.paletteWidth = seed->paletteWidth;
			outMaterials.paletteHeight = seed->paletteHeight;
		}

		if (source == NRIPersistentVoxelMaterialClosureSource::PreloadKnown)
		{
			state.telemetry.preloadSeedReuses++;
		}
		else
		{
			state.telemetry.runtimeSeedReuses++;
		}
		return true;
	}

	static NRIPersistentVoxelMaterialClosureTelemetry GetMaterialClosureTelemetry(NRIRenderer& renderer)
	{
		PersistentVoxelMaterialClosureServiceState& state = GetPersistentVoxelMaterialClosureState(renderer);
		state.telemetry.registry = state.registry.Stats();
		return state.telemetry;
	}

	static NRIPersistentVoxelMaterialClosureServices BuildMaterialClosureServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelMaterialClosureServices services = {};
		services.user = &renderer;
		services.registerClosure = [](
			void* user,
			uint64_t buildSerial,
			uint64_t materialKey,
			uint64_t validatedSignature,
			const nri_scene::MaterialBridgeData& materials,
			NRIPersistentVoxelMaterialClosureSource source,
			NRIPersistentVoxelMaterialClosureResult& outResult) -> bool
		{
			return RegisterMaterialClosure(
				*static_cast<NRIRenderer*>(user),
				buildSerial,
				materialKey,
				validatedSignature,
				materials,
				source,
				outResult);
		};
		services.tryReuse = [](
			void* user,
			uint64_t buildSerial,
			uint64_t materialKey,
			uint64_t validatedSignature,
			NRIPersistentVoxelMaterialClosureSource source,
			nri_scene::MaterialBridgeData& outMaterials,
			NRIPersistentVoxelMaterialClosureResult& outResult) -> bool
		{
			return TryReuseMaterialClosure(
				*static_cast<NRIRenderer*>(user),
				buildSerial,
				materialKey,
				validatedSignature,
				source,
				outMaterials,
				outResult);
		};
		return services;
	}

	static NRIPersistentVoxelResetServices BuildResetServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelResetServices services = {};
		services.user = &renderer;
		services.retireBuffer = [](void* user, NRIBufferResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
		};
		services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		services.invalidateSceneDataDescriptors = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->SetCurrentSceneDataDescriptorsInitialized(false);
		};
		return services;
	}

	static NRIPersistentVoxelAdmissionServices BuildAdmissionServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelAdmissionServices services = {};
		services.user = &renderer;
		services.materialClosure = BuildMaterialClosureServices(renderer);
		services.admitVariantResource = [](
			void* user,
			PersistentVoxelAdmissionEntry& entry,
			uint64_t byteBudget,
			uint32_t& blasBudget,
			uint64_t& outUploadBytes,
			bool& outReusedMesh,
			bool& outReusedMaterial,
			bool& outInProgress,
			bool isolateBlasBuild,
			const char*& outFailureReason,
			PersistentVoxelAdmissionStats* outStats) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mPersistentVoxels.AdmitVariantResource(
				entry,
				byteBudget,
				blasBudget,
				outUploadBytes,
				outReusedMesh,
				outReusedMaterial,
				outInProgress,
				isolateBlasBuild,
				outFailureReason,
				outStats,
				renderer->mFrameIndex,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildAdmissionServices(*renderer));
		};
		services.submitWaitAndRestart = [](void* user, const char* reason) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (renderer->mFrameBuffer == nullptr || !renderer->mFrameBuffer->SubmitWaitAndRestartCommandList(reason))
			{
				return false;
			}
			renderer->ResetResidentUploadScratchFrame(reason);
			return true;
		};
		services.isSubmitBudgetHit = [](void* user) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer != nullptr && renderer->mFrameBuffer->IsPreloadSubmitBudgetHit();
		};
		services.getRecordingCommandFenceValue = [](void* user) -> uint64_t
		{
			return static_cast<NRIRenderer*>(user)->GetRecordingCommandFenceValue();
		};
		services.isCommandFenceValueComplete = [](void* user, uint64_t fenceValue) -> bool
		{
			return static_cast<NRIRenderer*>(user)->IsCommandFenceValueComplete(fenceValue);
		};
		services.retireBuffer = [](void* user, NRIBufferResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentBufferResource(resource);
		};
		services.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		services.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
		{
			NRIRenderer& renderer = *static_cast<NRIRenderer*>(user);
			BuildMaterialsTracked(renderer, sceneView, materials, label, true);
		};
		services.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload) -> bool
		{
			return PrewarmTextureTracked(*static_cast<NRIRenderer*>(user), upload);
		};
		services.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
		};
		services.createStructuredBufferNoUpload = [](void* user, NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (renderer->mFrameBuffer == nullptr ||
				!renderer->CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE))
			{
				return false;
			}
			nri::BufferViewDesc viewDesc = {};
			viewDesc.buffer = resource.buffer;
			viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
			viewDesc.offset = 0;
			viewDesc.size = nri::WHOLE_SIZE;
			viewDesc.structureStride = stride;
			if (renderer->mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
			{
				renderer->DestroyBufferResource(resource);
				return false;
			}
			resource.usedSize = size;
			return true;
		};
		services.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
		};
		services.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
		};
		services.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
		};
		services.buildBottomLevel = [](
			void* user,
			const NRIBufferResource& vertexBuffer,
			const NRIBufferResource& indexBuffer,
			uint32_t vertexOffset,
			uint32_t vertexCount,
			uint32_t indexOffset,
			uint32_t indexCount,
			uint32_t primitiveCount,
			NRIAccelerationStructureResource& outAccelerationStructure,
			NRIBufferResource* buildScratchBuffer) -> bool
		{
			return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
				vertexBuffer,
				indexBuffer,
				vertexOffset,
				vertexCount,
				indexOffset,
				indexCount,
				primitiveCount,
				outAccelerationStructure,
				false,
				buildScratchBuffer);
		};
		services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		services.barrierComputeToBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierComputeToBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		return services;
	}

	static NRIPersistentVoxelAccelerationServices BuildAccelerationServices(NRIRenderer& renderer)
	{
		NRIPersistentVoxelAccelerationServices services = {};
		services.user = &renderer;
		services.buildBottomLevel = [](
			void* user,
			const NRIBufferResource& vertexBuffer,
			const NRIBufferResource& indexBuffer,
			uint32_t vertexOffset,
			uint32_t vertexCount,
			uint32_t indexOffset,
			uint32_t indexCount,
			uint32_t primitiveCount,
			NRIAccelerationStructureResource& outAccelerationStructure) -> bool
		{
			return static_cast<NRIRenderer*>(user)->BuildBottomLevelAccelerationStructure(
				vertexBuffer,
				indexBuffer,
				vertexOffset,
				vertexCount,
				indexOffset,
				indexCount,
				primitiveCount,
				outAccelerationStructure,
				false);
		};
		services.barrierBuildInputs = [](void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) -> bool
		{
			return BarrierBuildInputs(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), vertexBuffer, indexBuffer);
		};
		return services;
	}

	static bool PreloadResources(NRIRenderer& renderer)
	{
		EnsurePersistentVoxelMaterialClosureBuildSerial(renderer, renderer.mMapWorld.buildSerial);
		std::vector<nri_scene::PrecachedVoxelVariantView> variants;
		std::vector<nri_scene::PrecachedVoxelRawManifestView> rawVariants;
		nri_scene::PrecachedVoxelRawManifestStats rawManifestStats = {};
		std::vector<nri_scene::PersistentVoxelCacheEntryView> cacheEntries;
		const bool gpuLoadingEnabled = (bool)nri_ptloadingvoxelgpu;
		bool hasCacheEntries = false;
		nri_scene::PreloadLiveActorVoxelRawSources();
		const NRIVoxelComputePreloadSettings computePreloadSettings = BuildNRIVoxelComputePreloadSettingsFromCVars();
		const bool computePreloadPlanningEnabled =
			computePreloadSettings.enabled || computePreloadSettings.traceLevel >= 1 || (int)nri_ptloadingtrace >= 1;
		if (computePreloadPlanningEnabled)
		{
			nri_scene::BuildPrecachedVoxelRawManifestViews(rawVariants, &rawManifestStats);
		}
		if (gpuLoadingEnabled)
		{
			nri_scene::BuildPrecachedVoxelVariantViews(variants);
			hasCacheEntries = nri_scene::BuildPersistentVoxelCacheEntries(cacheEntries);
		}

		struct ComputePreloadTimeline
		{
			uint64_t buildSerial = 0;
			uint32_t maxRawVariants = 0;
			uint32_t maxLegacyVariants = 0;
		};
		static ComputePreloadTimeline sComputePreloadTimeline = {};
		const bool newTimeline = sComputePreloadTimeline.buildSerial != renderer.mMapWorld.buildSerial;
		if (newTimeline)
		{
			sComputePreloadTimeline = {};
			sComputePreloadTimeline.buildSerial = renderer.mMapWorld.buildSerial;
		}
		const bool improvedTimeline =
			(uint32_t)rawVariants.size() > sComputePreloadTimeline.maxRawVariants ||
			(uint32_t)variants.size() > sComputePreloadTimeline.maxLegacyVariants;
		const bool shouldPlanComputePreload =
			computePreloadPlanningEnabled &&
			(newTimeline || improvedTimeline || computePreloadSettings.traceLevel >= 2);
		NRIVoxelComputePreloadStats computePreloadStats = GetLastNRIVoxelComputePreloadStats();
		if (shouldPlanComputePreload)
		{
			const char* timelineStage = newTimeline ? "first" : (improvedTimeline ? "max" : "progress");
			const NRIRenderer::MemoryTelemetry memoryTelemetry = renderer.GetMemoryTelemetry();
			const NRIAdapterMemoryTelemetry adapterMemory = renderer.mFrameBuffer != nullptr ?
				renderer.mFrameBuffer->GetAdapterMemoryTelemetry() : NRIAdapterMemoryTelemetry{};
			computePreloadStats = PlanNRIVoxelComputePreload(
				variants,
				rawVariants,
				rawManifestStats,
				renderer.mPersistentVoxels,
				computePreloadSettings,
				renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
				renderer.mMapWorld.buildSerial,
				renderer.mFrameIndex,
				timelineStage,
				std::max(memoryTelemetry.totalTrackedBytes, adapterMemory.localUsageBytes),
				adapterMemory.localBudgetBytes);
			sComputePreloadTimeline.maxRawVariants = std::max(sComputePreloadTimeline.maxRawVariants, (uint32_t)rawVariants.size());
			sComputePreloadTimeline.maxLegacyVariants = std::max(sComputePreloadTimeline.maxLegacyVariants, (uint32_t)variants.size());
		}
		if (computePreloadSettings.enabled && !computePreloadSettings.dryRun && !computePreloadStats.memoryGuardHit)
		{
			std::vector<nri_scene::PrecachedVoxelVariantView> directPreloadVariants;
			BuildNRIVoxelComputePreloadDirectVariants(rawVariants, computePreloadSettings, directPreloadVariants);
			if (!directPreloadVariants.empty())
			{
				if ((int)nri_ptloadingtrace >= 1 || computePreloadSettings.traceLevel >= 1 || (int)nri_ptvoxelcomputetrace >= 1)
				{
					Printf("NRI PT voxel compute preload: event=admit-source level=%s build_serial=%llu frame=%u direct_variants=%u legacy_variants=%u dry_run=0\n",
						renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "unknown",
						(unsigned long long)renderer.mMapWorld.buildSerial,
						renderer.mFrameIndex,
						(uint32_t)directPreloadVariants.size(),
						(uint32_t)variants.size());
				}
				std::vector<nri_scene::PrecachedVoxelVariantView> mergedVariants;
				mergedVariants.reserve(directPreloadVariants.size() + variants.size());
				mergedVariants.insert(mergedVariants.end(), directPreloadVariants.begin(), directPreloadVariants.end());
				mergedVariants.insert(mergedVariants.end(), variants.begin(), variants.end());
				variants = std::move(mergedVariants);
			}
		}
		else if (computePreloadSettings.enabled && !computePreloadSettings.dryRun && computePreloadStats.memoryGuardHit)
		{
			if ((int)nri_ptloadingtrace >= 1 || computePreloadSettings.traceLevel >= 1)
			{
				Printf("NRI PT voxel compute preload: event=memory-guard result=abort level=%s build_serial=%llu estimated_peak_total_bytes=%llu local_budget_bytes=%llu minimum_reserve_bytes=%llu\n",
					renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "unknown",
					(unsigned long long)renderer.mMapWorld.buildSerial,
					(unsigned long long)computePreloadStats.estimatedPeakTotalBytes,
					(unsigned long long)computePreloadStats.localMemoryBudgetBytes,
					(unsigned long long)computePreloadStats.minimumLocalMemoryReserveBytes);
			}
			variants.clear();
			cacheEntries.clear();
			hasCacheEntries = false;
		}

		const NRIPersistentVoxelSettings persistentVoxelSettings = BuildNRIPersistentVoxelSettingsFromCVars();
		if (gpuLoadingEnabled)
		{
			renderer.ResetResidentUploadScratchFrame("voxel-preload-start");
		}
		NRIPersistentVoxelPreloadServices preloadServices = {};
		preloadServices.user = &renderer;
		preloadServices.materialClosure = BuildMaterialClosureServices(renderer);
		preloadServices.pumpAdmissionQueue = [](void* user, const char* phase) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			const NRIPersistentVoxelSettings settings = BuildNRIPersistentVoxelSettingsFromCVars();
			const NRIRenderer::MemoryTelemetry telemetry = renderer->GetMemoryTelemetry();
			return renderer->mPersistentVoxels.PumpAdmissionQueue(
				phase,
				renderer->mMapWorld.buildSerial,
				renderer->mFrameIndex,
				settings,
				telemetry.totalTrackedBytes,
				renderer->mFrameBuffer != nullptr ? renderer->mFrameBuffer->GetAdapterLocalBudgetBytes() : 0ull,
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildResetServices(*renderer),
				BuildAdmissionServices(*renderer));
		};
		preloadServices.pumpComputeJobs = [](void* user, uint32_t frameIndex) -> void
		{
			DispatchNRIVoxelComputeMeshingDiagnostics(*static_cast<NRIRenderer*>(user), frameIndex);
		};
		preloadServices.ensureBatch = [](void* user, NRIPersistentVoxelBatchStats* outStats) -> bool
		{
			return EnsureBatch(static_cast<NRIRenderer&>(*static_cast<NRIRenderer*>(user)), outStats);
		};
		preloadServices.warmSharedBlas = [](void* user, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants, uint32_t frameIndex) -> bool
		{
			NRIRenderer& renderer = *static_cast<NRIRenderer*>(user);
			if (renderer.mPersistentVoxels.HasRenderableOverlay())
			{
				NRIPersistentVoxelAccelerationBuildStats batchAccelerationStats = {};
				if (!renderer.mPersistentVoxels.BuildAccelerationStructures(
						frameIndex,
						BuildNRIPersistentVoxelSettingsFromCVars(),
						(bool)nri_voxelstats,
						BuildResetServices(renderer),
						BuildAccelerationServices(renderer),
						batchAccelerationStats))
				{
					return false;
				}
				if ((int)nri_ptloadingtrace >= 1)
				{
					Printf("NRI PT loading voxel acceleration: event=warmup active_actors=%u calls=%u builds=%u unique_mesh_builds=%u\n",
						batchAccelerationStats.instances,
						batchAccelerationStats.calls,
						batchAccelerationStats.builds,
						batchAccelerationStats.uniqueMeshBuilds);
				}
			}
			return renderer.mPersistentVoxels.WarmSharedBlasForLoading(
				variants,
				frameIndex,
				BuildNRIPersistentVoxelSettingsFromCVars(),
				(int)nri_ptloadingtrace,
				(bool)nri_voxelstats,
				BuildResetServices(renderer),
				BuildAccelerationServices(renderer));
		};
		preloadServices.isSubmitBudgetHit = [](void* user) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer != nullptr && renderer->mFrameBuffer->IsPreloadSubmitBudgetHit();
		};
		preloadServices.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
		{
			NRIRenderer& renderer = *static_cast<NRIRenderer*>(user);
			BuildMaterialsTracked(renderer, sceneView, materials, label, true);
		};
		preloadServices.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload) -> bool
		{
			return PrewarmTextureTracked(*static_cast<NRIRenderer*>(user), upload);
		};
		const bool preloadReady = renderer.mPersistentVoxels.PreloadResources(
			variants,
			cacheEntries,
			hasCacheEntries,
			gpuLoadingEnabled,
			computePreloadSettings.enabled && !computePreloadSettings.dryRun && computePreloadSettings.preloadMaterials,
			computePreloadSettings.maxMaterialRows,
			renderer.mMapWorld.buildSerial,
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
			renderer.mFrameIndex,
			persistentVoxelSettings,
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			BuildResetServices(renderer),
			preloadServices);
		if ((int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats || (int)nri_ptvoxelcomputetrace >= 1)
		{
			const NRIPersistentVoxelMaterialClosureTelemetry telemetry = GetMaterialClosureTelemetry(renderer);
			Printf(
				"PERF pt voxel material closure NRI: build_serial=%llu frame=%u preload_classify=%u preload_seeds=%u preload_seed_ready=%u preload_seed_deferred=%u preload_seed_failed=%u preload_actor_scoped=%u preload_seed_reuse=%u runtime_seed_reuse=%u runtime_broad_builds=%u texture_requests=%u texture_realize=%u texture_reuse=%u texture_deferred=%u texture_failed=%u texture_realized_bytes=%llu texture_realize_ms=%.3f registry_ready=%u registry_deferred=%u registry_failed=%u registry_hits=%u registry_misses=%u preload_ready=%u\n",
				(unsigned long long)telemetry.buildSerial,
				renderer.mFrameIndex,
				telemetry.preloadRuleClassifications,
				telemetry.preloadSeeds,
				telemetry.preloadSeedsReady,
				telemetry.preloadSeedsDeferred,
				telemetry.preloadSeedsFailed,
				telemetry.preloadActorSensitiveSeeds,
				telemetry.preloadSeedReuses,
				telemetry.runtimeSeedReuses,
				telemetry.runtimeBroadMaterialBuilds,
				telemetry.textureRequests,
				telemetry.textureRealizations,
				telemetry.textureReuses,
				telemetry.textureDeferred,
				telemetry.textureFailures,
				(unsigned long long)telemetry.textureRealizedBytes,
				telemetry.textureRealizeMs,
				telemetry.registry.readySeeds,
				telemetry.registry.deferredSeeds,
				telemetry.registry.failedSeeds,
				telemetry.registry.lookupHits,
				telemetry.registry.lookupMisses,
				preloadReady ? 1u : 0u);
		}
		return preloadReady;
	}

	static bool EnsureBatch(NRIRenderer& renderer, NRIPersistentVoxelBatchStats* outStats = nullptr)
	{
		NRIPersistentVoxelBatchServices batchServices = {};
		batchServices.user = &renderer;
		batchServices.materialClosure = BuildMaterialClosureServices(renderer);
		batchServices.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
		{
			NRIRenderer& renderer = *static_cast<NRIRenderer*>(user);
			BuildMaterialsTracked(renderer, sceneView, materials, label, false);
		};
		batchServices.isTextureCached = [](void* user, const nri_scene::TextureUpload& upload) -> bool
		{
			return static_cast<NRIRenderer*>(user)->FindSceneTextureCacheIndex(upload.key) != UINT32_MAX;
		};
		batchServices.prewarmTexture = [](void* user, const nri_scene::TextureUpload& upload, double* outMs) -> bool
		{
			return PrewarmTextureTracked(*static_cast<NRIRenderer*>(user), upload, outMs);
		};
		batchServices.assignGeometryPortalIndices = [](void* user, nri_scene::GeometryData& geometry)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
		};
		batchServices.ensureStructuredBuffer = [](void* user, NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			return renderer->EnsureResidentStructuredBuffer(resource, *stats, data, size, stride, usage, after, reason, uploadKind);
		};
		batchServices.ensureArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(resource, requiredSize, stride, usage, after);
		};
		batchServices.stageBufferCopyRange = [](void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->StageResidentBufferCopyRange(resource, byteOffset, data, size, after, uploadKind);
		};
		batchServices.noteBufferUpload = [](void* user, int uploadKind, uint64_t size, const char* reason)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			SceneBufferDebugStats* stats =
				uploadKind == ResidentUploadKind_Index ? &renderer->mIndexBufferStats :
				(uploadKind == ResidentUploadKind_Primitive ? &renderer->mPrimitiveBufferStats : &renderer->mVertexBufferStats);
			renderer->NotePerfBufferUpload(stats, size, false, reason, uploadKind);
		};
		batchServices.retireAccelerationStructure = [](void* user, NRIAccelerationStructureResource& resource)
		{
			static_cast<NRIRenderer*>(user)->RetireResidentAccelerationStructure(resource);
		};
		batchServices.materialWouldEmit = [](void* user, const nri_scene::MaterialLightingMetadata& metadata) -> bool
		{
			return static_cast<NRIRenderer*>(user)->mSceneLights.MaterialWouldEmit(metadata);
		};
		batchServices.buildSurfaceRecord = [](void* user, const nri_scene::SurfaceRef& surface, const nri_scene::MaterialBridgeData& materials, SceneLightRecordSource source, uint32_t materialIndex, uint32_t primitiveIndex) -> SceneLightSystem::SurfaceRecord
		{
			return static_cast<NRIRenderer*>(user)->mSceneLights.BuildSurfaceRecord(surface, materials, source, materialIndex, primitiveIndex);
		};

		NRIPersistentVoxelBatchStats batchStats = {};
		const bool result = renderer.mPersistentVoxels.EnsureBatch(
			renderer.mMapWorld.buildSerial,
			renderer.mFrameIndex,
			BuildNRIPersistentVoxelSettingsFromCVars(),
			(int)nri_ptloadingtrace,
			(bool)nri_voxelstats,
			BuildResetServices(renderer),
			batchServices,
			batchStats);
		if (outStats != nullptr)
		{
			*outStats = batchStats;
		}

		renderer.mLastPerfShellTraceStats.persistentVoxelBatchCacheEntryMs += batchStats.persistentVoxelBatchCacheEntryMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchSortMs += batchStats.persistentVoxelBatchSortMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchInstanceSyncMs += batchStats.persistentVoxelBatchInstanceSyncMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchExistingActorMapMs += batchStats.persistentVoxelBatchExistingActorMapMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchActorLoopMs += batchStats.persistentVoxelBatchActorLoopMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMaterialVariantMs += batchStats.persistentVoxelBatchMaterialVariantMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMeshAdmissionMs += batchStats.persistentVoxelBatchMeshAdmissionMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchMaterialBridgeMs += batchStats.persistentVoxelBatchMaterialBridgeMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelBatchStateMs += batchStats.persistentVoxelBatchStateMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantMs += batchStats.geometryBuildPersistentVoxelVariantMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelAppendMs += batchStats.geometryBuildPersistentVoxelAppendMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelRebuildMs += batchStats.geometryBuildPersistentVoxelRebuildMs;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMs += batchStats.persistentVoxelTexturePrewarmMs;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantCalls += batchStats.geometryBuildPersistentVoxelVariantCalls;
		renderer.mLastPerfShellTraceStats.geometryBuildPersistentVoxelVariantPrimitives += batchStats.geometryBuildPersistentVoxelVariantPrimitives;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmHitCount += batchStats.persistentVoxelTexturePrewarmHitCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmQueuedCount += batchStats.persistentVoxelTexturePrewarmQueuedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmMissCount += batchStats.persistentVoxelTexturePrewarmMissCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredCount += batchStats.persistentVoxelTexturePrewarmDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedCount += batchStats.persistentVoxelTexturePrewarmProcessedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmByteBudget = batchStats.persistentVoxelTexturePrewarmByteBudget;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmEstimatedBytes += batchStats.persistentVoxelTexturePrewarmEstimatedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmDeferredBytes += batchStats.persistentVoxelTexturePrewarmDeferredBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelTexturePrewarmProcessedBytes += batchStats.persistentVoxelTexturePrewarmProcessedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingCandidateCount += batchStats.persistentVoxelOnboardingCandidateCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredCount += batchStats.persistentVoxelOnboardingDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingPrimitiveBudgetHits += batchStats.persistentVoxelOnboardingPrimitiveBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudgetHits += batchStats.persistentVoxelOnboardingByteBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingActorBudgetHits += batchStats.persistentVoxelOnboardingActorBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedCount += batchStats.persistentVoxelOnboardingAdmittedCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingTextureBudgetHits += batchStats.persistentVoxelOnboardingTextureBudgetHits;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmissionPendingCount += batchStats.persistentVoxelOnboardingAdmissionPendingCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingTexturePrewarmDeferredCount += batchStats.persistentVoxelOnboardingTexturePrewarmDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingMaterialInvalidCount += batchStats.persistentVoxelOnboardingMaterialInvalidCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingBudgetDeferredCount += batchStats.persistentVoxelOnboardingBudgetDeferredCount;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingEstimatedBytes += batchStats.persistentVoxelOnboardingEstimatedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingDeferredBytes += batchStats.persistentVoxelOnboardingDeferredBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingAdmittedBytes += batchStats.persistentVoxelOnboardingAdmittedBytes;
		renderer.mLastPerfShellTraceStats.persistentVoxelOnboardingByteBudget = batchStats.persistentVoxelOnboardingByteBudget;
		renderer.mLastPerfShellTraceStats.persistentVoxelInstanceTransformUpdates += batchStats.persistentVoxelInstanceTransformUpdates;
		return result;
	}

private:
	static bool BarrierBuildInputs(NRIRenderer& renderer, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer)
	{
		if (renderer.mFrameBuffer == nullptr || renderer.mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[0].after = NRIResourceComputeShaderResourceAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[1].after = NRIResourceComputeShaderResourceAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	}

	static bool BarrierComputeToBuildInputs(NRIRenderer& renderer, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer)
	{
		if (renderer.mFrameBuffer == nullptr || renderer.mFrameBuffer->mCommandBuffer == nullptr)
		{
			return false;
		}
		nri::BufferBarrierDesc inputBarriers[2] = {};
		inputBarriers[0].buffer = vertexBuffer.buffer;
		inputBarriers[0].before = NRIResourceComputeShaderResourceAccess();
		inputBarriers[0].after = NRIResourceAccelerationStructureBuildInputAccess();
		inputBarriers[1].buffer = indexBuffer.buffer;
		inputBarriers[1].before = NRIResourceComputeShaderResourceAccess();
		inputBarriers[1].after = NRIResourceAccelerationStructureBuildInputAccess();
		nri::BarrierDesc inputBarrierDesc = {};
		inputBarrierDesc.buffers = inputBarriers;
		inputBarrierDesc.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, inputBarrierDesc);
		return true;
	}
};

bool NRIRenderer::PreloadPersistentVoxelResources()
{
	return NRIPersistentVoxelServiceFactory::PreloadResources(*this);
}

bool NRIRenderer::EnsurePersistentVoxelBatch()
{
	return NRIPersistentVoxelServiceFactory::EnsureBatch(*this);
}

NRIPersistentVoxelResetServices BuildNRIPersistentVoxelResetServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildResetServices(renderer);
}

NRIPersistentVoxelAdmissionServices BuildNRIPersistentVoxelAdmissionServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildAdmissionServices(renderer);
}

NRIPersistentVoxelAccelerationServices BuildNRIPersistentVoxelAccelerationServices(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::BuildAccelerationServices(renderer);
}

void ResetNRIPersistentVoxelMaterialClosure(NRIRenderer& renderer, uint64_t buildSerial)
{
	NRIPersistentVoxelServiceFactory::ResetMaterialClosure(renderer, buildSerial);
}

bool RegisterNRIPersistentVoxelMaterialClosure(
	NRIRenderer& renderer,
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	const nri_scene::MaterialBridgeData& materials,
	NRIPersistentVoxelMaterialClosureSource source,
	NRIPersistentVoxelMaterialClosureResult& outResult)
{
	return NRIPersistentVoxelServiceFactory::RegisterMaterialClosure(
		renderer,
		buildSerial,
		materialKey,
		validatedSignature,
		materials,
		source,
		outResult);
}

bool TryReuseNRIPersistentVoxelMaterialClosure(
	NRIRenderer& renderer,
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	NRIPersistentVoxelMaterialClosureSource source,
	nri_scene::MaterialBridgeData& outMaterials,
	NRIPersistentVoxelMaterialClosureResult& outResult)
{
	return NRIPersistentVoxelServiceFactory::TryReuseMaterialClosure(
		renderer,
		buildSerial,
		materialKey,
		validatedSignature,
		source,
		outMaterials,
		outResult);
}

NRIPersistentVoxelMaterialClosureTelemetry GetNRIPersistentVoxelMaterialClosureTelemetry(NRIRenderer& renderer)
{
	return NRIPersistentVoxelServiceFactory::GetMaterialClosureTelemetry(renderer);
}
