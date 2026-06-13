#include "nri_renderer.h"

#include "../scene/nri_material_bridge.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "nri_shader_contracts.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

EXTERN_CVAR(Int, nri_ptactorspritetrace)
EXTERN_CVAR(Int, nri_ptdebug)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	bool ShouldTraceSceneTexturePerf()
	{
		return (int)perf_looptraceframes > 0 || ShouldEmitRendererTemporalTraceLogs();
	}

	bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	double SceneTextureDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	uint64_t SceneTextureHashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint64_t HashSceneTextureDescriptorList(const nri::Descriptor* const* descriptors, size_t count)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = SceneTextureHashCombine64(hash, (uint64_t)count);
		for (size_t i = 0; i < count; ++i)
		{
			hash = SceneTextureHashCombine64(hash, (uint64_t)(uintptr_t)descriptors[i]);
		}
		return hash;
	}

	MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall: return "draw_list_wall";
		case nri_scene::SurfaceSourceType::MirrorWall: return "mirror_wall";
		case nri_scene::SurfaceSourceType::FloorFlat: return "floor_flat";
		case nri_scene::SurfaceSourceType::CeilingFlat: return "ceiling_flat";
		case nri_scene::SurfaceSourceType::FacingSprite: return "facing_sprite";
		case nri_scene::SurfaceSourceType::VoxelProxySprite: return "voxel_proxy_sprite";
		case nri_scene::SurfaceSourceType::MapWallBand: return "map_wall_band";
		case nri_scene::SurfaceSourceType::MapFloorSection: return "map_floor_section";
		case nri_scene::SurfaceSourceType::MapCeilingSection: return "map_ceiling_section";
		case nri_scene::SurfaceSourceType::MapPortalSurface: return "map_portal_surface";
		case nri_scene::SurfaceSourceType::DebugSphere: return "debug_sphere";
		case nri_scene::SurfaceSourceType::SurfaceLightOverlay: return "surface_light_overlay";
		default: return "unknown";
		}
	}
}

bool NRISceneTextureResidency::EnsurePaletteTexture(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials)
{
	if (mPaletteTexture.texture != nullptr &&
		mPaletteTexture.width == materials.paletteWidth &&
		mPaletteTexture.height == materials.paletteHeight)
	{
		return true;
	}

	device.DestroyTextureResource(mPaletteTexture);
	if (!device.CreateOwnedTexture(mPaletteTexture, materials.paletteWidth, materials.paletteHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE))
	{
		return false;
	}

	return device.UploadTextureData(mPaletteTexture, materials.paletteLookup.data(), materials.paletteWidth, materials.paletteHeight, materials.paletteWidth * 4u);
}

bool NRISceneTextureResidency::EnsureCacheEntry(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, double* outRealizeMs)
{
	if (upload.width == 0 || upload.height == 0)
	{
		return true;
	}

	if (device.mActiveCanvasSourceTexture != nullptr &&
		upload.sourceTexture == device.mActiveCanvasSourceTexture)
	{
		return true;
	}

	if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
	{
		return true;
	}

	if (FindCacheIndex(upload.key) != UINT32_MAX)
	{
		return true;
	}

	const auto realizeStart = outRealizeMs != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	std::vector<uint8_t> realizedPixels;
	uint32_t realizedWidth = upload.width;
	uint32_t realizedHeight = upload.height;
	const uint8_t* pixelData = upload.pixels.data();
	if (upload.pixels.empty())
	{
		if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
		{
			return true;
		}
		pixelData = realizedPixels.data();
	}

	if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
	{
		return true;
	}

	NRISceneCachedTexture cacheEntry = {};
	cacheEntry.key = upload.key;
	const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
	if (!device.CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
		!device.UploadTextureData(cacheEntry.resource, pixelData, realizedWidth, realizedHeight, rowPitch))
	{
		return false;
	}

	AddCachedTexture(std::move(cacheEntry));
	if (outRealizeMs != nullptr)
	{
		*outRealizeMs += SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now());
	}
	return true;
}

bool NRISceneTextureResidency::ResolveTextureDescriptor(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, bool tracePerf, SceneTextureResolveResult& outResult)
{
	outResult = {};

	if (device.mActiveCanvasSourceTexture != nullptr &&
		upload.sourceTexture == device.mActiveCanvasSourceTexture)
	{
		outResult.activeCanvasSelfReference = true;
		return true;
	}

	if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
	{
		auto* hardwareTexture = static_cast<NRIHardwareTexture*>(upload.sourceTexture->GetHardwareTexture(0, 0));
		if (hardwareTexture != nullptr)
		{
			hardwareTexture->EnsureCanvas(upload.sourceTexture);
			if (hardwareTexture->GetResource().shaderView != nullptr)
			{
				TrackLiveResource(hardwareTexture->GetResource());
				outResult.descriptor = hardwareTexture->GetResource().shaderView;
			}
		}
		return true;
	}

	if (upload.width == 0 || upload.height == 0)
	{
		return true;
	}

	uint32_t cacheIndex = UINT32_MAX;
	if (tracePerf)
	{
		const auto start = std::chrono::steady_clock::now();
		cacheIndex = FindCacheIndex(upload.key);
		outResult.lookupMs += SceneTextureDurationMs(start, std::chrono::steady_clock::now());
	}
	else
	{
		cacheIndex = FindCacheIndex(upload.key);
	}

	if (cacheIndex == UINT32_MAX)
	{
		outResult.cacheMiss = true;
		if (!EnsureCacheEntry(device, upload, &outResult.realizeMs))
		{
			return false;
		}
		cacheIndex = FindCacheIndex(upload.key);
		if (cacheIndex != UINT32_MAX)
		{
			outResult.inserted = true;
		}
	}

	if (cacheIndex != UINT32_MAX)
	{
		outResult.descriptor = mTextureCache[cacheIndex].resource.shaderView;
	}
	return true;
}

uint32_t NRISceneTextureResidency::TransitionInputsForCompute(NRIRenderDevice& device)
{
	uint32_t transitionCount = 0;

	if (mPaletteTexture.texture != nullptr)
	{
		device.TransitionTexture(mPaletteTexture, NRIComputeShaderResourceState());
	}

	if (device.mWhiteTexture != nullptr)
	{
		device.TransitionTexture(device.mWhiteTexture->GetResource(), NRIComputeShaderResourceState());
	}

	for (auto& entry : mTextureCache)
	{
		if (entry.resource.texture != nullptr)
		{
			transitionCount++;
			device.TransitionTexture(entry.resource, NRIComputeShaderResourceState());
		}
	}

	for (NRITextureResource* resource : mLiveResources)
	{
		if (resource != nullptr && resource->texture != nullptr)
		{
			transitionCount++;
			device.TransitionTexture(*resource, NRIComputeShaderResourceState());
		}
	}

	return transitionCount;
}

bool NRIRenderer::UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason)
{
	nri::DescriptorSet* sceneTextureSet = GetCurrentSceneTextureSet();
	if (sceneTextureSet == nullptr)
	{
		return false;
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = sceneTextureSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(descriptors.data());
	update.descriptorNum = (uint32_t)descriptors.size();
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	TraceSharedDescriptorRewrite(
		"scene_textures",
		reason != nullptr ? reason : "unlabeled",
		HashSceneTextureDescriptorList(reinterpret_cast<const nri::Descriptor* const*>(descriptors.data()), descriptors.size()),
		(uint32_t)descriptors.size(),
		true);
	return true;
}

void NRIRenderer::PrepareSceneTextureInputsForCompute()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool tracePerf = ShouldTraceSceneTexturePerf();
	const auto transitionStart = tracePerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const uint32_t transitionCount = mSceneTextures.TransitionInputsForCompute(*mFrameBuffer);

	const double transitionMs = tracePerf ? SceneTextureDurationMs(transitionStart, std::chrono::steady_clock::now()) : 0.0;
	mSceneTextures.CacheStats().transitionCountLastFrame = transitionCount;
	mSceneTextures.CacheStats().transitionMsLastFrame = transitionMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureTransitionCount = transitionCount;
	mLastPerfShellTraceStats.sceneTextureTransitionMs = transitionMs;
}

bool NRIRenderer::EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials)
{
	Clocker clock(NriPTPaletteUpload);
	return mFrameBuffer != nullptr && mSceneTextures.EnsurePaletteTexture(*mFrameBuffer, materials);
}

uint32_t NRIRenderer::FindSceneTextureCacheIndex(uint64_t key) const
{
	return mSceneTextures.FindCacheIndex(key);
}

bool NRIRenderer::EnsureSceneTextureCacheEntry(const nri_scene::TextureUpload& upload, double* outRealizeMs)
{
	return mFrameBuffer != nullptr && mSceneTextures.EnsureCacheEntry(*mFrameBuffer, upload, outRealizeMs);
}

bool NRIRenderer::EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky, const char* reason)
{
	Clocker clock(NriPTSceneTextures);
	static bool sLoggedActiveCanvasTextureReuse = false;
	const bool tracePerf = ShouldTraceSceneTexturePerf();
	uint32_t lookupMisses = 0;
	uint32_t insertCount = 0;
	double lookupMs = 0.0;
	double realizeMs = 0.0;
	double descriptorMs = 0.0;
	mSceneTextures.OverflowStats().textureCountLastBuild = (uint32_t)materials.textures.size();
	mSceneTextures.OverflowStats().truncatedTextureCountLastBuild =
		mSceneTextures.OverflowStats().textureCountLastBuild > NRI_MAX_SCENE_TEXTURES ?
		mSceneTextures.OverflowStats().textureCountLastBuild - NRI_MAX_SCENE_TEXTURES : 0;
	mSceneTextures.OverflowStats().baseTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().normalTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild = 0;
	mSceneTextures.CacheStats().cacheEntriesLastBuild = mSceneTextures.CacheCount();
	mSceneTextures.CacheStats().lookupMissesLastBuild = 0;
	mSceneTextures.CacheStats().insertCountLastBuild = 0;
	mSceneTextures.CacheStats().lookupMsLastBuild = 0.0;
	mSceneTextures.CacheStats().realizeMsLastBuild = 0.0;
	mSceneTextures.CacheStats().descriptorMsLastBuild = 0.0;
	mSceneTextures.ClearLiveResources();
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = 0;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = 0;
	mLastPerfShellTraceStats.sceneTextureLookupMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "none";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls++;
		if (preserveExistingSky)
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls++;
		}
		else
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls++;
		}
	}

	outGpuMaterials = materials.materials;
	ApplyEmissiveMaterialOverrides(materials, outGpuMaterials);
	ApplyActorShadowMaterialOverrides(materials, outGpuMaterials);
	const MaterialTextureAttributionCounts sceneTextureAttribution =
		GatherMaterialTextureAttribution(outGpuMaterials, materials.lightMetadata, materials.textures.size());
	mLastPerfShellTraceStats.sceneTextureRequestedCount = sceneTextureAttribution.textureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = sceneTextureAttribution.actorMaterialCount;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = sceneTextureAttribution.baseTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = sceneTextureAttribution.glowTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = sceneTextureAttribution.normalTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = sceneTextureAttribution.metallicTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = sceneTextureAttribution.roughnessTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = sceneTextureAttribution.emissiveTextureCount;
	if (!EnsureSkyTexture(sceneView, preserveExistingSky))
	{
		return false;
	}

	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mSceneTextures.PaletteTexture().shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;

	for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)materials.textures.size(), NRI_MAX_SCENE_TEXTURES); ++i)
	{
		const auto& upload = materials.textures[i];
		SceneTextureResolveResult textureResult = {};
		if (!mSceneTextures.ResolveTextureDescriptor(*mFrameBuffer, upload, tracePerf, textureResult))
		{
			return false;
		}
		if (textureResult.activeCanvasSelfReference)
		{
			if (!sLoggedActiveCanvasTextureReuse || nri_ptdebug > 0)
			{
				Printf(TEXTCOLOR_ORANGE "NRI PT textures: using a fallback descriptor for the canvas currently being rendered to avoid self-referential camera-texture uploads.\n");
				sLoggedActiveCanvasTextureReuse = true;
			}
			continue;
		}
		if (textureResult.cacheMiss)
		{
			lookupMisses++;
		}
		if (textureResult.inserted)
		{
			insertCount++;
		}
		lookupMs += textureResult.lookupMs;
		realizeMs += textureResult.realizeMs;
		if (textureResult.descriptor != nullptr)
		{
			descriptors[2 + i] = textureResult.descriptor;
		}
	}

	uint32_t actorOverflowTraceLines = 0;
	for (uint32_t materialIndex = 0; materialIndex < (uint32_t)outGpuMaterials.size(); ++materialIndex)
	{
		auto& material = outGpuMaterials[materialIndex];
		const uint32_t originalTextureIndex = material.textureIndex;
		const uint32_t originalNormalTextureIndex = material.normalTextureIndex;
		const uint32_t originalMetallicTextureIndex = material.metallicTextureIndex;
		const uint32_t originalRoughnessTextureIndex = material.roughnessTextureIndex;
		const uint32_t originalEmissiveTextureIndex = material.emissiveTextureIndex;
		bool baseClamped = false;
		bool normalClamped = false;
		bool metallicClamped = false;
		bool roughnessClamped = false;
		bool emissiveClamped = false;
		if (material.textureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().baseTextureClampCountLastBuild++;
			material.textureIndex = 0;
			baseClamped = true;
		}
		if (material.normalTextureIndex != UINT32_MAX && material.normalTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().normalTextureClampCountLastBuild++;
			material.normalTextureIndex = UINT32_MAX;
			normalClamped = true;
		}
		if (material.metallicTextureIndex != UINT32_MAX && material.metallicTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild++;
			material.metallicTextureIndex = UINT32_MAX;
			metallicClamped = true;
		}
		if (material.roughnessTextureIndex != UINT32_MAX && material.roughnessTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild++;
			material.roughnessTextureIndex = UINT32_MAX;
			roughnessClamped = true;
		}
		if (material.emissiveTextureIndex != UINT32_MAX && material.emissiveTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild++;
			material.emissiveTextureIndex = 0;
			emissiveClamped = true;
		}

		if (!(baseClamped || normalClamped || metallicClamped || roughnessClamped || emissiveClamped))
		{
			continue;
		}

		const nri_scene::MaterialLightingMetadata* metadata =
			materialIndex < materials.lightMetadata.size() ? &materials.lightMetadata[materialIndex] : nullptr;
		if (metadata == nullptr || metadata->actorIndex < 0)
		{
			continue;
		}

		mLastPerfShellTraceStats.actorOverflowMaterialCount++;
		if (baseClamped)
		{
			mLastPerfShellTraceStats.actorOverflowBaseClampCount++;
		}
		if (normalClamped)
		{
			mLastPerfShellTraceStats.actorOverflowNormalClampCount++;
		}
		if (metallicClamped)
		{
			mLastPerfShellTraceStats.actorOverflowMetallicClampCount++;
		}
		if (roughnessClamped)
		{
			mLastPerfShellTraceStats.actorOverflowRoughnessClampCount++;
		}
		if (emissiveClamped)
		{
			mLastPerfShellTraceStats.actorOverflowEmissiveClampCount++;
		}

		if (!ShouldTraceActorOverflow())
		{
			continue;
		}

		if (actorOverflowTraceLines < NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES)
		{
			Printf(
				"PERF pt actor overflow NRI: frame=%llu reason=%s actor=%d source=%s material=%u texture_id=%u base=%u->%u normal=%u->%u metallic=%u->%u roughness=%u->%u emissive=%u->%u\n",
				(unsigned long long)mFrameIndex,
				mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
				metadata->actorIndex,
				GetSurfaceSourceTypeName(metadata->sourceType),
				materialIndex,
				metadata->textureId,
				originalTextureIndex,
				material.textureIndex,
				originalNormalTextureIndex,
				material.normalTextureIndex,
				originalMetallicTextureIndex,
				material.metallicTextureIndex,
				originalRoughnessTextureIndex,
				material.roughnessTextureIndex,
				originalEmissiveTextureIndex,
				material.emissiveTextureIndex);
			actorOverflowTraceLines++;
		}
		else
		{
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount++;
		}
	}
	if (mLastPerfShellTraceStats.actorOverflowTraceOmittedCount > 0 && ShouldTraceActorOverflow())
	{
		Printf(
			"PERF pt actor overflow NRI: frame=%llu reason=%s omitted=%u limit=%u\n",
			(unsigned long long)mFrameIndex,
			mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount,
			NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES);
	}

	const bool sceneTextureOverflow =
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild > 0;
	if (sceneTextureOverflow)
	{
		mSceneTextures.OverflowStats().totalOverflowBuilds++;
		if (!mSceneTextures.OverflowStats().warningLogged || (int)nri_pttraceframes > 0 || (int)nri_ptactorspritetrace > 0 || nri_ptdebug > 0)
		{
			Printf(TEXTCOLOR_ORANGE "NRI PT scene textures: requested=%u cap=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u\n",
				mSceneTextures.OverflowStats().textureCountLastBuild,
				NRI_MAX_SCENE_TEXTURES,
				mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
				mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild);
			mSceneTextures.OverflowStats().warningLogged = true;
		}
	}

	mSceneTextures.CacheStats().cacheEntriesLastBuild = mSceneTextures.CacheCount();
	mSceneTextures.CacheStats().cacheEntriesHighWater = std::max(mSceneTextures.CacheStats().cacheEntriesHighWater, mSceneTextures.CacheCount());
	mSceneTextures.CacheStats().lookupMissesLastBuild = lookupMisses;
	mSceneTextures.CacheStats().insertCountLastBuild = insertCount;
	mSceneTextures.CacheStats().lookupMsLastBuild = lookupMs;
	mSceneTextures.CacheStats().realizeMsLastBuild = realizeMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = lookupMisses;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = insertCount;
	mLastPerfShellTraceStats.sceneTextureLookupMs = lookupMs;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = realizeMs;
	bool updated = false;
	if (tracePerf)
	{
		const auto descriptorStart = std::chrono::steady_clock::now();
		updated = UpdateSceneTextureSet(descriptors, reason);
		descriptorMs = SceneTextureDurationMs(descriptorStart, std::chrono::steady_clock::now());
	}
	else
	{
		updated = UpdateSceneTextureSet(descriptors, reason);
	}
	mSceneTextures.CacheStats().descriptorMsLastBuild = descriptorMs;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = descriptorMs;
	return updated;
}

bool NRIRenderer::UseFallbackSceneTextures(bool preserveExistingSky, const char* reason)
{
	mSceneTextures.ClearLiveResources();
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "fallback";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	if (!preserveExistingSky || GetActiveSkyTexture() == nullptr)
	{
		EnsureSkyTexture(nri_scene::SceneView{}, false);
	}
	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr && GetActiveSkyTexture()->shaderView != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	return UpdateSceneTextureSet(descriptors, reason != nullptr ? reason : "fallback");
}

uint32_t NRISceneTextureResidency::FindCacheIndex(uint64_t key) const
{
	const auto it = mTextureCacheKeyIndex.find(key);
	if (it == mTextureCacheKeyIndex.end())
	{
		return UINT32_MAX;
	}

	const uint32_t cacheIndex = it->second;
	if (cacheIndex >= mTextureCache.size() || mTextureCache[cacheIndex].key != key)
	{
		return UINT32_MAX;
	}

	return cacheIndex;
}

uint32_t NRISceneTextureResidency::AddCachedTexture(NRISceneCachedTexture&& texture)
{
	const uint32_t cacheIndex = (uint32_t)mTextureCache.size();
	mTextureCacheKeyIndex[texture.key] = cacheIndex;
	mTextureCache.push_back(std::move(texture));
	return cacheIndex;
}

void NRISceneTextureResidency::ClearLiveResources()
{
	mLiveResources.clear();
}

void NRISceneTextureResidency::TrackLiveResource(NRITextureResource& resource)
{
	if (resource.texture == nullptr)
	{
		return;
	}

	for (NRITextureResource* existing : mLiveResources)
	{
		if (existing == &resource)
		{
			return;
		}
	}

	mLiveResources.push_back(&resource);
}

void NRISceneTextureResidency::ClearCachedTextures()
{
	mTextureCache.clear();
	mTextureCacheKeyIndex.clear();
	mLiveResources.clear();
}
