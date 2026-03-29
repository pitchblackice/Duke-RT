#include "nri_renderer.h"

#include "nri_renderstate.h"
#include "../scene/nri_map_builder.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "skyboxtexture.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <windows.h>

CVAR(Int, nri_ptdebug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_denoise, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrddenoiser, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscaler, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscalermode, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttaa, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_sharpness, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_validation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdmaxframes, 31, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdfastframes, 7, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdstabilizationframes, 31, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_nrdantifirefly, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdhitdistrecon, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsplit, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdfasthistorysigma, 1.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassdiffuse, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdprepassspecular, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdblurmax, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_nrdsigmastabilization, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_nrdsigmaplanedistance, 0.01f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_apivalidation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_dred, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptbootstrap, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptbootstrapmode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectscene, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectionallight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptlightbounces, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmirrorbounces, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsurfaceprobe, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptscenestats, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmutationtracechunk, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmutationtracesector, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptruntimelinktrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissiveheuristics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissiveautoonly, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminpower, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptemissiveminsurface, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissivetlas, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissivefastshadow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptemissivesamples, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsectorlighting, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorambientscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorhemiscale, 0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorfogscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorclamp, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterpal, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterminshade, -128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfiltermaxshade, 127, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorfilterlotag, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsectorpulseframes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsectorpulseamount, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 256;
	constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
	constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 19;
	constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 14;
	constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 15;
	constexpr uint32_t NRI_MAX_RUNTIME_POINT_LIGHTS = 64;
	constexpr uint32_t NRI_MAX_EMISSIVE_SURFACES = 4096;
	constexpr uint32_t NRI_MAX_EMISSIVE_PRIMITIVES = 16384;
	constexpr uint32_t NRI_RUNTIME_LIGHT_TILE_SIZE = 64;
	constexpr uint32_t NRI_PTDEBUG_ANALYTIC_DIRECT = 26;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_TAGS = 27;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_DIRECT = 28;
	constexpr uint32_t NRI_PTDEBUG_SECTOR_AMBIENT = 29;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_UV = 30;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_RADIANCE = 31;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_PRIMITIVE = 32;
	constexpr uint32_t NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY = 33;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_DYNAMIC = 1;
	constexpr uint32_t NRI_SAMPLER_DESCRIPTOR_NUM = 4;
	constexpr uint32_t NRI_FLAG_RESET_HISTORY = 0x1u;
	constexpr uint32_t NRI_FLAG_USE_UPSCALED = 0x2u;
	constexpr uint32_t NRI_FLAG_BOOTSTRAP_VIEW = 0x4u;
	constexpr uint32_t NRI_FLAG_PRESENT_RAW_TRACE = 0x8u;
	constexpr uint32_t NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;
	constexpr uint32_t NRI_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
	constexpr uint32_t NRI_FLAG_USE_JITTER = 0x40u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT = 0x80u;
	constexpr uint32_t NRI_FLAG_FAST_EMISSIVE_SHADOW = 0x100u;
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8;
	constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;
	constexpr uint32_t NRI_EMISSIVE_SAMPLING_FLAG_AUTO_ONLY = 0x1u;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;

	const char* GetMaterialEmissiveModeName(uint32_t mode)
	{
		switch (mode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: return "base";
		case nri_scene::MaterialEmissiveMode_UseConstantColor: return "constant";
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: return "glowmap";
		default: return "none";
		}
	}

	const char* GetSceneDataSourceName(uint32_t dataSource)
	{
		switch (dataSource)
		{
		case NRI_SCENE_DATA_SOURCE_STATIC: return "static";
		case NRI_SCENE_DATA_SOURCE_DYNAMIC: return "dynamic";
		default: return "unknown";
		}
	}

	float ComputePrimitiveArea(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
	{
		if (primitiveIndex >= geometry.primitives.size())
		{
			return 0.0f;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return 0.0f;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	void ComputePrimitiveCenter(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (primitiveIndex >= geometry.primitives.size())
		{
			return;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		outCenter[0] = (a.position[0] + b.position[0] + c.position[0]) / 3.0f;
		outCenter[1] = (a.position[1] + b.position[1] + c.position[1]) / 3.0f;
		outCenter[2] = (a.position[2] + b.position[2] + c.position[2]) / 3.0f;
	}

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

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
		return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS };
	}

	static nri::AccessStage NRIAccelerationStructureWriteAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureScratchAccess()
	{
		return { nri::AccessBits::SCRATCH_BUFFER, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static nri::AccessStage NRIAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
	}

	static uint32_t ClampNrdHistoryFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	}

	static uint32_t ClampNrdFastFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	static uint32_t ClampNrdStabilizationFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	static uint32_t ClampSigmaStabilizationFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
	}

	static uint32_t GetNrdHitDistanceReconstructionMode()
	{
		return (uint32_t)std::clamp((int)nri_nrdhitdistrecon, 0, 2);
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static uint32_t GetNrdInputSplitMode()
	{
		return (uint32_t)std::clamp((int)nri_nrdsplit, 0, 2);
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static float ClampNrdFastHistorySigmaScale(float value)
	{
		return std::clamp(value, 1.0f, 3.0f);
	}

	static float ClampNrdPrepassBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 75.0f);
	}

	static float ClampNrdBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 60.0f);
	}

	static float ClampSigmaPlaneDistanceSensitivity(float value)
	{
		return std::clamp(value, 0.001f, 0.1f);
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static bool SameRuntimeLinkDebugState(const RuntimeLinkDebugState& a, const RuntimeLinkDebugState& b)
	{
		return
			a.available == b.available &&
			a.specialWaterSector == b.specialWaterSector &&
			a.playerSectorIndex == b.playerSectorIndex &&
			a.playerSectorLotag == b.playerSectorLotag &&
			a.playerSectorHitag == b.playerSectorHitag &&
			a.effectiveSectorLotag == b.effectiveSectorLotag &&
			a.actorSectorIndex == b.actorSectorIndex &&
			a.actorSectorLotag == b.actorSectorLotag &&
			a.actorSectorHitag == b.actorSectorHitag &&
			a.onWarpingSector == b.onWarpingSector &&
			a.transporterHold == b.transporterHold &&
			a.rrGeoCount == b.rrGeoCount;
	}

	static bool SameRuntimeTaggedSectorDebugInfo(const RuntimeTaggedSectorDebugInfo& a, const RuntimeTaggedSectorDebugInfo& b)
	{
		if (a.available != b.available ||
			a.sectorIndex != b.sectorIndex ||
			a.lotag != b.lotag ||
			a.hitag != b.hitag ||
			a.effectorCount != b.effectorCount)
		{
			return false;
		}

		for (size_t i = 0; i < countof(a.effectorLotags); ++i)
		{
			if (a.effectorLotags[i] != b.effectorLotags[i] || a.effectorHitags[i] != b.effectorHitags[i])
			{
				return false;
			}
		}

		return true;
	}

	static bool ShouldStoreRuntimeSectorControlInfo(const RuntimeTaggedSectorDebugInfo& info)
	{
		return info.available && (info.lotag != 0 || info.hitag != 0 || info.effectorCount > 0);
	}

	static bool AppendRuntimeSectorControlInfo(std::array<RuntimeTaggedSectorDebugInfo, 12>& infos, uint32_t& infoCount, const RuntimeTaggedSectorDebugInfo& info)
	{
		if (!ShouldStoreRuntimeSectorControlInfo(info))
		{
			return false;
		}

		for (uint32_t i = 0; i < infoCount; ++i)
		{
			if (infos[i].sectorIndex == info.sectorIndex)
			{
				return false;
			}
		}

		if (infoCount >= infos.size())
		{
			return false;
		}

		infos[infoCount++] = info;
		return true;
	}

	static bool GetRuntimeSectorControlInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo& info)
	{
		if (!validSectorIndex(sectorIndex))
		{
			return false;
		}

		info = {};
		if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo(sectorIndex, &info))
		{
			return true;
		}

		const auto& sec = sector[(unsigned)sectorIndex];
		info.available = true;
		info.sectorIndex = sectorIndex;
		info.lotag = sec.lotag;
		info.hitag = sec.hitag;
		return true;
	}

	static const char* GetSkyModeName(nri_scene::PTSkyMode mode)
	{
		switch (mode)
		{
		case nri_scene::PTSkyMode::None:
			return "none";
		case nri_scene::PTSkyMode::SolidColor:
			return "solid";
		case nri_scene::PTSkyMode::Cubemap:
			return "cubemap";
		default:
			return "unknown";
		}
	}

	static const char* GetSkySourceTypeName(nri_scene::PTSkySourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::PTSkySourceType::None:
			return "none";
		case nri_scene::PTSkySourceType::Wall:
			return "wall";
		case nri_scene::PTSkySourceType::Flat:
			return "flat";
		case nri_scene::PTSkySourceType::Portal:
			return "portal";
		default:
			return "unknown";
		}
	}

	static nri::AccessStage NRIComputeAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER };
	}

	static void AppendMutationReasonToken(std::string& text, const char* token)
	{
		if (!text.empty())
		{
			text += "|";
		}
		text += token;
	}

	static std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask)
	{
		std::string text;
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
		{
			AppendMutationReasonToken(text, "sector_geom");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
		{
			AppendMutationReasonToken(text, "sector_mat");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
		{
			AppendMutationReasonToken(text, "wall_geom");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
		{
			AppendMutationReasonToken(text, "wall_mat");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
		{
			AppendMutationReasonToken(text, "sector_dirty");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
		{
			AppendMutationReasonToken(text, "section_dirty");
		}
		if ((reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
		{
			AppendMutationReasonToken(text, "dragged");
		}
		if (text.empty())
		{
			text = "none";
		}
		return text;
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static uint64_t GetGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
	{
		uint64_t newCapacity = std::max<uint64_t>(requiredSize, stride);
		if (currentCapacity >= newCapacity && currentCapacity != 0)
		{
			return currentCapacity;
		}

		if (currentCapacity != 0)
		{
			newCapacity = std::max(newCapacity, currentCapacity);
			while (newCapacity < requiredSize)
			{
				const uint64_t doubled = newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ? newCapacity * 2 : std::numeric_limits<uint64_t>::max();
				if (doubled <= newCapacity)
				{
					newCapacity = requiredSize;
					break;
				}
				newCapacity = doubled;
			}
		}

		return std::max<uint64_t>(newCapacity, stride);
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount)
	{
		return (lightBounceCount & 0xffffu) | ((mirrorBounceCount & 0xffffu) << 16);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount)
	{
		return (denoiserMode & 0xffu) | ((emissiveSampleCount & 0xffu) << 8u);
	}

	static nri_scene::SceneDebugStats MergeSceneStats(const nri_scene::SceneDebugStats& a, const nri_scene::SceneDebugStats& b)
	{
		nri_scene::SceneDebugStats merged = {};
		merged.totalDrawItems = a.totalDrawItems + b.totalDrawItems;
		merged.wallDrawItems = a.wallDrawItems + b.wallDrawItems;
		merged.flatDrawItems = a.flatDrawItems + b.flatDrawItems;
		merged.spriteDrawItems = a.spriteDrawItems + b.spriteDrawItems;
		merged.translucentDrawItems = a.translucentDrawItems + b.translucentDrawItems;
		merged.triangleEstimate = a.triangleEstimate + b.triangleEstimate;
		merged.materialRefs = a.materialRefs + b.materialRefs;
		merged.mirrorSurfaces = a.mirrorSurfaces + b.mirrorSurfaces;
		merged.skySurfaces = a.skySurfaces + b.skySurfaces;
		merged.portalViews = a.portalViews + b.portalViews;
		merged.portalCapturesSkipped = a.portalCapturesSkipped + b.portalCapturesSkipped;
		merged.modelDrawItems = a.modelDrawItems + b.modelDrawItems;
		merged.voxelProxyDrawItems = a.voxelProxyDrawItems + b.voxelProxyDrawItems;
		merged.unsupportedModelDrawItems = a.unsupportedModelDrawItems + b.unsupportedModelDrawItems;
		return merged;
	}

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static uint32_t CountPortalTraversalClass(const nri_scene::PTMapWorld& mapWorld, uint32_t traversalClass)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			if (GetPortalTraversalClass(portal.kind) == traversalClass)
			{
				count++;
			}
		}
		return count;
	}

	static uint32_t CountPendingPlanePortals(const nri_scene::PTMapWorld& mapWorld)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			switch (portal.kind)
			{
			case nri_scene::PTPortalKind::SectorFloorStack:
			case nri_scene::PTPortalKind::SectorCeilingStack:
			case nri_scene::PTPortalKind::SectorFloorMirror:
			case nri_scene::PTPortalKind::SectorCeilingMirror:
				if (portal.sourceSurfaceIndex == UINT32_MAX)
				{
					count++;
				}
				break;
			default:
				break;
			}
		}
		return count;
	}

	static uint32_t CountOrphanLocalSpaces(const nri_scene::PTMapWorld& mapWorld)
	{
		if (!mapWorld.valid || mapWorld.localSpaces.empty())
		{
			return 0;
		}

		std::vector<uint8_t> linked(mapWorld.localSpaces.size(), 0u);
		for (const auto& portal : mapWorld.portals)
		{
			if (portal.sourceLocalSpaceIndex < linked.size())
			{
				linked[portal.sourceLocalSpaceIndex] = 1u;
			}

			for (uint32_t i = 0; i < portal.targetCount; ++i)
			{
				const uint32_t targetIndex = portal.firstTarget + i;
				if (targetIndex >= mapWorld.portalTargets.size())
				{
					break;
				}

				const uint32_t localSpaceIndex = mapWorld.portalTargets[targetIndex].localSpaceIndex;
				if (localSpaceIndex < linked.size())
				{
					linked[localSpaceIndex] = 1u;
				}
			}
		}

		uint32_t orphanCount = 0;
		for (uint8_t value : linked)
		{
			if (value == 0u)
			{
				orphanCount++;
			}
		}

		return orphanCount;
	}

	static void TranslateGeometry(nri_scene::GeometryData& geometry, float dx, float dy, float dz, float prevDx, float prevDy, float prevDz)
	{
		for (auto& vertex : geometry.vertices)
		{
			vertex.position[0] += dx;
			vertex.position[1] += dy;
			vertex.position[2] += dz;
			vertex.prevPosition[0] += prevDx;
			vertex.prevPosition[1] += prevDy;
			vertex.prevPosition[2] += prevDz;
		}
	}

	static void AssignGeometryPortalIndices(const nri_scene::PTMapWorld& mapWorld, nri_scene::GeometryData& geometry)
	{
		const size_t count = std::min(geometry.primitives.size(), geometry.primitiveProvenance.size());
		for (size_t i = 0; i < count; ++i)
		{
			geometry.primitives[i].portalIndex = UINT32_MAX;
			const uint32_t flags = geometry.primitives[i].flags;
			if ((flags & (nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Portal)) == 0)
			{
				continue;
			}

			const int32_t portalIndex = nri_scene::FindMapWorldPortalIndex(mapWorld, geometry.primitiveProvenance[i]);
			if (portalIndex >= 0)
			{
				geometry.primitives[i].portalIndex = (uint32_t)portalIndex;
			}
		}
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}

	static void AppendGeometry(const nri_scene::GeometryData& source, uint32_t materialIndexOffset, nri_scene::GeometryData& destination)
	{
		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(destination.vertices.end(), source.vertices.begin(), source.vertices.end());

		destination.indices.reserve(destination.indices.size() + source.indices.size());
		for (uint32_t index : source.indices)
		{
			destination.indices.push_back(vertexBase + index);
		}

		destination.primitives.reserve(destination.primitives.size() + source.primitives.size());
		for (const auto& primitive : source.primitives)
		{
			nri_scene::PrimitiveData copy = primitive;
			copy.indices[0] += vertexBase;
			copy.indices[1] += vertexBase;
			copy.indices[2] += vertexBase;
			copy.materialIndex += materialIndexOffset;
			destination.primitives.push_back(copy);
		}

		destination.primitiveProvenance.insert(destination.primitiveProvenance.end(), source.primitiveProvenance.begin(), source.primitiveProvenance.end());
	}

	static void AppendMaterialBridge(const nri_scene::MaterialBridgeData& source, nri_scene::MaterialBridgeData& destination)
	{
		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(destination.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)destination.textures.size(); ++i)
		{
			textureLookup.emplace(destination.textures[i].key, i);
		}

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			const bool hasLightMetadata = materialIndex < source.lightMetadata.size();
			if (material.textureIndex < source.textures.size())
			{
				const auto& texture = source.textures[material.textureIndex];
				auto it = textureLookup.find(texture.key);
				if (it == textureLookup.end())
				{
					const uint32_t newIndex = (uint32_t)destination.textures.size();
					textureLookup.emplace(texture.key, newIndex);
					destination.textures.push_back(texture);
					copy.textureIndex = newIndex;
				}
				else
				{
					copy.textureIndex = it->second;
				}
			}

			destination.materials.push_back(copy);
			if (hasLightMetadata)
			{
				destination.lightMetadata.push_back(source.lightMetadata[materialIndex]);
			}
		}

		if (destination.paletteLookup.empty())
		{
			destination.paletteLookup = source.paletteLookup;
			destination.paletteWidth = source.paletteWidth;
			destination.paletteHeight = source.paletteHeight;
		}
	}

	static float GetUpscalerRenderScale(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		default:
		case nri::UpscalerMode::NATIVE: return 1.0f;
		case nri::UpscalerMode::ULTRA_QUALITY: return 1.0f / 1.3f;
		case nri::UpscalerMode::QUALITY: return 1.0f / 1.5f;
		case nri::UpscalerMode::BALANCED: return 1.0f / 1.7f;
		case nri::UpscalerMode::PERFORMANCE: return 0.5f;
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return 1.0f / 3.0f;
		}
	}

	static const char* GetUpscalerName(NRIUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIUpscalerKind::NIS: return "NIS";
		case NRIUpscalerKind::DLSR: return "DLSS-SR";
		case NRIUpscalerKind::DLRR: return "DLRR";
		default: return "off";
		}
	}

	static const char* GetUpscalerModeName(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		case nri::UpscalerMode::ULTRA_QUALITY: return "ultra_quality";
		case nri::UpscalerMode::QUALITY: return "quality";
		case nri::UpscalerMode::BALANCED: return "balanced";
		case nri::UpscalerMode::PERFORMANCE: return "performance";
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return "ultra_performance";
		default: return "native";
		}
	}

	static nri::UpscalerType ToUpscalerType(NRIUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIUpscalerKind::NIS: return nri::UpscalerType::NIS;
		case NRIUpscalerKind::DLSR: return nri::UpscalerType::DLSR;
		case NRIUpscalerKind::DLRR: return nri::UpscalerType::DLRR;
		default: return nri::UpscalerType::NIS;
		}
	}

	struct NRITraceConstants
	{
		float CameraPos[3] = {};
		uint32_t RenderWidth = 0;
		float CameraForward[3] = {};
		uint32_t RenderHeight = 0;
		float CameraRight[3] = {};
		float TanHalfFovX = 1.0f;
		float CameraUp[3] = {};
		float TanHalfFovY = 1.0f;
		float PrevCameraPos[3] = {};
		uint32_t DisplayWidth = 0;
		float PrevCameraForward[3] = {};
		uint32_t DisplayHeight = 0;
		float PrevCameraRight[3] = {};
		float PrevTanHalfFovX = 1.0f;
		float PrevCameraUp[3] = {};
		float PrevTanHalfFovY = 1.0f;
		float LightDirection[3] = { 0.3f, 0.85f, -0.4f };
		uint32_t SceneInstanceCount = 0;
		float SkyColor[3] = { 0.38f, 0.48f, 0.65f };
		uint32_t DebugMode = 0;
		float GroundColor[3] = { 0.08f, 0.08f, 0.08f };
		uint32_t StaticPrimitiveCount = 0;
		uint32_t FrameIndex = 0;
		uint32_t DynamicPrimitiveCount = 0;
		uint32_t Flags = 0;
		uint32_t StaticMaterialCount = 0;
		uint32_t BootstrapMode = 0;
		uint32_t DynamicMaterialCount = 0;
		uint32_t BounceCounts = 0;
		uint32_t PortalCount = 0;
		uint32_t RuntimeLightCount = 0;
		uint32_t PortalDepth = 0;
		uint32_t ReservedTrace0 = 0;
		uint32_t ReservedTrace1 = 0;
	};

	struct NRIReprojectionData
	{
		float currentViewToClip[16] = {};
		float previousViewToClip[16] = {};
		float currentWorldToView[16] = {};
		float previousWorldToView[16] = {};
	};

	static bool IsAppTaaEligibleUpscaler(NRIUpscalerKind kind)
	{
		return kind == NRIUpscalerKind::Off || kind == NRIUpscalerKind::NIS;
	}

	static bool ShouldRunAppTaa(NRIUpscalerKind kind)
	{
		return IsAppTaaEligibleUpscaler(kind) && !!nri_pttaa;
	}

	static bool ShouldUseTemporalJitter(NRIUpscalerKind kind)
	{
		return ShouldRunAppTaa(kind) || kind == NRIUpscalerKind::DLSR || kind == NRIUpscalerKind::DLRR;
	}

	static float GetHaltonSample(uint32_t index, uint32_t base)
	{
		float inverseBase = 1.0f / (float)base;
		float fraction = inverseBase;
		float result = 0.0f;

		while (index > 0)
		{
			result += fraction * (float)(index % base);
			index /= base;
			fraction *= inverseBase;
		}

		return result;
	}

	static void ComputeTemporalJitter(uint32_t frameIndex, float outJitter[2])
	{
		const uint32_t sampleIndex = (frameIndex % NRI_TAA_JITTER_PHASE_COUNT) + 1u;
		outJitter[0] = GetHaltonSample(sampleIndex, 2u) - 0.5f;
		outJitter[1] = GetHaltonSample(sampleIndex, 3u) - 0.5f;
	}

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

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
	}

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static bool HasAutoEmissiveSourceFlags(uint32_t sourceFlags)
	{
		return (sourceFlags & (
			SceneEmissiveSurfaceSourceFlag_AutoFullbright |
			SceneEmissiveSurfaceSourceFlag_AutoTextureGlow |
			SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	struct SkyFaceUpload
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct SkyFaceProbe
	{
		FGameTexture* texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t contentId = 0;
	};

	struct SkyProbe
	{
		uint64_t key = 0;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceProbe, 6> faces = {};
	};

	struct SkyUpload
	{
		uint64_t key = 0;
		bool cubemap = false;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceUpload, 6> faces = {};
	};

	static uint64_t HashBytes64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static bool IsUsableGameTexturePointer(FGameTexture* texture)
	{
		const intptr_t value = (intptr_t)texture;
		return value > 0x10000 && value != -1;
	}

	static FTexture* TryGetBaseTexture(FGameTexture* texture)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return nullptr;
		}

		FTexture* baseTexture = nullptr;
		__try
		{
			baseTexture = texture->GetTexture();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseTexture = nullptr;
		}

		return baseTexture;
	}

	static FGameTexture* TryGetSkyFace(FSkyBox* skybox, int index)
	{
		if (skybox == nullptr || index < 0 || index >= 6)
		{
			return nullptr;
		}

		FGameTexture* face = nullptr;
		__try
		{
			face = skybox->GetSkyFace(index);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			face = nullptr;
		}

		return IsUsableGameTexturePointer(face) ? face : nullptr;
	}

	static uint64_t HashSkyColor(const float* color)
	{
		const uint8_t rgba[4] = {
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[0]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[1]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(Clamp01(color[2]) * 255.0f), 0, 255),
			255
		};
		return HashBytes64(rgba, sizeof(rgba));
	}

	static void FlipImageHorizontal(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		for (uint32_t y = 0; y < height; ++y)
		{
			uint8_t* row = pixels.data() + (size_t)y * width * 4u;
			for (uint32_t x = 0; x < width / 2; ++x)
			{
				uint8_t* a = row + x * 4u;
				uint8_t* b = row + (width - 1 - x) * 4u;
				for (uint32_t c = 0; c < 4; ++c)
				{
					std::swap(a[c], b[c]);
				}
			}
		}
	}

	static void FlipImageVertical(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		const size_t rowSize = (size_t)width * 4u;
		std::vector<uint8_t> temp(rowSize);
		for (uint32_t y = 0; y < height / 2; ++y)
		{
			uint8_t* a = pixels.data() + (size_t)y * rowSize;
			uint8_t* b = pixels.data() + (size_t)(height - 1 - y) * rowSize;
			std::memcpy(temp.data(), a, rowSize);
			std::memcpy(a, b, rowSize);
			std::memcpy(b, temp.data(), rowSize);
		}
	}

	static bool CopyFacePixels(FGameTexture* texture, SkyFaceUpload& outFace)
	{
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		outFace.width = (uint32_t)texBuffer.mWidth;
		outFace.height = (uint32_t)texBuffer.mHeight;
		outFace.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
		return true;
	}

	static bool ProbeFace(FGameTexture* texture, SkyFaceProbe& outFace)
	{
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		outFace.texture = texture;
		outFace.width = (uint32_t)texBuffer.mWidth;
		outFace.height = (uint32_t)texBuffer.mHeight;
		outFace.contentId = texBuffer.mContentId != 0 ? texBuffer.mContentId : (uint64_t)(uintptr_t)texture;
		return true;
	}

	static bool ProbeCubemapSky(const nri_scene::SceneView& sceneView, SkyProbe& outProbe)
	{
		if (sceneView.sky.mode != nri_scene::PTSkyMode::Cubemap || !IsUsableGameTexturePointer(sceneView.sky.texture))
		{
			return false;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(TryGetBaseTexture(sceneView.sky.texture));
		if (skybox == nullptr)
		{
			return false;
		}

		struct FaceMapping
		{
			int sourceIndex;
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ 3, false, false }, // +X = west
			{ 1, false, false }, // -X = east
			{ 4, true, false },  // +Y = top
			{ 5, true, true },   // -Y = bottom
			{ 2, false, false }, // +Z = south
			{ 0, false, false }  // -Z = north
		};

		uint64_t key = HashCombine64(1469598103934665603ull, (uint64_t)(uintptr_t)sceneView.sky.texture);
		key = HashCombine64(key, (uint64_t)sceneView.sky.faceMask);
		key = HashCombine64(key, sceneView.sky.flipTop ? 1ull : 0ull);
		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!ProbeFace(TryGetSkyFace(skybox, mappings[i].sourceIndex), outProbe.faces[i]))
			{
				return false;
			}

			key = HashCombine64(key, (uint64_t)(uintptr_t)outProbe.faces[i].texture);
			key = HashCombine64(key, outProbe.faces[i].contentId);
			key = HashCombine64(key, ((uint64_t)outProbe.faces[i].width << 32) | outProbe.faces[i].height);
		}

		outProbe.width = outProbe.faces[0].width;
		outProbe.height = outProbe.faces[0].height;
		for (uint32_t i = 1; i < 6; ++i)
		{
			if (outProbe.faces[i].width != outProbe.width || outProbe.faces[i].height != outProbe.height)
			{
				return false;
			}
		}

		outProbe.key = key;
		return true;
	}

	static bool BuildCubemapUpload(const nri_scene::SceneView& sceneView, const SkyProbe& probe, SkyUpload& outUpload)
	{
		struct FaceMapping
		{
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ false, false }, // +X = west
			{ false, false }, // -X = east
			{ true, false },  // +Y = top
			{ true, true },   // -Y = bottom
			{ false, false }, // +Z = south
			{ false, false }  // -Z = north
		};

		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!CopyFacePixels(probe.faces[i].texture, outUpload.faces[i]))
			{
				return false;
			}

			if (i == 2 && sceneView.sky.flipTop)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipHorizontal)
			{
				FlipImageHorizontal(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipVertical)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
		}

		outUpload.key = probe.key;
		outUpload.width = probe.width;
		outUpload.height = probe.height;
		outUpload.cubemap = true;
		return true;
	}

	static void BuildSolidSkyUpload(const float* skyColor, SkyUpload& outUpload)
	{
		outUpload = {};
		outUpload.key = HashSkyColor(skyColor) ^ 0x53594b59554c4c45ull;
		for (auto& face : outUpload.faces)
		{
			face.width = 1;
			face.height = 1;
			face.pixels = {
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[2]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[1]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(Clamp01(skyColor[0]) * 255.0f), 0, 255),
				255
			};
		}
	}

	static void RemapToPTSpace(const float* src, float* dst)
	{
		dst[0] = src[0];
		dst[1] = src[2];
		dst[2] = src[1];
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static bool IntersectProbeTriangle(const nri_scene::SceneVertex& v0, const nri_scene::SceneVertex& v1, const nri_scene::SceneVertex& v2, const float origin[3], const float direction[3], float& outT)
	{
		outT = 0.0f;
		const float edge1[3] = {
			v1.position[0] - v0.position[0],
			v1.position[1] - v0.position[1],
			v1.position[2] - v0.position[2]
		};
		const float edge2[3] = {
			v2.position[0] - v0.position[0],
			v2.position[1] - v0.position[1],
			v2.position[2] - v0.position[2]
		};
		const float p[3] = {
			direction[1] * edge2[2] - direction[2] * edge2[1],
			direction[2] * edge2[0] - direction[0] * edge2[2],
			direction[0] * edge2[1] - direction[1] * edge2[0]
		};
		const float det = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
		if (fabsf(det) < 1e-5f)
		{
			return false;
		}

		const float invDet = 1.0f / det;
		const float t[3] = {
			origin[0] - v0.position[0],
			origin[1] - v0.position[1],
			origin[2] - v0.position[2]
		};
		const float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * invDet;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}

		const float q[3] = {
			t[1] * edge1[2] - t[2] * edge1[1],
			t[2] * edge1[0] - t[0] * edge1[2],
			t[0] * edge1[1] - t[1] * edge1[0]
		};
		const float v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * invDet;
		if (v < 0.0f || (u + v) > 1.0f)
		{
			return false;
		}

		const float hitT = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * invDet;
		if (hitT <= 0.001f)
		{
			return false;
		}

		outT = hitT;
		return true;
	}

	static const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType)
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
		default: return "unknown";
		}
	}

	static const char* GetDrawListTypeName(uint32_t drawListType)
	{
		switch (drawListType)
		{
		case GLDL_PLAINWALLS: return "plain_walls";
		case GLDL_MASKEDWALLS: return "masked_walls";
		case GLDL_MASKEDWALLSS: return "masked_walls_split";
		case GLDL_MASKEDWALLSD: return "masked_walls_decal";
		case GLDL_MASKEDWALLSV: return "masked_walls_view";
		case GLDL_MASKEDWALLSH: return "masked_walls_horizon";
		case GLDL_TRANSLUCENTBORDER: return "translucent_border";
		case GLDL_PLAINFLATS: return "plain_flats";
		case GLDL_MASKEDFLATS: return "masked_flats";
		case GLDL_MASKEDSLOPEFLATS: return "masked_slope_flats";
		case GLDL_TRANSLUCENT: return "translucent";
		case GLDL_MODELS: return "models";
		case UINT32_MAX: return "none";
		default: return "unknown";
		}
	}

	static const char* GetSceneLightRecordSourceName(SceneLightRecordSource source)
	{
		switch (source)
		{
		case SceneLightRecordSource::CapturedScene: return "captured_scene";
		case SceneLightRecordSource::StaticMapScene: return "static_map_scene";
		case SceneLightRecordSource::DynamicScene: return "dynamic_scene";
		default: return "none";
		}
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
	Clocker clock(NriPTInitialize);

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}

	if (!CheckPathTracingSupport())
	{
		return true;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	return CreatePipelineLayout() && CreateTaaPipelineLayout() && AllocateDescriptorSets() && UpdateSamplerSet() && CreatePipelines();
}

void NRIRenderer::Shutdown()
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return;
	}

	mNrd.Shutdown();
	mUpscaler.Shutdown(*mFrameBuffer);
	DestroyAccelerationStructures();
	ClearRuntimePointLights();
	DestroySceneBuffers();
	DestroyFrameTextures();
	mFrameBuffer->DestroyTextureResource(mPaletteTexture);
	DestroyCachedTextures();

	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr)
		{
			mFrameBuffer->mCore.DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
	}

	if (mPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}
	if (mTaaPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mTaaPipelineLayout);
		mTaaPipelineLayout = nullptr;
	}

	mSamplerSet = nullptr;
	mSceneTextureSet = nullptr;
	mFrameTextureSet = nullptr;
	mOutputSet = nullptr;
	mCompositionFrameTextureSet = nullptr;
	mCompositionOutputSet = nullptr;
	mTaaFrameTextureSet = nullptr;
	mTaaOutputSet = nullptr;
	mPresentFrameTextureSet = nullptr;
	mPresentOutputSet = nullptr;
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if ((drawmode != DM_MAINVIEW && drawmode != DM_OFFSCREEN) || portal || mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!mPathTracingSupported)
	{
		LogFallback(GetAvailabilityReason());
		return false;
	}

	Clocker totalClock(NriPTAll);

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	uint32_t savedFrameIndex = mFrameIndex;
	float savedCurrentCameraPos[3] = {};
	float savedCurrentCameraForward[3] = {};
	float savedCurrentCameraRight[3] = {};
	float savedCurrentCameraUp[3] = {};
	float savedPreviousCameraPos[3] = {};
	float savedPreviousCameraForward[3] = {};
	float savedPreviousCameraRight[3] = {};
	float savedPreviousCameraUp[3] = {};
	float savedCurrentJitter[2] = {};
	float savedPreviousJitter[2] = {};
	float savedCurrentViewToClip[16] = {};
	float savedPreviousViewToClip[16] = {};
	float savedCurrentWorldToView[16] = {};
	float savedPreviousWorldToView[16] = {};
	float savedCurrentTanHalfFovX = mCurrentTanHalfFovX;
	float savedCurrentTanHalfFovY = mCurrentTanHalfFovY;
	float savedPreviousTanHalfFovX = mPreviousTanHalfFovX;
	float savedPreviousTanHalfFovY = mPreviousTanHalfFovY;
	bool savedHasPreviousCameraState = mHasPreviousCameraState;
	bool savedResetHistory = mResetHistory;
	if (preserveHistory)
	{
		Copy3(mCurrentCameraPos, savedCurrentCameraPos);
		Copy3(mCurrentCameraForward, savedCurrentCameraForward);
		Copy3(mCurrentCameraRight, savedCurrentCameraRight);
		Copy3(mCurrentCameraUp, savedCurrentCameraUp);
		Copy3(mPreviousCameraPos, savedPreviousCameraPos);
		Copy3(mPreviousCameraForward, savedPreviousCameraForward);
		Copy3(mPreviousCameraRight, savedPreviousCameraRight);
		Copy3(mPreviousCameraUp, savedPreviousCameraUp);
		Copy2(mCurrentJitter, savedCurrentJitter);
		Copy2(mPreviousJitter, savedPreviousJitter);
		std::memcpy(savedCurrentViewToClip, mCurrentViewToClip, sizeof(savedCurrentViewToClip));
		std::memcpy(savedPreviousViewToClip, mPreviousViewToClip, sizeof(savedPreviousViewToClip));
		std::memcpy(savedCurrentWorldToView, mCurrentWorldToView, sizeof(savedCurrentWorldToView));
		std::memcpy(savedPreviousWorldToView, mPreviousWorldToView, sizeof(savedPreviousWorldToView));
	}

	auto restoreHistory = [this, &savedCurrentCameraPos, &savedCurrentCameraForward, &savedCurrentCameraRight, &savedCurrentCameraUp,
		&savedPreviousCameraPos, &savedPreviousCameraForward, &savedPreviousCameraRight, &savedPreviousCameraUp, &savedCurrentJitter, &savedPreviousJitter,
		&savedCurrentViewToClip, &savedPreviousViewToClip, &savedCurrentWorldToView, &savedPreviousWorldToView, savedFrameIndex, savedCurrentTanHalfFovX,
		savedCurrentTanHalfFovY, savedPreviousTanHalfFovX, savedPreviousTanHalfFovY, savedHasPreviousCameraState, savedResetHistory]()
	{
		mFrameIndex = savedFrameIndex;
		Copy3(savedCurrentCameraPos, mCurrentCameraPos);
		Copy3(savedCurrentCameraForward, mCurrentCameraForward);
		Copy3(savedCurrentCameraRight, mCurrentCameraRight);
		Copy3(savedCurrentCameraUp, mCurrentCameraUp);
		Copy3(savedPreviousCameraPos, mPreviousCameraPos);
		Copy3(savedPreviousCameraForward, mPreviousCameraForward);
		Copy3(savedPreviousCameraRight, mPreviousCameraRight);
		Copy3(savedPreviousCameraUp, mPreviousCameraUp);
		Copy2(savedCurrentJitter, mCurrentJitter);
		Copy2(savedPreviousJitter, mPreviousJitter);
		std::memcpy(mCurrentViewToClip, savedCurrentViewToClip, sizeof(mCurrentViewToClip));
		std::memcpy(mPreviousViewToClip, savedPreviousViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mCurrentWorldToView, savedCurrentWorldToView, sizeof(mCurrentWorldToView));
		std::memcpy(mPreviousWorldToView, savedPreviousWorldToView, sizeof(mPreviousWorldToView));
		mCurrentTanHalfFovX = savedCurrentTanHalfFovX;
		mCurrentTanHalfFovY = savedCurrentTanHalfFovY;
		mPreviousTanHalfFovX = savedPreviousTanHalfFovX;
		mPreviousTanHalfFovY = savedPreviousTanHalfFovY;
		mHasPreviousCameraState = savedHasPreviousCameraState;
		mResetHistory = savedResetHistory;
	};

	const bool ready =
		Initialize() &&
		EnsureFrameResources(
			std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u),
			std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u),
			mFrameBuffer->mActiveTarget->width,
			mFrameBuffer->mActiveTarget->height);
	if (!ready)
	{
		LogFallback("PT frame resources or pipelines failed to initialize.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	ResetSceneBufferFrameStats();
	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;
	mDynamicSceneLastFrame = {};
	mRuntimeMapLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};

	if (!preserveHistory)
	{
		const NRIUpscalerKind resolvedUpscaler = ResolveUpscalerKind(false);
		const bool runAppTaa = ShouldRunAppTaa(resolvedUpscaler);
		if (!nri_ptbootstrap && (debugMode != mLastDebugMode || resolvedUpscaler != mLastTemporalHistoryUpscaler || runAppTaa != mLastTemporalAppTaaEnabled))
		{
			ArmTemporalTraceBudget("mode-change");
			if (nri_pttraceframes > 0)
			{
				Printf("NRI PT temporal reset: reason=mode-change frame=%u debug=%d->%d resolved=%s->%s app_taa=%s->%s\n",
					mFrameIndex,
					mLastDebugMode,
					debugMode,
					GetUpscalerName(mLastTemporalHistoryUpscaler),
					GetUpscalerName(resolvedUpscaler),
					mLastTemporalAppTaaEnabled ? "yes" : "no",
					runAppTaa ? "yes" : "no");
			}
			mResetHistory = true;
		}
		mLastDebugMode = debugMode;
		mLastTemporalHistoryUpscaler = resolvedUpscaler;
		mLastTemporalAppTaaEnabled = runAppTaa;
	}

	RefreshMapWorld();
	if (mPendingStaticMapLightingInvalidation)
	{
		InvalidateStaticMapSceneForMaterialLighting();
		mPendingStaticMapLightingInvalidation = false;
	}
	UpdatePerFrameState(di);
	if (preserveHistory)
	{
		mResetHistory = true;
	}

	if (bootstrapSimpleView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		if (!DispatchBootstrapView())
		{
			LogFallback("PT bootstrap view dispatch failed.");
			if (preserveHistory)
			{
				restoreHistory();
			}
			return false;
		}

		CopyFinalToActiveTarget();
		if (!preserveHistory)
		{
			++mFrameIndex;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			restoreHistory();
		}
		return true;
	}

	const bool allowStaticMapScene = !bootstrapCapturedView && !rawTraceDirectScene && mMapWorld.valid;
	nri_scene::SceneView capturedSceneView;
	nri_scene::SceneView dynamicSceneView;
	nri_scene::GeometryData capturedGeometry;
	nri_scene::GeometryData runtimeMutationGeometry;
	nri_scene::GeometryData runtimeSpaceLinkGeometry;
	nri_scene::GeometryData dynamicGeometry;
	nri_scene::GeometryData mergedDynamicGeometry;
	nri_scene::GeometryData overlayGeometry;
	nri_scene::GeometryData combinedGeometry;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData runtimeMutationMaterialBridge;
	nri_scene::MaterialBridgeData runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData overlayMaterialBridge;
	nri_scene::MaterialBridgeData combinedMaterialBridge;
	std::vector<nri_scene::MaterialData> capturedGpuMaterials;
	std::vector<nri_scene::MaterialData> dynamicGpuMaterials;
	std::vector<nri_scene::MaterialData> combinedGpuMaterials;
	const nri_scene::SceneView* activeSceneView = nullptr;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	const nri_scene::MaterialBridgeData* activeMaterialBridge = nullptr;
	const nri_scene::SceneView* sceneLightCapturedView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightCapturedMaterials = nullptr;
	const nri_scene::SceneView* sceneLightDynamicView = nullptr;
	const nri_scene::MaterialBridgeData* sceneLightDynamicMaterials = nullptr;
	nri_scene::SceneView mergedDynamicSceneView;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;
	bool usingPersistentDynamicEmissiveCache = false;
	bool liveDynamicHasEmissive = false;

	if (allowStaticMapScene && EnsureStaticMapScene())
	{
		sceneLightUsesStaticMapScene = true;
		emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
		mUsedStaticMapSceneLastFrame = true;
		activeSceneView = &mStaticMapScene.sceneView;
		activeGeometry = &mStaticMapScene.geometry;
		activeGpuMaterials = &mStaticMapScene.gpuMaterials;
		activeMaterialBridge = &mStaticMapScene.materialBridge;
		activeStats = mStaticMapScene.sceneView.stats;

		const bool deferOverlayThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
		const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
		const bool hasRuntimeMutationOverlay = !deferOverlayThisFrame && BuildRuntimeMapMutationOverlay(runtimeMutationGeometry, runtimeMutationMaterialBridge);
		const bool hasDynamicScene = !deferOverlayThisFrame && nri_scene::CaptureDynamicScene(di, dynamicSceneView);
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
				AssignGeometryPortalIndices(mMapWorld, dynamicGeometry);
			}

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					nri_scene::BuildMaterials(dynamicSceneView, dynamicMaterialBridge);
				}
			}

			sceneLightDynamicView = &dynamicSceneView;
			sceneLightDynamicMaterials = &dynamicMaterialBridge;
			activeDynamicSceneView = &dynamicSceneView;
			activeDynamicGeometry = &dynamicGeometry;
			activeDynamicMaterials = &dynamicMaterialBridge;
			liveDynamicHasEmissive = RebuildPersistentDynamicEmissiveCache(dynamicSceneView, dynamicMaterialBridge);
		}

		const bool shouldUsePersistentDynamicEmissive = mPersistentDynamicEmissiveCache.valid && !liveDynamicHasEmissive;
		if (shouldUsePersistentDynamicEmissive)
		{
			usingPersistentDynamicEmissiveCache = true;
			if (hasDynamicScene)
			{
				mergedDynamicSceneView = dynamicSceneView;
				mergedDynamicSceneView.opaqueWalls.insert(
					mergedDynamicSceneView.opaqueWalls.end(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.begin(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.end());
				mergedDynamicSceneView.opaqueFlats.insert(
					mergedDynamicSceneView.opaqueFlats.end(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.begin(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.end());
				mergedDynamicSceneView.opaqueSprites.insert(
					mergedDynamicSceneView.opaqueSprites.end(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.begin(),
					mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.end());
				mergedDynamicSceneView.stats = MergeSceneStats(dynamicSceneView.stats, mPersistentDynamicEmissiveCache.sceneView.stats);

				{
					Clocker clock(NriPTGeometryBuild);
					nri_scene::BuildGeometry(mergedDynamicSceneView, mergedDynamicGeometry);
					AssignGeometryPortalIndices(mMapWorld, mergedDynamicGeometry);
				}
				{
					Clocker clock(NriPTMaterialBuild);
					nri_scene::BuildMaterials(mergedDynamicSceneView, mergedDynamicMaterialBridge);
				}

				if (!mergedDynamicGeometry.primitives.empty())
				{
					activeDynamicSceneView = &mergedDynamicSceneView;
					activeDynamicGeometry = &mergedDynamicGeometry;
					activeDynamicMaterials = &mergedDynamicMaterialBridge;
				}
			}
			else
			{
				activeDynamicSceneView = &mPersistentDynamicEmissiveCache.sceneView;
				activeDynamicGeometry = &mPersistentDynamicEmissiveCache.geometry;
				activeDynamicMaterials = &mPersistentDynamicEmissiveCache.materialBridge;
			}

			sceneLightDynamicView = activeDynamicSceneView;
			sceneLightDynamicMaterials = activeDynamicMaterials;
		}

		const bool hasActiveDynamicOverlay =
			activeDynamicGeometry != nullptr &&
			!activeDynamicGeometry->primitives.empty() &&
			activeDynamicMaterials != nullptr;

		if (hasRuntimeSpaceLinkOverlay || hasRuntimeMutationOverlay || hasActiveDynamicOverlay)
		{
			overlayGeometry = {};
			overlayMaterialBridge = {};

			if (hasRuntimeSpaceLinkOverlay)
			{
				if (!runtimeSpaceLinkGeometry.primitives.empty())
				{
					AppendGeometry(runtimeSpaceLinkGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				}
				AppendMaterialBridge(runtimeSpaceLinkMaterialBridge, overlayMaterialBridge);
			}

			if (hasRuntimeMutationOverlay)
			{
				if (!runtimeMutationGeometry.primitives.empty())
				{
					AppendGeometry(runtimeMutationGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				}
				AppendMaterialBridge(runtimeMutationMaterialBridge, overlayMaterialBridge);
			}

			if (hasActiveDynamicOverlay)
			{
				AppendGeometry(*activeDynamicGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(*activeDynamicMaterials, overlayMaterialBridge);
			}

			std::vector<nri::TopLevelInstance> instances;
			std::vector<SceneInstanceData> sceneInstances;
			const std::vector<uint8_t>* replacedChunkMask = hasRuntimeMutationOverlay ? &mRuntimeMapMutations.replacedChunkMask : nullptr;
			BuildStaticMapInstances(instances, sceneInstances, replacedChunkMask);

			if (overlayGeometry.primitives.empty())
			{
				accelerationReady =
					BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
					UpdateSceneDataSet(
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						mStaticVertexBuffer,
						mStaticIndexBuffer,
						mStaticPrimitiveBuffer,
						mStaticMaterialBuffer,
						sceneInstances,
						(uint32_t)mStaticMapScene.geometry.primitives.size(),
						0u,
						(uint32_t)mStaticMapScene.gpuMaterials.size(),
						0u);
				if (accelerationReady && hasRuntimeMutationOverlay)
				{
					mBuiltStaticMapSceneASLastFrame = false;
				}
			}
			else
			{
				combinedMaterialBridge = mStaticMapScene.materialBridge;
				AppendMaterialBridge(overlayMaterialBridge, combinedMaterialBridge);
				paletteReady = EnsurePaletteTexture(combinedMaterialBridge);
				texturesReady = paletteReady && EnsureSceneTextures(mStaticMapScene.sceneView, combinedMaterialBridge, combinedGpuMaterials, false);
				dynamicGpuMaterials.clear();
				if (texturesReady)
				{
					const size_t staticMaterialCount = mStaticMapScene.gpuMaterials.size();
					if (combinedGpuMaterials.size() < staticMaterialCount)
					{
						texturesReady = false;
					}
					else
					{
						dynamicGpuMaterials.assign(combinedGpuMaterials.begin() + staticMaterialCount, combinedGpuMaterials.end());
					}
				}
				buffersReady = texturesReady && UploadSceneBuffers(overlayGeometry, dynamicGpuMaterials);
				accelerationReady = false;
				if (buffersReady)
				{
					accelerationReady =
						BuildDynamicAccelerationStructure(overlayGeometry) &&
						mDynamicBottomLevelAS.accelerationStructure != nullptr;
				}
				emissiveSamplingContext.dynamicGeometry = hasActiveDynamicOverlay ? activeDynamicGeometry : nullptr;
				emissiveSamplingContext.dynamicPrimitiveBaseOffset = (uint32_t)(runtimeSpaceLinkGeometry.primitives.size() + runtimeMutationGeometry.primitives.size());
				if (accelerationReady)
				{
					nri::TopLevelInstance dynamicInstance = {};
					dynamicInstance.transform[0][0] = 1.0f;
					dynamicInstance.transform[1][1] = 1.0f;
					dynamicInstance.transform[2][2] = 1.0f;
					dynamicInstance.instanceId = (uint32_t)sceneInstances.size();
					dynamicInstance.mask = 0xFF;
					dynamicInstance.shaderBindingTableLocalOffset = 0;
					dynamicInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
					dynamicInstance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);
					instances.push_back(dynamicInstance);
					sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, 0u });

					accelerationReady =
						BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static | SceneDataBufferMask_Dynamic) &&
						UpdateSceneDataSet(
							mStaticVertexBuffer,
							mStaticIndexBuffer,
							mStaticPrimitiveBuffer,
							mStaticMaterialBuffer,
							mVertexBuffer,
							mIndexBuffer,
							mPrimitiveBuffer,
							mMaterialBuffer,
							sceneInstances,
							(uint32_t)mStaticMapScene.geometry.primitives.size(),
							(uint32_t)overlayGeometry.primitives.size(),
							(uint32_t)mStaticMapScene.gpuMaterials.size(),
							(uint32_t)dynamicGpuMaterials.size());
				}
			}

			if (overlayGeometry.primitives.empty() || texturesReady)
			{
				PrepareSceneTextureInputsForCompute();
			}

			if (paletteReady && texturesReady && buffersReady && accelerationReady)
			{
				mUsedDynamicSceneLastFrame = hasActiveDynamicOverlay;
				mGpuSceneHasDynamicOverlay = true;
				if (activeDynamicSceneView != nullptr && activeDynamicGeometry != nullptr && activeDynamicMaterials != nullptr)
				{
					mDynamicSceneLastFrame.spriteSurfaceCount = (uint32_t)activeDynamicSceneView->opaqueSprites.size();
					mDynamicSceneLastFrame.primitiveCount = (uint32_t)activeDynamicGeometry->primitives.size();
					mDynamicSceneLastFrame.materialCount = (uint32_t)activeDynamicMaterials->materials.size();
					mDynamicSceneLastFrame.modelCount = activeDynamicSceneView->stats.modelDrawItems;
					mDynamicSceneLastFrame.unsupportedModelCount = activeDynamicSceneView->stats.unsupportedModelDrawItems;
				}
				if (!overlayGeometry.primitives.empty())
				{
					combinedGeometry = mStaticMapScene.geometry;
					AppendGeometry(overlayGeometry, (uint32_t)mStaticMapScene.materialBridge.materials.size(), combinedGeometry);
					activeGeometry = &combinedGeometry;
					activeGpuMaterials = &combinedGpuMaterials;
					activeMaterialBridge = &combinedMaterialBridge;
				}
				else
				{
					activeGeometry = &mStaticMapScene.geometry;
					activeGpuMaterials = &mStaticMapScene.gpuMaterials;
					activeMaterialBridge = &mStaticMapScene.materialBridge;
				}

				activeStats = MergeSceneStats(
					mStaticMapScene.sceneView.stats,
					activeDynamicSceneView != nullptr ? activeDynamicSceneView->stats : nri_scene::SceneDebugStats{});
			}
			else
			{
				LogFallback("PT runtime/dynamic overlay update failed; tracing the resident static world only.");
				if (mGpuSceneHasDynamicOverlay)
				{
					RestoreStaticTopLevelScene();
				}
				paletteReady = true;
				texturesReady = true;
				buffersReady = true;
				accelerationReady = true;
			}
		}
		else if (deferOverlayThisFrame)
		{
			Printf("NRI PT dynamic scene deferred: skipping dynamic overlay on the same frame that rebuilt resident static map assets.\n");
		}
		else if (mGpuSceneHasDynamicOverlay)
		{
			DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
			if (!RestoreStaticTopLevelScene())
			{
				LogFallback("PT static scene restore failed after dynamic overlay.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}

			mGpuSceneHasDynamicOverlay = false;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
		ResetPersistentDynamicEmissiveCache();
		Clocker clock(NriPTSceneCapture);
		if (!nri_scene::CaptureScene(di, capturedSceneView))
		{
			LogFallback("PT scene capture failed.");
			if (preserveHistory)
			{
				restoreHistory();
			}
			return false;
		}

		activeSceneView = &capturedSceneView;
		activeMaterialBridge = &materialBridge;
		sceneLightCapturedView = &capturedSceneView;
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
			AssignGeometryPortalIndices(mMapWorld, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			nri_scene::BuildMaterials(capturedSceneView, materialBridge);
		}
		sceneLightCapturedMaterials = &materialBridge;

		const bool needsFallbackMaterials = bootstrapCapturedDiagnostics || bootstrapCapturedFlat;
		const bool needsRealTextures = !nri_ptbootstrap || bootstrapCapturedBaseColor || bootstrapMode >= 13u;
		paletteReady = needsRealTextures ? EnsurePaletteTexture(materialBridge) : true;
		texturesReady = needsFallbackMaterials ? UseFallbackSceneTextures(preserveHistory) : (needsRealTextures ? (paletteReady && EnsureSceneTextures(capturedSceneView, materialBridge, capturedGpuMaterials, preserveHistory)) : EnsureSkyTexture(capturedSceneView, preserveHistory));
		if (needsFallbackMaterials)
		{
			capturedGpuMaterials = materialBridge.materials;
			for (auto& material : capturedGpuMaterials)
			{
				material.textureIndex = 0;
				material.paletteIndex = 0;
				material.flags = 0;
				material.lightLevel = 1.0f;
				material.alpha = 1.0f;
			}
		}
		else if (!needsRealTextures)
		{
			capturedGpuMaterials = materialBridge.materials;
		}

		buffersReady = texturesReady && UploadSceneBuffers(capturedGeometry, capturedGpuMaterials);
		std::vector<SceneInstanceData> sceneInstances;
		if (buffersReady)
		{
			sceneInstances.push_back({ 0u, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u, 0u });
			buffersReady = UpdateSceneDataSet(
				mVertexBuffer,
				mIndexBuffer,
				mPrimitiveBuffer,
				mMaterialBuffer,
				mVertexBuffer,
				mIndexBuffer,
				mPrimitiveBuffer,
				mMaterialBuffer,
				sceneInstances,
				0u,
				(uint32_t)capturedGeometry.primitives.size(),
				0u,
				(uint32_t)capturedGpuMaterials.size());
		}
		if (texturesReady)
		{
			PrepareSceneTextureInputsForCompute();
		}
		if (bootstrapCapturedView || rawTraceDirectScene)
		{
			accelerationReady = true;
		}
		else if (buffersReady)
		{
			accelerationReady =
				BuildDynamicAccelerationStructure(capturedGeometry) &&
				mDynamicBottomLevelAS.accelerationStructure != nullptr;
			if (accelerationReady)
			{
				nri::TopLevelInstance instance = {};
				instance.transform[0][0] = 1.0f;
				instance.transform[1][1] = 1.0f;
				instance.transform[2][2] = 1.0f;
				instance.instanceId = 0;
				instance.mask = 0xFF;
				instance.shaderBindingTableLocalOffset = 0;
				instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
				instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);

				std::vector<nri::TopLevelInstance> instances = { instance };
				accelerationReady = BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Dynamic);
			}
		}
		else
		{
			accelerationReady = false;
		}
		activeGeometry = &capturedGeometry;
		activeGpuMaterials = &capturedGpuMaterials;
		emissiveSamplingContext.capturedGeometry = &capturedGeometry;
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr || activeMaterialBridge == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	RefreshSceneLightSystem(
		sceneLightUsesStaticMapScene,
		sceneLightCapturedView,
		sceneLightCapturedMaterials,
		sceneLightDynamicView,
		sceneLightDynamicMaterials);

	if (sceneLightUsesStaticMapScene && !mGpuSceneHasDynamicOverlay)
	{
		const bool needsResidentStaticLightRefresh =
			!mSceneLights.GetAnalyticLights().activeLights.empty() ||
			mBoundRuntimeLightCount != 0 ||
			mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
			mBoundSectorLightActiveCount != 0;
		if (needsResidentStaticLightRefresh)
		{
			if (!RefreshResidentStaticSceneDataSet())
			{
				LogFallback("PT static scene light refresh failed.");
				if (preserveHistory)
				{
					restoreHistory();
				}
				return false;
			}
		}
	}

	if (!UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		LogFallback("PT emissive primitive update failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}
	if (!BuildEmissiveTopLevelAccelerationStructure())
	{
		LogFallback("PT emissive TLAS update failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	TraceRuntimeLinkEvents(di);
	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, activeMaterialBridge, true);
	}
	if (activeGeometry->primitives.empty())
	{
		LogFallback("PT scene path produced no supported opaque geometry.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	if (mUsedStaticMapSceneLastFrame)
	{
		PrepareSceneTextureInputsForCompute();
	}

	bool dispatched = false;
	if (bootstrapCapturedView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		dispatched = buffersReady && DispatchBootstrapView();
	}
	else
	{
		dispatched = accelerationReady && DispatchFrameGraph(di, *activeGeometry, *activeGpuMaterials, drawmode);
	}
	const bool success = paletteReady && texturesReady && buffersReady && accelerationReady && dispatched;

	if (!paletteReady)
	{
		LogFallback("PT palette texture upload failed.");
	}
	else if (!texturesReady)
	{
		LogFallback("PT material texture upload failed.");
	}
	else if (!buffersReady)
	{
		LogFallback("PT scene buffer upload failed.");
	}
	else if (!accelerationReady)
	{
		LogFallback("PT acceleration structure build failed.");
	}
	else if (!dispatched)
	{
		LogFallback(bootstrapCapturedView ? "PT bootstrap captured-scene dispatch failed." : "PT frame graph dispatch failed.");
	}

	if (success)
	{
		mHasLoggedFallback = false;
		if (bootstrapCapturedView)
		{
			CopyFinalToActiveTarget();
		}

		if (!preserveHistory)
		{
			mFrameIndex++;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			restoreHistory();
		}
	}
	else if (preserveHistory)
	{
		restoreHistory();
	}

	return success;
}

void NRIRenderer::ResetHistory()
{
	RequestHistoryReset("history-reset", true, true);
}

void NRIRenderer::RequestHistoryReset(const char* reason, bool clearPreviousCameraState, bool clearRuntimeChunkTranslationHistory)
{
	ArmTemporalTraceBudget(reason);
	mResetHistory = true;
	if (clearPreviousCameraState)
	{
		mHasPreviousCameraState = false;
	}
	if (clearRuntimeChunkTranslationHistory)
	{
		mRuntimeChunkTranslationHistory.clear();
	}
}

bool NRIRenderer::AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	if (mSceneLights.GetManualAnalyticLightCount() >= NRI_MAX_RUNTIME_POINT_LIGHTS)
	{
		return false;
	}

	outId = mNextRuntimePointLightId++;
	if (!mSceneLights.AddManualAnalyticLight(outId, position, color, intensity, radius))
	{
		return false;
	}
	mBoundRuntimeLightCount = 0;
	RequestHistoryReset("runtime-light-change");
	return true;
}

bool NRIRenderer::RemoveRuntimePointLight(uint32_t id)
{
	if (!mSceneLights.RemoveManualAnalyticLight(id))
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;
	RequestHistoryReset("runtime-light-change");
	return true;
}

void NRIRenderer::ClearRuntimePointLights()
{
	if (mSceneLights.GetManualAnalyticLightCount() == 0)
	{
		return;
	}

	mSceneLights.ClearManualAnalyticLights();
	mBoundRuntimeLightCount = 0;
	RequestHistoryReset("runtime-light-change");
}

void NRIRenderer::PrintRuntimePointLights() const
{
	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	Printf("NRI PT analytic lights: active=%u manual=%u rules=%u matched_surfaces=%u deduped=%u truncated=%u limit=%u\n",
		(uint32_t)analyticLights.activeLights.size(),
		(uint32_t)analyticLights.manualLights.size(),
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.matchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount,
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	if (analyticLights.activeLights.empty())
	{
		return;
	}

	for (const SceneLightSystem::SceneAnalyticLight& light : analyticLights.activeLights)
	{
		Printf("NRI PT analytic light %u: id=%u stable=0x%016llx source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			(light.sourceFlags & SceneAnalyticLightSourceFlag_Manual) != 0 ? "manual" : "heuristic",
			(light.sourceFlags & SceneAnalyticLightSourceFlag_SpriteTileHeuristic) != 0 ? ":sprite_tile" : "",
			light.sourceRuleId,
			light.actorIndex,
			light.textureId,
			light.position[0],
			light.position[1],
			light.position[2],
			light.color[0],
			light.color[1],
			light.color[2],
			light.intensity,
			light.radius);
	}
}

void NRIRenderer::PrintRuntimeLightClusterStatus() const
{
	const uint32_t tileCount = mBoundRuntimeLightTileCountX * mBoundRuntimeLightTileCountY;
	const uint32_t centerTileX = mBoundRuntimeLightTileCountX > 0 ? (mBoundRuntimeLightTileCountX - 1) / 2u : 0u;
	const uint32_t centerTileY = mBoundRuntimeLightTileCountY > 0 ? (mBoundRuntimeLightTileCountY - 1) / 2u : 0u;
	uint32_t centerTileCount = 0;
	if (mRuntimeLightTileHeaderBuffer.buffer != nullptr &&
		mBoundRuntimeLightTileCountX > 0 &&
		mBoundRuntimeLightTileCountY > 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*mRuntimeLightTileHeaderBuffer.buffer, 0, mRuntimeLightTileHeaderBuffer.usedSize);
		if (mapped != nullptr)
		{
			const auto* headers = reinterpret_cast<const RuntimeLightTileHeaderGpuData*>(mapped);
			const uint32_t centerIndex = centerTileY * mBoundRuntimeLightTileCountX + centerTileX;
			if ((uint64_t)(centerIndex + 1) * sizeof(RuntimeLightTileHeaderGpuData) <= mRuntimeLightTileHeaderBuffer.usedSize)
			{
				centerTileCount = headers[centerIndex].indexCount;
			}
			mFrameBuffer->mCore.UnmapBuffer(*mRuntimeLightTileHeaderBuffer.buffer);
		}
	}

	Printf("NRI PT light clusters: tile_size=%u grid=%ux%u tiles=%u active_lights=%u used_indices=%u max_occupancy=%u center_tile=(%u,%u) center_count=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		tileCount,
		mBoundRuntimeLightCount,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		centerTileX,
		centerTileY,
		centerTileCount,
		NRI_PTDEBUG_ANALYTIC_DIRECT);
}

uint32_t NRIRenderer::GetRuntimePointLightCount() const
{
	return mSceneLights.GetManualAnalyticLightCount();
}

bool NRIRenderer::AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (!mSceneLights.AddSpriteTileHeuristic(textureId, color, intensity, radius, flickerFrames, outRuleId))
	{
		return false;
	}

	RequestHistoryReset("analytic-light-heuristic-change");
	return true;
}

void NRIRenderer::ClearSpriteTileLightHeuristics()
{
	if (mSceneLights.GetAnalyticLights().spriteTileRules.empty())
	{
		return;
	}

	mSceneLights.ClearSpriteTileHeuristics();
	RequestHistoryReset("analytic-light-heuristic-change");
}

void NRIRenderer::PrintSpriteTileLightHeuristics() const
{
	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	Printf("NRI PT analytic sprite-tile heuristics: rules=%u matched_surfaces=%u deduped=%u truncated=%u\n",
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.matchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount);
	for (const auto& rule : analyticLights.spriteTileRules)
	{
		Printf("NRI PT analytic heuristic %u: tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
			rule.ruleId,
			rule.textureId,
			rule.color[0],
			rule.color[1],
			rule.color[2],
			rule.intensity,
			rule.radius,
			rule.flickerFrames);
	}
}

bool NRIRenderer::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (!mSceneLights.AddTextureEmissiveHeuristic(textureId, emissiveMode, intensityScale, emissiveColor, hasExplicitColor, outRuleId))
	{
		return false;
	}

	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialsDirty();
	RequestHistoryReset("emissive-heuristic-change");
	return true;
}

void NRIRenderer::ClearTextureEmissiveHeuristics()
{
	if (mSceneLights.GetEmissiveSurfaces().textureRules.empty())
	{
		return;
	}

	mSceneLights.ClearTextureEmissiveHeuristics();
	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialsDirty();
	RequestHistoryReset("emissive-heuristic-change");
}

void NRIRenderer::PrintTextureEmissiveHeuristics() const
{
	const auto& emissive = mSceneLights.GetEmissiveSurfaces();
	Printf("NRI PT emissive heuristics: rules=%u auto_tagged=%u explicit_matches=%u active=%u total_power=%.3f truncated=%u\n",
		(uint32_t)emissive.textureRules.size(),
		emissive.autoTaggedCount,
		emissive.explicitRuleMatchCount,
		(uint32_t)emissive.activeSurfaces.size(),
		emissive.totalPowerEstimate,
		emissive.truncatedSurfaceCount);
	for (const auto& rule : emissive.textureRules)
	{
		Printf("NRI PT emissive heuristic %u: tile=%u mode=%s intensity_scale=%.3f explicit_color=%s color=(%.3f, %.3f, %.3f)\n",
			rule.ruleId,
			rule.textureId,
			GetMaterialEmissiveModeName(rule.emissiveMode),
			rule.intensityScale,
			rule.hasExplicitColor ? "yes" : "no",
			rule.emissiveColor[0],
			rule.emissiveColor[1],
			rule.emissiveColor[2]);
	}
}

void NRIRenderer::PrintEmissiveSurfaceDump(float radius, uint32_t limit) const
{
	if (mBoundEmissivePrimitiveRecords.empty())
	{
		Printf("NRI PT emissive primitives: no emissive primitive candidates are bound.\n");
		return;
	}

	struct Candidate
	{
		const EmissivePrimitiveDebugRecord* record = nullptr;
		float distanceSq = 0.0f;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(mBoundEmissivePrimitiveRecords.size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;
	for (const auto& record : mBoundEmissivePrimitiveRecords)
	{
		const float dx = record.center[0] - mCurrentCameraPos[0];
		const float dy = record.center[1] - mCurrentCameraPos[1];
		const float dz = record.center[2] - mCurrentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}
		candidates.push_back({ &record, distanceSq });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		return a.distanceSq < b.distanceSq;
	});

	Printf("NRI PT emissive primitives: active=%u source_surfaces=%u auto=%u explicit=%u total_power=%.3f min_surface=%.3f min_power=%.3f sampling_auto_only=%s\n",
		(uint32_t)mBoundEmissivePrimitiveRecords.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mBoundEmissiveTotalPower,
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		nri_ptemissiveautoonly ? "on" : "off");

	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const auto& record = *candidates[i].record;
		Printf("NRI PT emissive %u: stable=0x%016llx source=%s primitive=%u material=%u flags=0x%x rule=%u actor=%d tile=%u mode=%s emissive_tex=%u area=%.2f power=%.3f pdf=%.6f center=(%.2f, %.2f, %.2f) color=(%.3f, %.3f, %.3f) intensity=%.3f\n",
			i,
			(unsigned long long)record.stableKey,
			GetSceneDataSourceName(record.dataSource),
			record.primitiveIndex,
			record.materialIndex,
			record.sourceFlags,
			record.sourceRuleId,
			record.actorIndex,
			record.textureId,
			GetMaterialEmissiveModeName(record.emissiveMode),
			record.emissiveTextureIndex != UINT32_MAX ? record.emissiveTextureIndex : 0u,
			record.primitiveArea,
			record.powerEstimate,
			record.selectionPdf,
			record.center[0],
			record.center[1],
			record.center[2],
			record.emissiveColor[0],
			record.emissiveColor[1],
			record.emissiveColor[2],
			record.emissiveIntensity);
	}
}

void NRIRenderer::PrintSectorLightDump(float radius, uint32_t limit) const
{
	const auto& registry = mSceneLights.GetSectorLighting();
	if (registry.activeSectorIndices.empty())
	{
		Printf("NRI PT sector lights: no active sector-light records are available.\n");
		return;
	}

	struct SectorCandidate
	{
		uint32_t sectorIndex = UINT32_MAX;
		float distanceSq = std::numeric_limits<float>::max();
		float center[3] = {};
	};

	std::vector<float> centerSums((size_t)registry.sectorCount * 3u, 0.0f);
	std::vector<uint32_t> centerCounts(registry.sectorCount, 0u);
	for (const auto& record : mSceneLights.GetSurfaceRecords())
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= registry.sectorCount)
		{
			continue;
		}

		centerSums[(size_t)sectorIndex * 3u + 0u] += record.center[0];
		centerSums[(size_t)sectorIndex * 3u + 1u] += record.center[1];
		centerSums[(size_t)sectorIndex * 3u + 2u] += record.center[2];
		centerCounts[sectorIndex]++;
	}

	std::vector<SectorCandidate> candidates;
	candidates.reserve(registry.activeSectorIndices.size());
	const float radiusSq = radius > 0.0f ? radius * radius : std::numeric_limits<float>::max();
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectorCount || sectorIndex >= centerCounts.size() || centerCounts[sectorIndex] == 0u)
		{
			continue;
		}

		SectorCandidate candidate = {};
		candidate.sectorIndex = sectorIndex;
		const float invCount = 1.0f / (float)centerCounts[sectorIndex];
		candidate.center[0] = centerSums[(size_t)sectorIndex * 3u + 0u] * invCount;
		candidate.center[1] = centerSums[(size_t)sectorIndex * 3u + 1u] * invCount;
		candidate.center[2] = centerSums[(size_t)sectorIndex * 3u + 2u] * invCount;
		const float dx = candidate.center[0] - mCurrentCameraPos[0];
		const float dy = candidate.center[1] - mCurrentCameraPos[1];
		const float dz = candidate.center[2] - mCurrentCameraPos[2];
		candidate.distanceSq = dx * dx + dy * dy + dz * dz;
		if (candidate.distanceSq <= radiusSq)
		{
			candidates.push_back(candidate);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const SectorCandidate& a, const SectorCandidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.sectorIndex < b.sectorIndex;
	});

	Printf("NRI PT sector lights: active=%u eligible=%u fog=%u pulsing=%u radius=%.1f limit=%u scales=(%.3f, %.3f, %.3f) clamp=%.3f filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		registry.activeSectorCount,
		registry.eligibleSectorCount,
		registry.fogSectorCount,
		registry.pulsingSectorCount,
		radius,
		limit,
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);

	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SectorCandidate& candidate = candidates[i];
		const auto& entry = registry.sectors[candidate.sectorIndex];
		Printf("NRI PT sector light %u: sector=%u dist=%.2f center=(%.2f, %.2f, %.2f) ambient=(%.3f, %.3f, %.3f)*%.3f hemi=%.3f fog=%.3f pulse=%.3f palette=%d shade=%d lotag=%d hitag=%d flags=0x%x\n",
			i,
			candidate.sectorIndex,
			std::sqrt(candidate.distanceSq),
			candidate.center[0],
			candidate.center[1],
			candidate.center[2],
			entry.ambientColor[0],
			entry.ambientColor[1],
			entry.ambientColor[2],
			entry.ambientIntensity,
			entry.hemisphereAmount,
			entry.fogAmount,
			entry.pulseScale,
			entry.paletteIndex,
			entry.averageShade,
			entry.lotag,
			entry.hitag,
			entry.sourceFlags);
	}

	if (printCount == 0)
	{
		Printf("NRI PT sector lights: no active sector lights matched the requested radius.\n");
	}
}

void NRIRenderer::PrintStatus() const
{
	const NRIUpscalerKind requested = GetSelectedUpscalerKind();
	const NRIUpscalerKind resolved = GetResolvedUpscalerKindForStatus();
	const uint32_t bootstrapMode = GetBootstrapMode();
	const uint32_t nrdMaxFrames = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);
	const uint32_t nrdFastFrames = ClampNrdFastFrameCount((int)nri_nrdfastframes, nrdMaxFrames);
	const uint32_t nrdStabilizationFrames = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, nrdMaxFrames);
	const uint32_t nrdHitDistanceReconstruction = GetNrdHitDistanceReconstructionMode();
	const uint32_t nrdInputSplit = GetNrdInputSplitMode();
	const NRINrdDenoiserMode nrdDenoiserMode = GetSelectedNrdDenoiserMode();
	const float nrdFastHistorySigma = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	const float nrdDiffusePrepass = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	const float nrdSpecularPrepass = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	const float nrdMinBlur = ClampNrdBlurRadius((float)nri_nrdblurmin);
	const float nrdMaxBlur = std::max(nrdMinBlur, ClampNrdBlurRadius((float)nri_nrdblurmax));
	const uint32_t sigmaStabilizationFrames = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	const float sigmaPlaneDistance = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);

	Printf("NRI PT status: support=%s", mPathTracingSupported ? "available" : "raster-fallback");
	if (!mPathTracingSupported)
	{
		Printf(" (%s)", GetAvailabilityReason());
	}
	Printf("\n");
	Printf("NRI PT frame: index=%u render=%ux%u output=%ux%u prev_camera=%s reset_history=%s\n",
		mFrameIndex,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		mHasPreviousCameraState ? "yes" : "no",
		mResetHistory ? "yes" : "no");
	Printf("NRI PT features: bootstrap=%s denoise=%s validation=%s api_validation=%s dred=%s upscaler=%s->%s mode=%s render_scale=%.3f sharpness=%.3f\n",
		nri_ptbootstrap ? "on" : "off",
		nri_denoise ? "on" : "off",
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		GetUpscalerName(requested),
		GetUpscalerName(resolved),
		GetUpscalerModeName(GetSelectedUpscalerMode()),
		(float)nri_renderscale,
		(float)nri_sharpness);
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d\n",
		nri_ptdirectscene ? "on" : "off",
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		ClampTraceBounceCount((int)nri_ptportaldepth, 8u),
		(int)nri_ptsurfaceprobe);
	Printf("NRI PT lighting shell: directional_placeholder=%s sector=%s emissive_heuristics=%s\n",
		nri_ptdirectionallight ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off",
		nri_ptemissiveheuristics ? "on" : "off");
	uint32_t emissiveBaseCount = 0;
	uint32_t emissiveConstantCount = 0;
	uint32_t emissiveGlowmapCount = 0;
	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		switch (surface.emissiveMode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: emissiveBaseCount++; break;
		case nri_scene::MaterialEmissiveMode_UseConstantColor: emissiveConstantCount++; break;
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: emissiveGlowmapCount++; break;
		default: break;
		}
	}
	Printf("NRI PT NRD: integration=%s requested=%s validation_output=%s denoiser=%s motion=%s prev_position=%s extra_debugs=%s\n",
		mNrd.IsReady() ? "ready" : "cold",
		nri_denoise ? "on" : "off",
		nri_validation ? "expected" : "disabled",
		GetNrdDenoiserModeName(nrdDenoiserMode),
		"2.5D",
		"interpolated",
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=current_reprojection_error 35=hit_position_delta 36=basis_reprojection_error 37=jitter_comp_probe 38=uv_reference_probe 39=previous_position_delta 40=current_ray_closure 41=motion_xy_buffer 42=motion_reprojection_error 43=basis_motion_reprojection_error 44=motion_xy_raw 45=motion_reprojection_raw");
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		nrdMaxFrames,
		nrdFastFrames,
		nrdStabilizationFrames,
		nri_nrdantifirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(nrdHitDistanceReconstruction),
		GetNrdInputSplitModeName(nrdInputSplit),
		mUseSplitShadowDenoiser ? "sigma-debug" : "off");
	Printf("NRI PT SIGMA tuning: stabilization_frames=%u plane_distance_sensitivity=%.3f\n",
		sigmaStabilizationFrames,
		sigmaPlaneDistance);
	if (nrdDenoiserMode == NRINrdDenoiserMode::Relax)
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f prepass=%.2f/%.2f material_floor=1/2 blur_radius=n/a_relax\n",
			nrdFastHistorySigma,
			nrdDiffusePrepass,
			nrdSpecularPrepass);
	}
	else
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f blur_radius=%.2f..%.2f prepass=%.2f/%.2f material_floor=1/2\n",
			nrdFastHistorySigma,
			nrdMinBlur,
			nrdMaxBlur,
			nrdDiffusePrepass,
			nrdSpecularPrepass);
	}
	Printf("NRI PT NRD guides: diffuse_signal=demodulated_illumination hit_distance=%s roughness=material_hint metalness=material_hint material_id=semantic_class\n",
		nrdDenoiserMode == NRINrdDenoiserMode::Relax ? "secondary_transport_linear_hitdist" : "secondary_transport_reblur_norm");
	Printf("NRI PT scene stats: %s\n", nri_ptscenestats ? "on" : "off");
	Printf("NRI PT mutation trace: chunk=%d sector=%d\n",
		(int)nri_ptmutationtracechunk,
		(int)nri_ptmutationtracesector);
	Printf("NRI PT runtime link trace: %s\n", nri_ptruntimelinktrace ? "on" : "off");
	Printf("NRI PT analytic lights: active=%u manual=%u rules=%u limit=%u\n",
		(uint32_t)mSceneLights.GetAnalyticLights().activeLights.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().manualLights.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().spriteTileRules.size(),
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	Printf("NRI PT analytic clusters: tile=%u grid=%ux%u used_indices=%u max_occupancy=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		NRI_PTDEBUG_ANALYTIC_DIRECT);
	Printf("NRI PT emissive surfaces: active=%u rules=%u auto=%u explicit=%u total_power=%.3f debug_mode=%u/%u thresholds=area>=%.3f power>=%.3f heuristics=%s sampling_auto_only=%s\n",
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().textureRules.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().totalPowerEstimate,
		NRI_PTDEBUG_EMISSIVE_TAGS,
		NRI_PTDEBUG_EMISSIVE_DIRECT,
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		nri_ptemissiveheuristics ? "on" : "off",
		nri_ptemissiveautoonly ? "on" : "off");
	Printf("NRI PT emissive sources: base=%u glowmap=%u constant=%u\n",
		emissiveBaseCount,
		emissiveGlowmapCount,
		emissiveConstantCount);
	Printf("NRI PT emissive sampling: primitives=%u total_power=%.3f samples=%u dominant_tile=%u dominant_primitive=%u dominant_source=%s dominant_power=%.3f dominant_flags=0x%x debug_modes=%u/%u/%u/%u\n",
		mBoundEmissivePrimitiveCount,
		mBoundEmissiveTotalPower,
		std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u),
		mBoundEmissiveDominantTile,
		mBoundEmissiveDominantPrimitive,
		GetSceneDataSourceName(mBoundEmissiveDominantDataSource),
		mBoundEmissiveDominantPower,
		mBoundEmissiveDominantFlags,
		NRI_PTDEBUG_EMISSIVE_SAMPLE_UV,
		NRI_PTDEBUG_EMISSIVE_SAMPLE_RADIANCE,
		NRI_PTDEBUG_EMISSIVE_SAMPLE_PRIMITIVE,
		NRI_PTDEBUG_EMISSIVE_SAMPLE_VISIBILITY);
	Printf("NRI PT emissive query: tlas=%s fast_shadow=%s instances=%u static=%u dynamic=%u builds=%u\n",
		nri_ptemissivetlas ? "on" : "off",
		nri_ptemissivefastshadow ? "on" : "off",
		mEmissiveTlasInstanceCount,
		mEmissiveTlasStaticInstanceCount,
		mEmissiveTlasDynamicInstanceCount,
		mEmissiveTlasBuildCount);
	Printf("NRI PT sector lighting: enabled=%s active=%u eligible=%u fog=%u pulsing=%u debug_mode=%u scales=ambient=%.3f hemi=%.3f fog=%.3f clamp=%.3f filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		nri_ptsectorlighting ? "on" : "off",
		mSceneLights.GetSectorLighting().activeSectorCount,
		mSceneLights.GetSectorLighting().eligibleSectorCount,
		mSceneLights.GetSectorLighting().fogSectorCount,
		mSceneLights.GetSectorLighting().pulsingSectorCount,
		NRI_PTDEBUG_SECTOR_AMBIENT,
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);
	Printf("NRI PT sector buffer: sectors=%u active=%u pulsing=%u dominant_sector=%u dominant_contribution=%.3f\n",
		mBoundSectorLightSectorCount,
		mBoundSectorLightActiveCount,
		mBoundSectorLightPulsingCount,
		mBoundSectorLightDominantSector != UINT32_MAX ? mBoundSectorLightDominantSector : 0u,
		mBoundSectorLightDominantContribution);
	if (nri_ptbootstrap)
	{
		Printf("NRI PT bootstrap mode: %u\n", bootstrapMode);
	}

	if (mHasLoggedStats)
	{
		const auto& stats = mLastStats;
		Printf("NRI PT last scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
	}
	else
	{
		Printf("NRI PT last scene: no translated PT scene has been captured yet.\n");
	}

	PrintMapWorldStatus();
	PrintPortalTraversalStatus();
	PrintStaticMapSceneStatus();
	PrintDynamicSceneStatus();
	PrintTemporalStatus();
	PrintRuntimeMapMutationStatus();
	PrintRuntimeSpaceLinkStatus();
	PrintSceneBufferStatus();
	PrintSurfaceProbeStatus();
}

void NRIRenderer::PrintTemporalStatus() const
{
	const NRIUpscalerKind requested = GetSelectedUpscalerKind();
	const NRIUpscalerKind resolved = GetResolvedUpscalerKindForStatus();
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	Printf("NRI PT temporal: debug=%d requested=%s resolved=%s taa=%s last_debug=%d last_temporal=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		(int)nri_ptdebug,
		GetUpscalerName(requested),
		GetUpscalerName(resolved),
		nri_pttaa ? "on" : "off",
		mLastDebugMode,
		GetUpscalerName(mLastTemporalHistoryUpscaler),
		mResetHistory ? "yes" : "no",
		mHasPreviousCameraState ? "yes" : "no",
		GetFrameTextureSlotName(mHistoryInputSlot),
		historyInput.width,
		historyInput.height,
		(uint32_t)historyInput.state.access,
		(uint32_t)historyInput.state.layout,
		(uint32_t)historyInput.state.stages,
		GetFrameTextureSlotName(mHistoryOutputSlot),
		historyOutput.width,
		historyOutput.height,
		(uint32_t)historyOutput.state.access,
		(uint32_t)historyOutput.state.layout,
		(uint32_t)historyOutput.state.stages,
		GetFrameTextureSlotName(presentSlot),
		GetFrameTextureSlotName(mUpscaledInputSlot),
		mUseUpscaledInFinal ? "yes" : "no");
}

void NRIRenderer::ArmTemporalTraceBudget(const char* reason)
{
	if ((int)nri_pttraceframes >= NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT)
	{
		return;
	}

	nri_pttraceframes = NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT;
	Printf("NRI PT temporal trace: armed=%d reason=%s frame=%u debug=%d resolved=%s\n",
		(int)nri_pttraceframes,
		reason != nullptr ? reason : "unspecified",
		mFrameIndex,
		(int)nri_ptdebug,
		GetUpscalerName(ResolveUpscalerKind(false)));
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIUpscalerKind resolvedUpscaler, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (nri_pttraceframes <= 0)
	{
		return;
	}

	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	const NRITextureResource& primary = GetFrameTexture(primarySlot);
	const NRITextureResource& secondary = secondarySlot == FrameTextureSlot::Count ? GetFrameTexture(mHistoryOutputSlot) : GetFrameTexture(secondarySlot);
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved=%s run_app_taa=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		stage != nullptr ? stage : "unknown",
		mFrameIndex,
		(int)nri_ptdebug,
		GetUpscalerName(resolvedUpscaler),
		runAppTaa ? "yes" : "no",
		mResetHistory ? "yes" : "no",
		mHasPreviousCameraState ? "yes" : "no",
		GetFrameTextureSlotName(mHistoryInputSlot),
		historyInput.width,
		historyInput.height,
		(uint32_t)historyInput.state.access,
		(uint32_t)historyInput.state.layout,
		(uint32_t)historyInput.state.stages,
		GetFrameTextureSlotName(mHistoryOutputSlot),
		historyOutput.width,
		historyOutput.height,
		(uint32_t)historyOutput.state.access,
		(uint32_t)historyOutput.state.layout,
		(uint32_t)historyOutput.state.stages,
		GetFrameTextureSlotName(primarySlot),
		primary.width,
		primary.height,
		(uint32_t)primary.state.access,
		(uint32_t)primary.state.layout,
		(uint32_t)primary.state.stages,
		GetFrameTextureSlotName(secondarySlot == FrameTextureSlot::Count ? mHistoryOutputSlot : secondarySlot),
		secondary.width,
		secondary.height,
		(uint32_t)secondary.state.access,
		(uint32_t)secondary.state.layout,
		(uint32_t)secondary.state.stages,
		mUseUpscaledInFinal ? "yes" : "no");
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u local_spaces=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portal_surfaces=%u portal_graph=%u portal_targets=%u wall_portals=%u sector_portals=%u mirror_portals=%u runtime_portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.localSpaceCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.portalCount,
		stats.portalTargetCount,
		stats.wallPortalCount,
		stats.sectorPortalCount,
		stats.mirrorPortalCount,
		stats.runtimePortalCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

void NRIRenderer::PrintPortalTraversalStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT portal traversal: no authoritative portal graph is available.\n");
		return;
	}

	Printf("NRI PT portal traversal: depth=%u reflective=%u transfer=%u runtime_bound=%u hittable_surfaces=%u plane_portals_pending=%u\n",
		ClampTraceBounceCount((int)nri_ptportaldepth, 8u),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER),
		CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND),
		mMapWorld.stats.portalSurfaceCount,
		CountPendingPlanePortals(mMapWorld));
}

void NRIRenderer::PrintStaticMapSceneStatus() const
{
	const char* source = mUsedStaticMapSceneLastFrame ? "authoritative-map-world" : "captured-scene";
	Printf("NRI PT static scene: source=%s resident=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u reuses=%u last_frame_upload=%s last_frame_as_build=%s chunks=%u tlas_instances=%u tris=%u materials=%u\n",
		source,
		(mStaticMapScene.valid && mStaticMapScene.texturesResident && mStaticMapScene.buffersResident && mStaticMapScene.accelerationResident) ? "yes" : "no",
		(unsigned long long)mStaticMapScene.buildSerial,
		mStaticMapScene.sceneBuildCount,
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount,
		mStaticMapScene.reuseCount,
		mUploadedStaticMapSceneLastFrame ? "yes" : "no",
		mBuiltStaticMapSceneASLastFrame ? "yes" : "no",
		(uint32_t)mStaticMapScene.chunks.size(),
		mStaticMapScene.tlasInstanceCount,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size());
}

void NRIRenderer::PrintDynamicSceneStatus() const
{
	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u emissive_cache=%s cache_surfaces=%u cache_tris=%u cache_materials=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount,
		mPersistentDynamicEmissiveCache.valid ? "yes" : "no",
		mPersistentDynamicEmissiveCache.surfaceCount,
		mPersistentDynamicEmissiveCache.primitiveCount,
		mPersistentDynamicEmissiveCache.materialCount);
}

void NRIRenderer::ResetPersistentDynamicEmissiveCache()
{
	mPersistentDynamicEmissiveCache = {};
}

bool NRIRenderer::RebuildPersistentDynamicEmissiveCache(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials)
{
	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = sceneView.drawInfo;
	next.sceneView.sky = sceneView.sky;
	Copy3(sceneView.skyColor, next.sceneView.skyColor);
	Copy3(sceneView.groundColor, next.sceneView.groundColor);

	uint32_t materialIndex = 0;
	auto appendSurfaceList = [this, &materials, &materialIndex](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			const bool keepSurface =
				materialIndex < materials.lightMetadata.size() &&
				mSceneLights.MaterialWouldEmit(materials.lightMetadata[materialIndex]);
			if (keepSurface)
			{
				destination.push_back(surface);
			}
			materialIndex++;
		}
	};

	appendSurfaceList(sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendSurfaceList(sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendSurfaceList(sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		return false;
	}

	{
		Clocker clock(NriPTGeometryBuild);
		nri_scene::BuildGeometry(next.sceneView, next.geometry);
		AssignGeometryPortalIndices(mMapWorld, next.geometry);
	}
	{
		Clocker clock(NriPTMaterialBuild);
		nri_scene::BuildMaterials(next.sceneView, next.materialBridge);
	}

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		return false;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
	return true;
}

void NRIRenderer::PrintRuntimeMapMutationStatus() const
{
	Printf("NRI PT runtime map: active=%s dirty_chunks=%u replaced_chunks=%u rebuilt_chunks=%u held_chunks=%u blind_spots=%u sector_geom=%u sector_mat=%u wall_geom=%u wall_mat=%u sector_dirty=%u section_dirty=%u dragged=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeMapLastFrame.active ? "yes" : "no",
		mRuntimeMapLastFrame.dirtyChunkCount,
		mRuntimeMapLastFrame.replacedChunkCount,
		mRuntimeMapLastFrame.rebuiltChunkCount,
		mRuntimeMapLastFrame.heldChunkCount,
		mRuntimeMapLastFrame.blindSpotChunkCount,
		mRuntimeMapLastFrame.sectorGeometryChunkCount,
		mRuntimeMapLastFrame.sectorMaterialChunkCount,
		mRuntimeMapLastFrame.wallGeometryChunkCount,
		mRuntimeMapLastFrame.wallMaterialChunkCount,
		mRuntimeMapLastFrame.sectorDirtyChunkCount,
		mRuntimeMapLastFrame.sectionDirtyChunkCount,
		mRuntimeMapLastFrame.draggedChunkCount,
		mRuntimeMapLastFrame.replacementSurfaceCount,
		mRuntimeMapLastFrame.replacementTriangleCount,
		mRuntimeMapLastFrame.materialCount);
}

void NRIRenderer::PrintRuntimeSpaceLinkStatus() const
{
	Printf("NRI PT runtime links: active=%s geo_effect=%s query_attempted=%s query_rejected=%s candidate_sector=%d candidate_lotag=%d source_sector=%d reported_geo_count=%d view_roots=%u visible_sectors=%u providers=%u geo_providers=%u provider_groups=%u local_space_matches=%u visible_matches=%u links=%u translated_chunks=%u orphan_local_spaces=%u unresolved_runtime_portals=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeSpaceLinkLastFrame.active ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.geoEffectActive ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryAttempted ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryRejected ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.candidateSectorIndex,
		mRuntimeSpaceLinkLastFrame.candidateSectorLotag,
		mRuntimeSpaceLinkLastFrame.sourceSectorIndex,
		mRuntimeSpaceLinkLastFrame.reportedGeoCount,
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount,
		mRuntimeSpaceLinkLastFrame.visibleSectorCount,
		mRuntimeSpaceLinkLastFrame.providerSectorCount,
		mRuntimeSpaceLinkLastFrame.geoProviderCount,
		mRuntimeSpaceLinkLastFrame.providerGroupCount,
		mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.linkCount,
		mRuntimeSpaceLinkLastFrame.translatedChunkCount,
		mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount,
		mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount,
		mRuntimeSpaceLinkLastFrame.surfaceCount,
		mRuntimeSpaceLinkLastFrame.triangleCount,
		mRuntimeSpaceLinkLastFrame.materialCount);
	Printf("NRI PT runtime link motion: prev_chunk_offsets=%u topology_changed=%s special_material_history=%s\n",
		(uint32_t)mRuntimeChunkTranslationHistory.size(),
		mRuntimeSpaceLinkLastFrame.topologyChanged ? "yes" : "no",
		"portal_mirror_raw_fallback");
}

void NRIRenderer::TraceRuntimeLinkEvents(HWDrawInfo& di)
{
	if (!nri_ptruntimelinktrace)
	{
		mHasRuntimeLinkTraceState = false;
		mLastRuntimeLinkTraceState = {};
		return;
	}

	RuntimeLinkTraceState current = {};
	current.valid = true;
	current.candidateSectorIndex = mRuntimeSpaceLinkLastFrame.candidateSectorIndex;
	current.sourceSectorIndex = mRuntimeSpaceLinkLastFrame.sourceSectorIndex;
	current.geoEffectActive = mRuntimeSpaceLinkLastFrame.geoEffectActive;

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		const auto& sec = sector[sectorIndex];
		if (sec.lotag != 0)
		{
			current.visibleTaggedSectorCount++;
			if (current.taggedVisibleSectorStoredCount < current.taggedVisibleSectors.size())
			{
				RuntimeTaggedSectorDebugInfo info = {};
				if (gi != nullptr && gi->GetRuntimeLinkDebugTaggedSectorInfo((int)sectorIndex, &info))
				{
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
				else
				{
					info.available = true;
					info.sectorIndex = (int32_t)sectorIndex;
					info.lotag = sec.lotag;
					info.hitag = sec.hitag;
					current.taggedVisibleSectors[current.taggedVisibleSectorStoredCount++] = info;
				}
			}
		}
		if (sec.lotag == 848)
		{
			current.visible848SectorCount++;
		}
		if (sec.lotag == 160 || sec.lotag == 161)
		{
			current.visibleTeleportSectorCount++;
		}
	}

	if (gi != nullptr)
	{
		gi->GetRuntimeLinkDebugState(&current.game);
	}

	std::array<int32_t, 4> controlRoots =
	{
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.actorSectorIndex
	};

	for (const int32_t rootSectorIndex : controlRoots)
	{
		if (!validSectorIndex(rootSectorIndex))
		{
			continue;
		}

		RuntimeTaggedSectorDebugInfo rootInfo = {};
		if (GetRuntimeSectorControlInfo(rootSectorIndex, rootInfo))
		{
			AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, rootInfo);
		}

		const auto& rootSector = sector[(unsigned)rootSectorIndex];
		for (const auto& wal : rootSector.walls)
		{
			if (!wal.twoSided())
			{
				continue;
			}

			const int32_t adjacentSectorIndex = wal.nextsector;
			RuntimeTaggedSectorDebugInfo adjacentInfo = {};
			if (GetRuntimeSectorControlInfo(adjacentSectorIndex, adjacentInfo))
			{
				AppendRuntimeSectorControlInfo(current.nearbyControlSectors, current.nearbyControlSectorStoredCount, adjacentInfo);
			}
		}
	}

	const bool sameAsLast =
		mHasRuntimeLinkTraceState &&
		mLastRuntimeLinkTraceState.valid == current.valid &&
		mLastRuntimeLinkTraceState.candidateSectorIndex == current.candidateSectorIndex &&
		mLastRuntimeLinkTraceState.sourceSectorIndex == current.sourceSectorIndex &&
		mLastRuntimeLinkTraceState.geoEffectActive == current.geoEffectActive &&
		mLastRuntimeLinkTraceState.visibleTaggedSectorCount == current.visibleTaggedSectorCount &&
		mLastRuntimeLinkTraceState.visible848SectorCount == current.visible848SectorCount &&
		mLastRuntimeLinkTraceState.visibleTeleportSectorCount == current.visibleTeleportSectorCount &&
		mLastRuntimeLinkTraceState.taggedVisibleSectorStoredCount == current.taggedVisibleSectorStoredCount &&
		mLastRuntimeLinkTraceState.nearbyControlSectorStoredCount == current.nearbyControlSectorStoredCount &&
		SameRuntimeLinkDebugState(mLastRuntimeLinkTraceState.game, current.game);

	bool sameTaggedSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.taggedVisibleSectors[i], current.taggedVisibleSectors[i]))
			{
				sameTaggedSectors = false;
				break;
			}
		}
	}

	bool sameNearbyControlSectors = true;
	if (sameAsLast)
	{
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			if (!SameRuntimeTaggedSectorDebugInfo(mLastRuntimeLinkTraceState.nearbyControlSectors[i], current.nearbyControlSectors[i]))
			{
				sameNearbyControlSectors = false;
				break;
			}
		}
	}

	if (sameAsLast && sameTaggedSectors && sameNearbyControlSectors)
	{
		return;
	}

	mLastRuntimeLinkTraceState = current;
	mHasRuntimeLinkTraceState = true;

	Printf("NRI PT runtime link event: geo_effect=%s candidate_sector=%d source_sector=%d player_sector=%d lotag=%d hitag=%d effective_lotag=%d actor_sector=%d actor_lotag=%d actor_hitag=%d on_warp=%d transporter_hold=%d rr_geo_count=%d special_water=%s visible_tagged=%u visible_848=%u visible_teleport=%u\n",
		current.geoEffectActive ? "yes" : "no",
		current.candidateSectorIndex,
		current.sourceSectorIndex,
		current.game.playerSectorIndex,
		current.game.playerSectorLotag,
		current.game.playerSectorHitag,
		current.game.effectiveSectorLotag,
		current.game.actorSectorIndex,
		current.game.actorSectorLotag,
		current.game.actorSectorHitag,
		current.game.onWarpingSector,
		current.game.transporterHold,
		current.game.rrGeoCount,
		current.game.specialWaterSector ? "yes" : "no",
		current.visibleTaggedSectorCount,
		current.visible848SectorCount,
		current.visibleTeleportSectorCount);

	if (current.taggedVisibleSectorStoredCount > 0)
	{
		std::string taggedLine = "NRI PT runtime tagged sectors:";
		for (uint32_t i = 0; i < current.taggedVisibleSectorStoredCount; ++i)
		{
			const auto& info = current.taggedVisibleSectors[i];
			taggedLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				taggedLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						taggedLine += ",";
					}
					taggedLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					taggedLine += ",...";
				}
			}
			taggedLine += "]";
		}
		Printf("%s\n", taggedLine.c_str());
	}

	if (current.nearbyControlSectorStoredCount > 0)
	{
		std::string controlLine = "NRI PT runtime nearby controls:";
		for (uint32_t i = 0; i < current.nearbyControlSectorStoredCount; ++i)
		{
			const auto& info = current.nearbyControlSectors[i];
			controlLine += " [sector=" + std::to_string(info.sectorIndex) +
				" lotag=" + std::to_string(info.lotag) +
				" hitag=" + std::to_string(info.hitag);
			if (info.effectorCount > 0)
			{
				controlLine += " effectors=";
				const uint32_t storedEffectors = std::min<uint32_t>(info.effectorCount, (uint32_t)countof(info.effectorLotags));
				for (uint32_t effectorIndex = 0; effectorIndex < storedEffectors; ++effectorIndex)
				{
					if (effectorIndex > 0)
					{
						controlLine += ",";
					}
					controlLine += std::to_string(info.effectorLotags[effectorIndex]) +
						"/" + std::to_string(info.effectorHitags[effectorIndex]);
				}
				if (info.effectorCount > storedEffectors)
				{
					controlLine += ",...";
				}
			}
			controlLine += "]";
		}
		Printf("%s\n", controlLine.c_str());
	}
}

void NRIRenderer::TraceRuntimeMapMutationChunk(const nri_scene::PTMapChunk& mapChunk, RuntimeMapMutationCache::ChunkReplacement& replacement)
{
	if (nri_pttraceframes <= 0)
	{
		return;
	}

	const bool filterByChunk = nri_ptmutationtracechunk >= 0;
	const bool filterBySector = nri_ptmutationtracesector >= 0;
	if (!filterByChunk && !filterBySector)
	{
		return;
	}

	if (filterByChunk && mapChunk.chunkIndex != (uint32_t)nri_ptmutationtracechunk)
	{
		return;
	}

	if (filterBySector && mapChunk.sectorIndex != nri_ptmutationtracesector)
	{
		return;
	}

	const bool changed =
		replacement.traceCount == 0 ||
		replacement.lastTraceSignature != replacement.liveSignature ||
		replacement.lastTraceReasonMask != replacement.reasonMask ||
		replacement.lastTraceActive != replacement.active ||
		replacement.lastTraceBlindSpot != replacement.blindSpot;
	if (!changed)
	{
		return;
	}

	const std::string reasons = GetRuntimeMapMutationReasonSummary(replacement.reasonMask);
	Printf("NRI PT runtime map trace: chunk=%u sector=%d active=%s blind_spot=%s signature_changed=%s baseline_sig=0x%llx live_sig=0x%llx reasons=%s section_dirty=%u sector_dirty=%s dragged=%s surfaces=%u tris=%u materials=%u\n",
		mapChunk.chunkIndex,
		mapChunk.sectorIndex,
		replacement.active ? "yes" : "no",
		replacement.blindSpot ? "yes" : "no",
		replacement.liveSignature != replacement.baselineSignature ? "yes" : "no",
		(unsigned long long)replacement.baselineSignature,
		(unsigned long long)replacement.liveSignature,
		reasons.c_str(),
		replacement.sectionDirtyCount,
		replacement.sectorDirty ? "yes" : "no",
		replacement.dragged ? "yes" : "no",
		replacement.surfaceCount,
		replacement.triangleCount,
		(uint32_t)replacement.materialBridge.materials.size());

	replacement.lastTraceSignature = replacement.liveSignature;
	replacement.lastTraceReasonMask = replacement.reasonMask;
	replacement.lastTraceActive = replacement.active;
	replacement.lastTraceBlindSpot = replacement.blindSpot;
	replacement.traceCount++;
}

void NRIRenderer::PrintSceneBufferStatus() const
{
	const auto printBuffer = [](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		const uint64_t usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
		const uint64_t capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			stats.label,
			(unsigned long long)resource.usedSize,
			(unsigned long long)resource.size,
			(unsigned long long)usedItems,
			(unsigned long long)capacityItems,
			stats.uploadCount,
			stats.growthCount,
			stats.overwriteCount,
			(unsigned long long)stats.bytesUploadedLastFrame,
			stats.growEventsLastFrame,
			stats.overwriteEventsLastFrame,
			(unsigned long long)stats.peakUsedBytes);
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	const uint64_t totalUsed = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	const uint64_t totalCapacity = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	const uint64_t lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	const uint32_t lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	const uint32_t lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)totalUsed,
		(unsigned long long)totalCapacity,
		(unsigned long long)lastFrameUploadBytes,
		lastFrameGrowEvents,
		lastFrameOverwriteEvents);
	printBuffer(activeVertexBuffer, mVertexBufferStats);
	printBuffer(activeIndexBuffer, mIndexBufferStats);
	printBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	printBuffer(activeMaterialBuffer, mMaterialBufferStats);
	printBuffer(mPortalBuffer, mPortalBufferStats);
	printBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	printBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	printBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	printBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	printBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	printBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	printBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	printBuffer(mSectorLightBuffer, mSectorLightBufferStats);
}

void NRIRenderer::UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging)
{
	if (nri_ptsurfaceprobe <= 0 || !allowLogging)
	{
		return;
	}

	SurfaceProbeResult result = {};
	result.valid = true;

	float direction[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(direction);

	float bestDistance = std::numeric_limits<float>::infinity();
	for (uint32_t primitiveIndex = 0; primitiveIndex < geometry.primitives.size(); ++primitiveIndex)
	{
		const auto& primitive = geometry.primitives[primitiveIndex];
		const auto& v0 = geometry.vertices[primitive.indices[0]];
		const auto& v1 = geometry.vertices[primitive.indices[1]];
		const auto& v2 = geometry.vertices[primitive.indices[2]];
		float hitT = 0.0f;
		if (!IntersectProbeTriangle(v0, v1, v2, mCurrentCameraPos, direction, hitT) || hitT >= bestDistance)
		{
			continue;
		}

		bestDistance = hitT;
		result.hit = true;
		result.primitiveIndex = primitiveIndex;
		result.materialIndex = primitive.materialIndex;
		result.primitiveFlags = primitive.flags;
		result.distance = hitT;
		result.position[0] = mCurrentCameraPos[0] + direction[0] * hitT;
		result.position[1] = mCurrentCameraPos[1] + direction[1] * hitT;
		result.position[2] = mCurrentCameraPos[2] + direction[2] * hitT;
		result.normal[0] = primitive.normal[0];
		result.normal[1] = primitive.normal[1];
		result.normal[2] = primitive.normal[2];
		if (primitiveIndex < geometry.primitiveProvenance.size())
		{
			result.provenance = geometry.primitiveProvenance[primitiveIndex];
		}
	}

	if (result.hit && materials != nullptr && result.materialIndex < materials->lightMetadata.size())
	{
		const auto& metadata = materials->lightMetadata[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.textureId = metadata.textureId;
		result.materialClass = metadata.materialClass;
		result.lightLevel = metadata.lightLevel;
		result.alpha = metadata.alpha;
		Copy3(metadata.averageColor, result.averageColor);
		Copy3(metadata.emissiveColor, result.emissiveColor);
		Copy3(metadata.glowColor, result.glowColor);

		nri_scene::MaterialData effectiveMaterial = {};
		effectiveMaterial.textureIndex = metadata.textureIndex;
		effectiveMaterial.paletteIndex = metadata.paletteIndex;
		effectiveMaterial.flags = metadata.materialFlags;
		effectiveMaterial.materialClass = metadata.materialClass;
		effectiveMaterial.lightLevel = metadata.lightLevel;
		effectiveMaterial.alpha = metadata.alpha;
		effectiveMaterial.emissiveTextureIndex = metadata.emissiveTextureIndex;
		mSceneLights.ApplyEmissiveMaterialSettings(metadata, effectiveMaterial);
		result.emissiveMode = effectiveMaterial.emissiveMode;
		result.emissiveTextureIndex = effectiveMaterial.emissiveTextureIndex;
	}

	auto sameIdentity = [](const SurfaceProbeResult& a, const SurfaceProbeResult& b)
	{
		if (a.valid != b.valid || a.hit != b.hit)
		{
			return false;
		}
		if (!a.valid || !a.hit)
		{
			return true;
		}

		return
			a.provenance.sourceType == b.provenance.sourceType &&
			a.provenance.sectorIndex == b.provenance.sectorIndex &&
			a.provenance.wallIndex == b.provenance.wallIndex &&
			a.provenance.nextSectorIndex == b.provenance.nextSectorIndex &&
			a.provenance.actorIndex == b.provenance.actorIndex &&
			a.provenance.drawListType == b.provenance.drawListType &&
			a.provenance.cstat == b.provenance.cstat &&
			a.textureId == b.textureId &&
			a.materialLightingFlags == b.materialLightingFlags &&
			a.primitiveFlags == b.primitiveFlags &&
			a.materialIndex == b.materialIndex &&
			(a.provenance.sourceType != nri_scene::SurfaceSourceType::Unknown || a.primitiveIndex == b.primitiveIndex);
	};

	mLastSurfaceProbe = result;

	const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
	if (logOnChangeOnly && sameIdentity(mLastLoggedSurfaceProbe, result))
	{
		return;
	}

	if (!result.hit)
	{
		Printf("NRI PT surface probe: miss\n");
		mLastLoggedSurfaceProbe = result;
		return;
	}

	const uint32_t flags = result.primitiveFlags;
	const uint32_t lightingFlags = result.materialLightingFlags;
	const int32_t localSpaceIndex = result.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)result.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, result.provenance);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s chunk=%d local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s material_class=%u emissive_mode=%s emissive_tex=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(result.provenance.sourceType),
		GetDrawListTypeName(result.provenance.drawListType),
		result.provenance.mapChunkIndex,
		localSpaceIndex,
		portalGraphIndex,
		result.provenance.sectorIndex,
		result.provenance.wallIndex,
		result.provenance.nextSectorIndex,
		result.provenance.actorIndex,
		result.provenance.cstat,
		result.primitiveIndex,
		result.materialIndex,
		result.textureId,
		result.distance,
		result.position[0], result.position[1], result.position[2],
		result.normal[0], result.normal[1], result.normal[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		result.materialClass,
		GetMaterialEmissiveModeName(result.emissiveMode),
		result.emissiveTextureIndex != UINT32_MAX ? result.emissiveTextureIndex : 0u,
		result.lightLevel,
		result.alpha,
		result.averageColor[0], result.averageColor[1], result.averageColor[2],
		result.emissiveColor[0], result.emissiveColor[1], result.emissiveColor[2],
		result.glowColor[0], result.glowColor[1], result.glowColor[2]);
	mLastLoggedSurfaceProbe = result;
}

void NRIRenderer::PrintSurfaceProbeStatus() const
{
	if (!mLastSurfaceProbe.valid)
	{
		Printf("NRI PT surface probe: no sampled center hit has been recorded yet.\n");
		return;
	}

	if (!mLastSurfaceProbe.hit)
	{
		Printf("NRI PT surface probe: last sampled center ray missed translated PT geometry.\n");
		return;
	}

	const uint32_t flags = mLastSurfaceProbe.primitiveFlags;
	const uint32_t lightingFlags = mLastSurfaceProbe.materialLightingFlags;
	const int32_t localSpaceIndex = mLastSurfaceProbe.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, mLastSurfaceProbe.provenance);
	Printf("NRI PT surface probe: source=%s drawlist=%s chunk=%d local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s material_class=%u emissive_mode=%s emissive_tex=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(mLastSurfaceProbe.provenance.sourceType),
		GetDrawListTypeName(mLastSurfaceProbe.provenance.drawListType),
		mLastSurfaceProbe.provenance.mapChunkIndex,
		localSpaceIndex,
		portalGraphIndex,
		mLastSurfaceProbe.provenance.sectorIndex,
		mLastSurfaceProbe.provenance.wallIndex,
		mLastSurfaceProbe.provenance.nextSectorIndex,
		mLastSurfaceProbe.provenance.actorIndex,
		mLastSurfaceProbe.provenance.cstat,
		mLastSurfaceProbe.primitiveIndex,
		mLastSurfaceProbe.materialIndex,
		mLastSurfaceProbe.textureId,
		mLastSurfaceProbe.distance,
		mLastSurfaceProbe.position[0],
		mLastSurfaceProbe.position[1],
		mLastSurfaceProbe.position[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		mLastSurfaceProbe.materialClass,
		GetMaterialEmissiveModeName(mLastSurfaceProbe.emissiveMode),
		mLastSurfaceProbe.emissiveTextureIndex != UINT32_MAX ? mLastSurfaceProbe.emissiveTextureIndex : 0u,
		mLastSurfaceProbe.lightLevel,
		mLastSurfaceProbe.alpha,
		mLastSurfaceProbe.averageColor[0],
		mLastSurfaceProbe.averageColor[1],
		mLastSurfaceProbe.averageColor[2],
		mLastSurfaceProbe.emissiveColor[0],
		mLastSurfaceProbe.emissiveColor[1],
		mLastSurfaceProbe.emissiveColor[2],
		mLastSurfaceProbe.glowColor[0],
		mLastSurfaceProbe.glowColor[1],
		mLastSurfaceProbe.glowColor[2]);
}

void NRIRenderer::RefreshSceneLightSystem(
	bool usedStaticMapScene,
	const nri_scene::SceneView* capturedSceneView,
	const nri_scene::MaterialBridgeData* capturedMaterials,
	const nri_scene::SceneView* dynamicSceneView,
	const nri_scene::MaterialBridgeData* dynamicMaterials)
{
	mSceneLights.BeginFrame(mFrameIndex);

	if (usedStaticMapScene && mStaticMapScene.valid)
	{
		const size_t chunkCount = std::min(mStaticMapScene.lightChunkViews.size(), mStaticMapScene.chunks.size());
		for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			mSceneLights.AppendSceneView(
				mStaticMapScene.lightChunkViews[chunkIndex],
				mStaticMapScene.materialBridge,
				SceneLightRecordSource::StaticMapScene,
				mStaticMapScene.chunks[chunkIndex].materialOffset);
		}
	}
	else if (capturedSceneView != nullptr && capturedMaterials != nullptr)
	{
		mSceneLights.AppendSceneView(*capturedSceneView, *capturedMaterials, SceneLightRecordSource::CapturedScene);
	}

	if (dynamicSceneView != nullptr && dynamicMaterials != nullptr)
	{
		mSceneLights.AppendSceneView(*dynamicSceneView, *dynamicMaterials, SceneLightRecordSource::DynamicScene);
	}

	mSceneLights.RebuildAnalyticLights(mFrameIndex, NRI_MAX_RUNTIME_POINT_LIGHTS);
	mSceneLights.RebuildEmissiveSurfaces(NRI_MAX_EMISSIVE_SURFACES);
	mSceneLights.RebuildSectorLighting(mFrameIndex, (uint32_t)sector.Size());
	if (mSceneLights.ConsumeAnalyticLightTopologyChanged())
	{
		mBoundRuntimeLightCount = 0;
		RequestHistoryReset("analytic-light-topology");
	}
	if (mSceneLights.ConsumeEmissiveSurfaceTopologyChanged())
	{
		RequestHistoryReset("emissive-surface-topology");
	}
	if (mSceneLights.ConsumeEmissiveMaterialsDirty())
	{
		QueueStaticMapSceneLightingInvalidation();
		RequestHistoryReset("emissive-material-change");
	}
	if (mSceneLights.ConsumeSectorLightingTopologyChanged())
	{
		RequestHistoryReset("sector-light-topology");
	}
}

void NRIRenderer::QueueStaticMapSceneLightingInvalidation()
{
	mPendingStaticMapLightingInvalidation = true;
}

void NRIRenderer::ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const
{
	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		mSceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], inOutGpuMaterials[materialIndex]);
	}
}

void NRIRenderer::InvalidateStaticMapSceneForMaterialLighting()
{
	if (!mStaticMapScene.valid)
	{
		return;
	}

	DestroyStaticMapSceneCache();
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
}

void NRIRenderer::PrintSceneLightDump(float radius, uint32_t limit) const
{
	if (!mSceneLights.HasRecords())
	{
		Printf("NRI PT scene lights: no cached scene-light identity is available yet.\n");
		return;
	}

	struct Candidate
	{
		const SceneLightSystem::SurfaceRecord* record = nullptr;
		float distanceSq = 0.0f;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(mSceneLights.GetSurfaceRecords().size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;

	for (const SceneLightSystem::SurfaceRecord& record : mSceneLights.GetSurfaceRecords())
	{
		const float dx = record.center[0] - mCurrentCameraPos[0];
		const float dy = record.center[1] - mCurrentCameraPos[1];
		const float dz = record.center[2] - mCurrentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}

		candidates.push_back({ &record, distanceSq });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.record->materialIndex < b.record->materialIndex;
	});

	const uint32_t requestedLimit = limit == 0 ? 32u : limit;
	const uint32_t printCount = (uint32_t)std::min<size_t>(candidates.size(), requestedLimit);
	Printf("NRI PT scene lights: cached_surface_identities=%u near_camera=%u radius=%.2f frame=%u\n",
		(uint32_t)mSceneLights.GetSurfaceRecords().size(),
		(uint32_t)candidates.size(),
		radius,
		mFrameIndex);

	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SceneLightSystem::SurfaceRecord& record = *candidates[i].record;
		const uint32_t lightingFlags = record.material.lightingFlags;
		const int32_t localSpaceIndex = record.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)record.provenance.mapChunkIndex) : -1;
		const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, record.provenance);
		const char* textureName = record.material.texture != nullptr ? record.material.texture->GetName().GetChars() : "(null)";
		Printf("NRI PT scene light %u: source=%s drawlist=%s dist=%.2f center=(%.2f, %.2f, %.2f) radius=%.2f material=%u material_key=0x%016llx texture_key=0x%016llx glowmap_key=0x%016llx tile=%u texture=%s sector=%d wall=%d chunk=%d local_space=%d portal_graph=%d actor=%d palette=%u shade=%d alpha=%.3f light=%.3f flags=0x%x fullbright=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s emissive_mode=%s emissive_tex=%u avg=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
			i,
			GetSceneLightRecordSourceName(record.source),
			GetDrawListTypeName(record.provenance.drawListType),
			std::sqrt(candidates[i].distanceSq),
			record.center[0],
			record.center[1],
			record.center[2],
			record.boundsRadius,
			record.materialIndex,
			(unsigned long long)record.material.materialKey,
			(unsigned long long)record.material.textureContentKey,
			(unsigned long long)record.material.glowmapContentKey,
			record.material.textureId,
			textureName,
			record.provenance.sectorIndex,
			record.provenance.wallIndex,
			record.provenance.mapChunkIndex,
			localSpaceIndex,
			portalGraphIndex,
			record.provenance.actorIndex,
			record.material.paletteIndex,
			record.material.shade,
			record.material.alpha,
			record.material.lightLevel,
			record.material.materialFlags,
			(lightingFlags & nri_scene::MaterialLightingFlag_MaterialFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0 ? "yes" : "no",
			GetMaterialEmissiveModeName(record.material.emissiveMode),
			record.material.emissiveTextureIndex != UINT32_MAX ? record.material.emissiveTextureIndex : 0u,
			record.material.averageColor[0],
			record.material.averageColor[1],
			record.material.averageColor[2],
			record.material.glowColor[0],
			record.material.glowColor[1],
			record.material.glowColor[2]);
	}

	if (printCount == 0)
	{
		Printf("NRI PT scene lights: no cached surfaces matched the requested radius.\n");
	}
}

const char* NRIRenderer::GetAvailabilityReason() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return "renderer device is not initialized";
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	if (deviceDesc.tiers.rayTracing == 0)
	{
		return "required ray tracing capability is unavailable on this device/API";
	}

	if (deviceDesc.pipelineLayout.rootConstantMaxSize < sizeof(NRITraceConstants) ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		return "device pipeline layout limits are below the NRI PT backend requirements";
	}

	return "path tracing is unavailable";
}

bool NRIRenderer::CheckPathTracingSupport()
{
	mPathTracingSupported = mFrameBuffer != nullptr && mFrameBuffer->mDevice != nullptr;
	if (!mPathTracingSupported)
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	if (deviceDesc.tiers.rayTracing == 0 ||
		deviceDesc.pipelineLayout.rootConstantMaxSize < sizeof(NRITraceConstants) ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		mPathTracingSupported = false;
		LogFallback(GetAvailabilityReason());
	}

	return mPathTracingSupported;
}

void NRIRenderer::LogFallback(const char* reason)
{
	if (mHasLoggedFallback)
	{
		return;
	}

	Printf(TEXTCOLOR_ORANGE "NRI PT fallback: %s\n", reason != nullptr ? reason : "unknown reason");
	mHasLoggedFallback = true;
}

void NRIRenderer::RefreshMapWorld()
{
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged && mSceneLights.GetManualAnalyticLightCount() > 0)
	{
		const uint32_t clearedCount = mSceneLights.GetManualAnalyticLightCount();
		ClearRuntimePointLights();
		Printf("NRI PT test lights cleared: count=%u reason=level-change\n", clearedCount);
	}
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		return;
	}

	ResetPersistentDynamicEmissiveCache();

	nri_scene::PTMapWorld world;
	if (!nri_scene::BuildMapWorld(world))
	{
		if (pendingBuildSerial != mObservedMapWorldBuildSerial || levelChanged)
		{
			Printf(TEXTCOLOR_RED "NRI PT map world: authoritative level-load build failed for %s.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)");
		}
		mMapWorld.Reset();
		mMapWorld.level = currentLevel;
		mObservedMapWorldBuildSerial = pendingBuildSerial;
		return;
	}

	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = pendingBuildSerial;
	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world built: level=%s build_serial=%llu chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

bool NRIRenderer::CreatePipelineLayout()
{
	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIComputeStage();

	nri::DescriptorRangeDesc sceneTextureRange = {};
	sceneTextureRange.baseRegisterIndex = 0;
	sceneTextureRange.descriptorNum = NRI_SCENE_DESCRIPTOR_NUM;
	sceneTextureRange.descriptorType = nri::DescriptorType::TEXTURE;
	sceneTextureRange.shaderStages = NRIComputeStage();
	sceneTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc sceneDataRange = {};
	sceneDataRange.baseRegisterIndex = 0;
	sceneDataRange.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	sceneDataRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	sceneDataRange.shaderStages = NRIComputeStage();
	sceneDataRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[5] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &sceneTextureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[2].registerSpace = 2;
	descriptorSets[2].ranges = &sceneDataRange;
	descriptorSets[2].rangeNum = 1;
	descriptorSets[2].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[3].registerSpace = 3;
	descriptorSets[3].ranges = &inputRange;
	descriptorSets[3].rangeNum = 1;
	descriptorSets[3].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[4].registerSpace = 4;
	descriptorSets[4].ranges = &outputRange;
	descriptorSets[4].rangeNum = 1;
	descriptorSets[4].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::RootDescriptorDesc rootDescriptors[1] = {};
	rootDescriptors[0].registerIndex = 0;
	rootDescriptors[0].shaderStages = NRIComputeStage();
	rootDescriptors[0].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 5;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.rootDescriptors = rootDescriptors;
	desc.rootDescriptorNum = (uint32_t)std::size(rootDescriptors);
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreateTaaPipelineLayout()
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 3;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = NRIComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 1;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = NRIComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceConstants);
	rootConstant.shaderStages = NRIComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 2;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = NRIComputeStage();

	return mFrameBuffer->mCore.CreatePipelineLayout(*mFrameBuffer->mDevice, desc, mTaaPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIRenderer::CreatePipelines()
{
	auto createPipeline = [this](const char* fileName, PipelineSlot slot, nri::PipelineLayout* layout)
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
		pipelineDesc.pipelineLayout = layout;
		pipelineDesc.shader = shader;
		return mFrameBuffer->mCore.CreateComputePipeline(*mFrameBuffer->mDevice, pipelineDesc, mPipelines[(size_t)slot]) == nri::Result::SUCCESS;
	};

	const bool d3d12 = mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	const char* suffix = d3d12 ? "dxil" : "spirv";

	FString trace = FStringf("TraceOpaque.cs.%s", suffix);
	FString composition = FStringf("Composition.cs.%s", suffix);
	FString taa = FStringf("Taa.cs.%s", suffix);
	FString rawPresent = FStringf("RawPresent.cs.%s", suffix);
	FString finalPresent = FStringf("FinalPresent.cs.%s", suffix);
	FString dlssBefore = FStringf("DlssBefore.cs.%s", suffix);
	FString dlssAfter = FStringf("DlssAfter.cs.%s", suffix);
	FString final = FStringf("Final.cs.%s", suffix);

	return
		createPipeline(trace.GetChars(), PipelineSlot::TraceOpaque, mPipelineLayout) &&
		createPipeline(composition.GetChars(), PipelineSlot::Composition, mPipelineLayout) &&
		createPipeline(taa.GetChars(), PipelineSlot::Taa, mTaaPipelineLayout) &&
		createPipeline(rawPresent.GetChars(), PipelineSlot::RawPresent, mTaaPipelineLayout) &&
		createPipeline(finalPresent.GetChars(), PipelineSlot::FinalPresent, mTaaPipelineLayout) &&
		createPipeline(dlssBefore.GetChars(), PipelineSlot::DlssBefore, mPipelineLayout) &&
		createPipeline(dlssAfter.GetChars(), PipelineSlot::DlssAfter, mPipelineLayout) &&
		createPipeline(final.GetChars(), PipelineSlot::Final, mPipelineLayout);
}

bool NRIRenderer::AllocateDescriptorSets()
{
	return
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &mSamplerSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 1, &mSceneTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 2, &mSceneDataSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mCompositionFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mCompositionOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mTaaFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mTaaOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mPresentOutputSet, 1, 0) == nri::Result::SUCCESS;
}

bool NRIRenderer::UpdateSamplerSet()
{
	const nri::Descriptor* descriptors[NRI_SAMPLER_DESCRIPTOR_NUM] = {
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapLinear],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampLinear],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapPoint],
		mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampPoint]
	};
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSamplerSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors;
	update.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors)
{
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSceneTextureSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(descriptors.data());
	update.descriptorNum = (uint32_t)descriptors.size();
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

void NRIRenderer::BuildRuntimePointLightUpload(std::vector<RuntimePointLightGpuData>& outLights) const
{
	const auto& activeLights = mSceneLights.GetAnalyticLights().activeLights;
	outLights.clear();
	outLights.reserve(activeLights.size());
	for (const SceneLightSystem::SceneAnalyticLight& light : activeLights)
	{
		RuntimePointLightGpuData gpuLight = {};
		Copy3(light.position, gpuLight.position);
		gpuLight.radius = light.radius;
		Copy3(light.color, gpuLight.color);
		gpuLight.intensity = light.intensity;
		outLights.push_back(gpuLight);
	}
}

void NRIRenderer::BuildEmissiveSamplingUpload(
	const EmissiveSamplingBuildContext& context,
	EmissivePrimitiveHeaderGpuData& outHeader,
	std::vector<EmissivePrimitiveGpuData>& outPrimitives,
	std::vector<float>& outCdf,
	std::vector<EmissivePrimitiveDebugRecord>& outDebugRecords) const
{
	outHeader = {};
	outHeader.dominantIndex = UINT32_MAX;
	outHeader.flags = nri_ptemissiveautoonly ? NRI_EMISSIVE_SAMPLING_FLAG_AUTO_ONLY : 0u;
	outPrimitives.clear();
	outCdf.clear();
	outDebugRecords.clear();

	struct MaterialPrimitiveRange
	{
		uint32_t first = UINT32_MAX;
		uint32_t count = 0;
	};

	struct BuiltCandidate
	{
		EmissivePrimitiveGpuData gpu = {};
		EmissivePrimitiveDebugRecord debug = {};
	};

	auto buildRanges = [](const nri_scene::GeometryData* geometry, std::vector<MaterialPrimitiveRange>& outRanges)
	{
		outRanges.clear();
		if (geometry == nullptr)
		{
			return;
		}

		uint32_t maxMaterialIndex = 0;
		for (const auto& primitive : geometry->primitives)
		{
			maxMaterialIndex = std::max(maxMaterialIndex, primitive.materialIndex);
		}

		outRanges.assign((size_t)maxMaterialIndex + 1u, {});
		for (uint32_t primitiveIndex = 0; primitiveIndex < geometry->primitives.size(); ++primitiveIndex)
		{
			const uint32_t materialIndex = geometry->primitives[primitiveIndex].materialIndex;
			auto& range = outRanges[materialIndex];
			if (range.count == 0)
			{
				range.first = primitiveIndex;
			}
			range.count++;
		}
	};

	std::vector<MaterialPrimitiveRange> staticRanges;
	std::vector<MaterialPrimitiveRange> capturedRanges;
	std::vector<MaterialPrimitiveRange> dynamicRanges;
	buildRanges(context.staticGeometry, staticRanges);
	buildRanges(context.capturedGeometry, capturedRanges);
	buildRanges(context.dynamicGeometry, dynamicRanges);

	std::vector<BuiltCandidate> candidates;
	const auto& activeSurfaces = mSceneLights.GetEmissiveSurfaces().activeSurfaces;
	candidates.reserve(activeSurfaces.size());

	auto appendSurfacePrimitives = [&](const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, const nri_scene::GeometryData* geometry, const std::vector<MaterialPrimitiveRange>& ranges, uint32_t dataSource, uint32_t primitiveBase)
	{
		if (geometry == nullptr || surface.materialIndex == UINT32_MAX || surface.materialIndex >= ranges.size())
		{
			return;
		}

		const auto& range = ranges[surface.materialIndex];
		if (range.count == 0 || range.first == UINT32_MAX)
		{
			return;
		}

		float representativeLuminance = 0.0f;
		if (surface.surfaceArea > 0.0f && surface.emissiveIntensity > 0.0f)
		{
			representativeLuminance = std::max(surface.powerEstimate / (surface.surfaceArea * surface.emissiveIntensity), 0.0f);
		}

		for (uint32_t localOffset = 0; localOffset < range.count; ++localOffset)
		{
			const uint32_t localPrimitiveIndex = range.first + localOffset;
			const uint32_t primitiveIndex = primitiveBase + localPrimitiveIndex;
			const float primitiveArea = ComputePrimitiveArea(*geometry, localPrimitiveIndex);
			if (primitiveArea <= 0.0f)
			{
				continue;
			}

			BuiltCandidate candidate = {};
			candidate.gpu.dataSource = dataSource;
			candidate.gpu.primitiveIndex = primitiveIndex;
			candidate.gpu.sourceFlags = surface.sourceFlags;
			candidate.gpu.textureId = surface.textureId;
			candidate.gpu.primitiveArea = primitiveArea;
			candidate.gpu.powerEstimate = std::max(primitiveArea * representativeLuminance * surface.emissiveIntensity, 0.0f);

			candidate.debug.stableKey = HashCombine64(surface.stableKey, ((uint64_t)dataSource << 32u) | primitiveIndex);
			candidate.debug.dataSource = dataSource;
			candidate.debug.primitiveIndex = primitiveIndex;
			candidate.debug.materialIndex = surface.materialIndex;
			candidate.debug.sourceFlags = surface.sourceFlags;
			candidate.debug.sourceRuleId = surface.sourceRuleId;
			candidate.debug.textureId = surface.textureId;
			candidate.debug.emissiveMode = surface.emissiveMode;
			candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
			candidate.debug.actorIndex = surface.actorIndex;
			candidate.debug.primitiveArea = primitiveArea;
			candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
			candidate.debug.selectionPdf = 0.0f;
			candidate.debug.emissiveIntensity = surface.emissiveIntensity;
			Copy3(surface.emissiveColor, candidate.debug.emissiveColor);
			ComputePrimitiveCenter(*geometry, localPrimitiveIndex, candidate.debug.center);

			candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
			candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
			candidates.push_back(candidate);
		}
	};

	for (const auto& surface : activeSurfaces)
	{
		if (nri_ptemissiveautoonly && !HasAutoEmissiveSourceFlags(surface.sourceFlags))
		{
			continue;
		}

		switch (surface.source)
		{
		case SceneLightRecordSource::StaticMapScene:
			appendSurfacePrimitives(surface, context.staticGeometry, staticRanges, NRI_SCENE_DATA_SOURCE_STATIC, 0u);
			break;
		case SceneLightRecordSource::CapturedScene:
			appendSurfacePrimitives(surface, context.capturedGeometry, capturedRanges, NRI_SCENE_DATA_SOURCE_DYNAMIC, 0u);
			break;
		case SceneLightRecordSource::DynamicScene:
			appendSurfacePrimitives(surface, context.dynamicGeometry, dynamicRanges, NRI_SCENE_DATA_SOURCE_DYNAMIC, context.dynamicPrimitiveBaseOffset);
			break;
		default:
			break;
		}
	}

	if (candidates.size() > NRI_MAX_EMISSIVE_PRIMITIVES)
	{
		std::stable_sort(candidates.begin(), candidates.end(), [](const BuiltCandidate& a, const BuiltCandidate& b)
		{
			if (a.gpu.powerEstimate != b.gpu.powerEstimate)
			{
				return a.gpu.powerEstimate > b.gpu.powerEstimate;
			}

			return a.debug.stableKey < b.debug.stableKey;
		});
		candidates.resize(NRI_MAX_EMISSIVE_PRIMITIVES);
	}

	outPrimitives.reserve(candidates.size());
	outDebugRecords.reserve(candidates.size());

	float totalPower = 0.0f;
	float dominantPower = -1.0f;
	uint32_t dominantTile = 0;
	uint32_t dominantFlags = 0;
	uint32_t dominantPrimitive = UINT32_MAX;
	uint32_t dominantDataSource = 0;

	for (size_t i = 0; i < candidates.size(); ++i)
	{
		outPrimitives.push_back(candidates[i].gpu);
		outDebugRecords.push_back(candidates[i].debug);
		totalPower += candidates[i].gpu.powerEstimate;
		if (candidates[i].gpu.powerEstimate > dominantPower)
		{
			dominantPower = candidates[i].gpu.powerEstimate;
			outHeader.dominantIndex = (uint32_t)i;
			dominantTile = candidates[i].gpu.textureId;
			dominantFlags = candidates[i].gpu.sourceFlags;
			dominantPrimitive = candidates[i].gpu.primitiveIndex;
			dominantDataSource = candidates[i].gpu.dataSource;
		}
	}

	outHeader.activeCount = (uint32_t)outPrimitives.size();
	outHeader.totalPower = totalPower;

	if (outPrimitives.empty())
	{
		outCdf.resize(1, 1.0f);
		return;
	}

	float runningCdf = 0.0f;
	const float invTotalPower = totalPower > 0.0f ? (1.0f / totalPower) : 0.0f;
	for (size_t i = 0; i < outPrimitives.size(); ++i)
	{
		float pdf = 0.0f;
		if (totalPower > 0.0f)
		{
			pdf = outPrimitives[i].powerEstimate * invTotalPower;
		}
		else
		{
			pdf = 1.0f / (float)outPrimitives.size();
		}

		outPrimitives[i].selectionPdf = pdf;
		outDebugRecords[i].selectionPdf = pdf;
		runningCdf += pdf;
		outCdf.push_back(i + 1 == outPrimitives.size() ? 1.0f : std::min(runningCdf, 1.0f));
	}
}

void NRIRenderer::BuildSectorLightingUpload(
	SectorLightHeaderGpuData& outHeader,
	std::vector<SectorLightGpuData>& outSectors)
{
	const auto& registry = mSceneLights.GetSectorLighting();
	outHeader = {};
	outHeader.sectorCount = registry.sectorCount;
	outHeader.activeCount = registry.activeSectorCount;
	outHeader.pulsingCount = registry.pulsingSectorCount;
	outHeader.flags = nri_ptsectorlighting ? NRI_SECTOR_LIGHTING_FLAG_ENABLED : 0u;
	outSectors.assign(registry.sectorCount, {});

	mBoundSectorLightSectorCount = registry.sectorCount;
	mBoundSectorLightActiveCount = registry.activeSectorCount;
	mBoundSectorLightPulsingCount = registry.pulsingSectorCount;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size() || sectorIndex >= outSectors.size())
		{
			continue;
		}

		const auto& source = registry.sectors[sectorIndex];
		auto& target = outSectors[sectorIndex];
		Copy3(source.ambientColor, target.ambientColor);
		Copy3(source.ambientColor, target.hemisphereColor);
		target.ambientIntensity = source.ambientIntensity;
		target.hemisphereAmount = source.hemisphereAmount;
		target.fogAmount = source.fogAmount;
		target.pulseScale = source.pulseScale;
		target.sourceFlags = source.sourceFlags;
		target.paletteIndex = source.paletteIndex;
		target.lotag = source.lotag;
		target.hitag = source.hitag;

		const float contribution = source.ambientIntensity + std::abs(source.hemisphereAmount) + source.fogAmount;
		if (contribution > mBoundSectorLightDominantContribution)
		{
			mBoundSectorLightDominantContribution = contribution;
			mBoundSectorLightDominantSector = sectorIndex;
		}
	}
}

bool NRIRenderer::UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context)
{
	EmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<EmissivePrimitiveDebugRecord> emissiveDebugRecords;
	BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveDebugRecords);

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveHeaderBuffer,
		mEmissivePrimitiveHeaderBufferStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(EmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveBuffer,
		mEmissivePrimitiveBufferStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(EmissivePrimitiveGpuData),
		sizeof(EmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveCdfBuffer,
		mEmissivePrimitiveCdfBufferStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	mBoundEmissivePrimitiveCount = emissiveHeader.activeCount;
	mBoundEmissiveTotalPower = emissiveHeader.totalPower;
	mBoundEmissiveDominantPrimitive = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissiveDebugRecords.size() ? emissiveDebugRecords[emissiveHeader.dominantIndex].primitiveIndex : UINT32_MAX;
	mBoundEmissiveDominantTile = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].textureId : 0u;
	mBoundEmissiveDominantFlags = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].sourceFlags : 0u;
	mBoundEmissiveDominantDataSource = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].dataSource : 0u;
	mBoundEmissiveDominantPower = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].powerEstimate : 0.0f;
	mBoundEmissivePrimitiveRecords = std::move(emissiveDebugRecords);

	mSceneDataDescriptors[13] = mEmissivePrimitiveHeaderBuffer.shaderView;
	mSceneDataDescriptors[14] = mEmissivePrimitiveBuffer.shaderView;
	mSceneDataDescriptors[15] = mEmissivePrimitiveCdfBuffer.shaderView;

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSceneDataSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data());
	update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateReprojectionBuffer()
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, mPreviousWorldToView, sizeof(data.previousWorldToView));
	if (!EnsureStructuredBuffer(
		mReprojectionBuffer,
		mReprojectionBufferStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (mSceneDataDescriptors[18] != mReprojectionBuffer.shaderView)
	{
		mSceneDataDescriptors[18] = mReprojectionBuffer.shaderView;
		bool descriptorsReady = mSceneDataSet != nullptr;
		for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				descriptorsReady = false;
				break;
			}
		}

		if (descriptorsReady)
		{
			nri::UpdateDescriptorRangeDesc update = {};
			update.descriptorSet = mSceneDataSet;
			update.rangeIndex = 0;
			update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data());
			update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
			mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
		}
	}

	return true;
}

void NRIRenderer::BuildRuntimeLightClusterUpload(
	std::vector<RuntimeLightTileHeaderGpuData>& outHeaders,
	std::vector<uint32_t>& outIndices,
	uint32_t& outTileCountX,
	uint32_t& outTileCountY,
	uint32_t& outTileIndexCount,
	uint32_t& outMaxTileOccupancy) const
{
	const auto& activeLights = mSceneLights.GetAnalyticLights().activeLights;
	const uint32_t activeLightCount = (uint32_t)activeLights.size();
	outTileCountX = std::max(1u, (mRenderWidth + NRI_RUNTIME_LIGHT_TILE_SIZE - 1u) / NRI_RUNTIME_LIGHT_TILE_SIZE);
	outTileCountY = std::max(1u, (mRenderHeight + NRI_RUNTIME_LIGHT_TILE_SIZE - 1u) / NRI_RUNTIME_LIGHT_TILE_SIZE);
	const uint32_t tileCount = outTileCountX * outTileCountY;
	const uint32_t maxIndexCapacity = tileCount * NRI_MAX_RUNTIME_POINT_LIGHTS;
	outTileIndexCount = 0;
	outMaxTileOccupancy = 0;
	outHeaders.assign(tileCount, {});
	outIndices.assign(maxIndexCapacity, 0u);

	if (tileCount == 0 || activeLightCount == 0 || mRenderWidth == 0 || mRenderHeight == 0)
	{
		return;
	}

	auto dot3 = [](const float* a, const float* b) -> float
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	};

	std::vector<std::vector<uint32_t>> tileLights(tileCount);
	for (uint32_t lightIndex = 0; lightIndex < activeLightCount; ++lightIndex)
	{
		const SceneLightSystem::SceneAnalyticLight& light = activeLights[lightIndex];
		const float toLight[3] = {
			light.position[0] - mCurrentCameraPos[0],
			light.position[1] - mCurrentCameraPos[1],
			light.position[2] - mCurrentCameraPos[2]
		};
		const float viewX = dot3(toLight, mCurrentCameraRight);
		const float viewY = dot3(toLight, mCurrentCameraUp);
		const float viewZ = dot3(toLight, mCurrentCameraForward);
		if (viewZ <= -light.radius)
		{
			continue;
		}

		int32_t minTileX = 0;
		int32_t minTileY = 0;
		int32_t maxTileX = (int32_t)outTileCountX - 1;
		int32_t maxTileY = (int32_t)outTileCountY - 1;

		if (viewZ > light.radius &&
			mCurrentTanHalfFovX > 0.0f &&
			mCurrentTanHalfFovY > 0.0f)
		{
			const float conservativeDepth = std::max(viewZ - light.radius, 1.0f);
			const float centerNdcX = viewX / (viewZ * mCurrentTanHalfFovX);
			const float centerNdcY = viewY / (viewZ * mCurrentTanHalfFovY);
			const float radiusNdcX = light.radius / (conservativeDepth * mCurrentTanHalfFovX);
			const float radiusNdcY = light.radius / (conservativeDepth * mCurrentTanHalfFovY);
			const float minPixelX = ((centerNdcX - radiusNdcX) * 0.5f + 0.5f) * (float)mRenderWidth;
			const float maxPixelX = ((centerNdcX + radiusNdcX) * 0.5f + 0.5f) * (float)mRenderWidth;
			const float minPixelY = (0.5f - (centerNdcY + radiusNdcY) * 0.5f) * (float)mRenderHeight;
			const float maxPixelY = (0.5f - (centerNdcY - radiusNdcY) * 0.5f) * (float)mRenderHeight;
			if (maxPixelX < 0.0f || minPixelX >= (float)mRenderWidth || maxPixelY < 0.0f || minPixelY >= (float)mRenderHeight)
			{
				continue;
			}

			minTileX = std::max(0, (int32_t)std::floor(minPixelX / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			minTileY = std::max(0, (int32_t)std::floor(minPixelY / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			maxTileX = std::min((int32_t)outTileCountX - 1, (int32_t)std::floor(std::max(maxPixelX - 1.0f, 0.0f) / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
			maxTileY = std::min((int32_t)outTileCountY - 1, (int32_t)std::floor(std::max(maxPixelY - 1.0f, 0.0f) / (float)NRI_RUNTIME_LIGHT_TILE_SIZE));
		}

		if (minTileX > maxTileX || minTileY > maxTileY)
		{
			continue;
		}

		for (int32_t tileY = minTileY; tileY <= maxTileY; ++tileY)
		{
			for (int32_t tileX = minTileX; tileX <= maxTileX; ++tileX)
			{
				tileLights[(size_t)tileY * outTileCountX + (size_t)tileX].push_back(lightIndex);
			}
		}
	}

	uint32_t indexCursor = 0;
	for (uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		RuntimeLightTileHeaderGpuData& header = outHeaders[tileIndex];
		const std::vector<uint32_t>& tileLightList = tileLights[tileIndex];
		header.indexOffset = indexCursor;
		header.indexCount = (uint32_t)tileLightList.size();
		outMaxTileOccupancy = std::max(outMaxTileOccupancy, header.indexCount);
		for (uint32_t lightIndex : tileLightList)
		{
			if (indexCursor < outIndices.size())
			{
				outIndices[indexCursor] = lightIndex;
				indexCursor++;
			}
		}
	}

	outTileIndexCount = indexCursor;
}

bool NRIRenderer::UpdateSceneDataSet(
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
	uint32_t dynamicMaterialCount)
{
	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	if (sceneInstances.empty())
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;

	static SceneBufferDebugStats sSceneInstanceStats = { "SceneInstance" };
	if (!EnsureStructuredBuffer(
		mSceneInstanceBuffer,
		sSceneInstanceStats,
		sceneInstances.data(),
		sceneInstances.size() * sizeof(SceneInstanceData),
		sizeof(SceneInstanceData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}
	mBoundSceneInstances = sceneInstances;

	const std::vector<ScenePortalData> scenePortals = BuildScenePortalData(mMapWorld);
	if (!EnsureStructuredBuffer(
		mPortalBuffer,
		mPortalBufferStats,
		scenePortals.data(),
		scenePortals.size() * sizeof(ScenePortalData),
		sizeof(ScenePortalData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	std::vector<RuntimePointLightGpuData> runtimeLights;
	BuildRuntimePointLightUpload(runtimeLights);
	if (!EnsureStructuredBuffer(
		mRuntimeLightBuffer,
		mRuntimeLightBufferStats,
		runtimeLights.empty() ? nullptr : runtimeLights.data(),
		runtimeLights.size() * sizeof(RuntimePointLightGpuData),
		sizeof(RuntimePointLightGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	std::vector<RuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
	std::vector<uint32_t> runtimeLightTileIndices;
	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	BuildRuntimeLightClusterUpload(
		runtimeLightTileHeaders,
		runtimeLightTileIndices,
		runtimeLightTileCountX,
		runtimeLightTileCountY,
		runtimeLightTileIndexCount,
		runtimeLightMaxTileOccupancy);
	if (!EnsureStructuredBuffer(
		mRuntimeLightTileHeaderBuffer,
		mRuntimeLightTileHeaderBufferStats,
		runtimeLightTileHeaders.data(),
		runtimeLightTileHeaders.size() * sizeof(RuntimeLightTileHeaderGpuData),
		sizeof(RuntimeLightTileHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mRuntimeLightTileIndexBuffer,
		mRuntimeLightTileIndexBufferStats,
		runtimeLightTileIndices.data(),
		runtimeLightTileIndices.size() * sizeof(uint32_t),
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	EmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<EmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<EmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
	BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, ignoredEmissiveDebugRecords);
	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveHeaderBuffer,
		mEmissivePrimitiveHeaderBufferStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(EmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveBuffer,
		mEmissivePrimitiveBufferStats,
		emissivePrimitives.empty() ? nullptr : emissivePrimitives.data(),
		emissivePrimitives.empty() ? 0u : emissivePrimitives.size() * sizeof(EmissivePrimitiveGpuData),
		sizeof(EmissivePrimitiveGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mEmissivePrimitiveCdfBuffer,
		mEmissivePrimitiveCdfBufferStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	SectorLightHeaderGpuData sectorLightHeader = {};
	std::vector<SectorLightGpuData> sectorLights;
	BuildSectorLightingUpload(sectorLightHeader, sectorLights);
	if (!EnsureStructuredBuffer(
		mSectorLightHeaderBuffer,
		mSectorLightHeaderBufferStats,
		&sectorLightHeader,
		sizeof(sectorLightHeader),
		sizeof(SectorLightHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!EnsureStructuredBuffer(
		mSectorLightBuffer,
		mSectorLightBufferStats,
		sectorLights.empty() ? nullptr : sectorLights.data(),
		sectorLights.empty() ? 0u : sectorLights.size() * sizeof(SectorLightGpuData),
		sizeof(SectorLightGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	mSceneDataDescriptors = {
		selectView(staticVertexBuffer, dynamicVertexBuffer),
		selectView(staticIndexBuffer, dynamicIndexBuffer),
		selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer),
		selectView(staticMaterialBuffer, dynamicMaterialBuffer),
		selectView(dynamicVertexBuffer, staticVertexBuffer),
		selectView(dynamicIndexBuffer, staticIndexBuffer),
		selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer),
		selectView(dynamicMaterialBuffer, staticMaterialBuffer),
		mSceneInstanceBuffer.shaderView,
		mPortalBuffer.shaderView,
		mRuntimeLightBuffer.shaderView,
		mRuntimeLightTileHeaderBuffer.shaderView,
		mRuntimeLightTileIndexBuffer.shaderView,
		mEmissivePrimitiveHeaderBuffer.shaderView,
		mEmissivePrimitiveBuffer.shaderView,
		mEmissivePrimitiveCdfBuffer.shaderView,
		mSectorLightHeaderBuffer.shaderView,
		mSectorLightBuffer.shaderView,
		mReprojectionBuffer.shaderView,
	};

	for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSceneDataSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(mSceneDataDescriptors.data());
	update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);

	mBoundStaticPrimitiveCount = staticPrimitiveCount;
	mBoundDynamicPrimitiveCount = dynamicPrimitiveCount;
	mBoundStaticMaterialCount = staticMaterialCount;
	mBoundDynamicMaterialCount = dynamicMaterialCount;
	mBoundPortalCount = mMapWorld.valid ? (uint32_t)mMapWorld.portals.size() : 0u;
	mBoundRuntimeLightCount = (uint32_t)runtimeLights.size();
	mBoundRuntimeLightTileCountX = runtimeLightTileCountX;
	mBoundRuntimeLightTileCountY = runtimeLightTileCountY;
	mBoundRuntimeLightTileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	mBoundRuntimeLightTileIndexCount = runtimeLightTileIndexCount;
	mBoundRuntimeLightMaxTileOccupancy = runtimeLightMaxTileOccupancy;
	return true;
}

bool NRIRenderer::UpdateFrameTextureSet()
{
	return UpdateFrameTextureSet(mFrameTextureSet, mFrameInputDescriptors);
}

bool NRIRenderer::UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 14>& descriptors)
{
	const nri::Descriptor* rawDescriptors[NRI_INPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_INPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::UpdateOutputSet()
{
	return UpdateOutputSet(mOutputSet, mOutputDescriptors);
}

bool NRIRenderer::UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 15>& descriptors)
{
	const nri::Descriptor* rawDescriptors[NRI_OUTPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_OUTPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIRenderer::CreateFrameTexture(FrameTextureSlot slot, uint32_t width, uint32_t height, nri::Format format)
{
	return mFrameBuffer->CreateOwnedTexture(GetFrameTexture(slot), width, height, format, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
}

void NRIRenderer::PrepareSceneTextureInputsForCompute()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	if (mPaletteTexture.texture != nullptr)
	{
		mFrameBuffer->TransitionTexture(mPaletteTexture, NRIComputeShaderResourceState());
	}

	if (mFrameBuffer->mWhiteTexture != nullptr)
	{
		mFrameBuffer->TransitionTexture(mFrameBuffer->mWhiteTexture->GetResource(), NRIComputeShaderResourceState());
	}

	for (auto& entry : mTextureCache)
	{
		if (entry.resource.texture != nullptr)
		{
			mFrameBuffer->TransitionTexture(entry.resource, NRIComputeShaderResourceState());
		}
	}
}

bool NRIRenderer::EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	Clocker clock(NriPTFrameResources);

	if (outputWidth == 0 || outputHeight == 0 || targetWidth == 0 || targetHeight == 0)
	{
		return false;
	}

	const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
	// Preserve the oversized hardware viewport and crop it during present instead of shrinking it to the visible target.
	const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
	const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - (int32_t)outputHeight;

	const NRIUpscalerKind upscalerKind = ResolveUpscalerKind(false);
	float renderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	if (upscalerKind == NRIUpscalerKind::DLSR || upscalerKind == NRIUpscalerKind::DLRR)
	{
		renderScale = GetUpscalerRenderScale(GetSelectedUpscalerMode());
	}

	const uint32_t renderWidth = std::max(1u, (uint32_t)std::lround((double)outputWidth * renderScale));
	const uint32_t renderHeight = std::max(1u, (uint32_t)std::lround((double)outputHeight * renderScale));
	const nri::Format outputFormat =
		(mFrameBuffer->mActiveTarget != nullptr && mFrameBuffer->mActiveTarget->format != nri::Format::UNKNOWN)
		? mFrameBuffer->mActiveTarget->format
		: nri::Format::BGRA8_UNORM;

	const bool upToDate =
		mRenderWidth == renderWidth &&
		mRenderHeight == renderHeight &&
		mOutputWidth == outputWidth &&
		mOutputHeight == outputHeight &&
		mTargetWidth == targetWidth &&
		mTargetHeight == targetHeight &&
		mSceneLeft == sceneLeft &&
		mSceneTop == sceneTop &&
		mOutputFormat == outputFormat &&
		GetFrameTexture(FrameTextureSlot::Final).texture != nullptr;

	if (upToDate)
	{
		return true;
	}

	mNrd.Shutdown();
	DestroyFrameTextures();
	mRenderWidth = renderWidth;
	mRenderHeight = renderHeight;
	mOutputWidth = outputWidth;
	mOutputHeight = outputHeight;
	mTargetWidth = targetWidth;
	mTargetHeight = targetHeight;
	mSceneLeft = sceneLeft;
	mSceneTop = sceneTop;
	mOutputFormat = outputFormat;
	RequestHistoryReset("frame-resources");

	const nri::Format colorFormat = nri::Format::RGBA16_SFLOAT;
	const nri::Format normalRoughnessFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format finalFormat = outputFormat;

	return
		CreateFrameTexture(FrameTextureSlot::ViewZ, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Motion, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::NormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::BaseColorMetalness, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UnfilteredPenumbra, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedShadow, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Composed, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectLighting, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectEmission, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssSpecularAlbedo, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssSpecularHitDistance, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssNormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::Upscaled, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::PreFinal, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Final, targetWidth, targetHeight, finalFormat);
}

bool NRIRenderer::EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials)
{
	Clocker clock(NriPTPaletteUpload);

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

bool NRIRenderer::DispatchBootstrapView()
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceConstants constants = {};
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags = NRI_FLAG_BOOTSTRAP_VIEW | (mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	UpdateFrameTextureSet();

	mOutputDescriptors.fill(GetFrameTexture(FrameTextureSlot::PreFinal).storageView);
	mOutputDescriptors[2] = final.storageView;
	UpdateOutputSet();

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

void NRIRenderer::ResetSceneBufferFrameStats()
{
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mPortalBufferStats.bytesUploadedLastFrame = 0;
	mPortalBufferStats.growEventsLastFrame = 0;
	mPortalBufferStats.overwriteEventsLastFrame = 0;
}

const NRIBufferResource& NRIRenderer::GetActiveVertexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mVertexBuffer : mStaticVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveIndexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mIndexBuffer : mStaticIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActivePrimitiveBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? mPrimitiveBuffer : mStaticPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveMaterialBuffer() const
{
	return mBoundDynamicMaterialCount > 0 ? mMaterialBuffer : mStaticMaterialBuffer;
}

void NRIRenderer::BindSceneRootDescriptors()
{
	if (mTopLevelAS.descriptor != nullptr)
	{
		mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 0, mTopLevelAS.descriptor, 0, nri::BindPoint::COMPUTE });
	}
}

bool NRIRenderer::EnsureStaticMapScene()
{
	if (!mMapWorld.valid)
	{
		return false;
	}

	if (mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		DestroyStaticMapSceneCache();
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
	}

	if (mStaticMapScene.valid &&
		mStaticMapScene.texturesResident &&
		mStaticMapScene.buffersResident &&
		mStaticMapScene.accelerationResident &&
		mStaticMapScene.buildSerial == mMapWorld.buildSerial)
	{
		mStaticMapScene.reuseCount++;
		return true;
	}

	nri_scene::BuildMapSceneView(mMapWorld, mStaticMapScene.sceneView);
	mStaticMapScene.lightChunkViews.clear();
	mStaticMapScene.geometry = {};
	mStaticMapScene.materialBridge = {};
	mStaticMapScene.chunks.clear();
	mStaticMapScene.lightChunkViews.reserve(mMapWorld.chunks.size());
	mStaticMapScene.chunks.reserve(mMapWorld.chunks.size());
	mRuntimeMapMutations.chunks.clear();
	mRuntimeMapMutations.chunks.resize(mMapWorld.chunks.size());
	mRuntimeMapMutations.replacedChunkMask.assign(mMapWorld.chunks.size(), 0u);

	for (const nri_scene::PTMapChunk& chunk : mMapWorld.chunks)
	{
		if (chunk.chunkIndex < mRuntimeMapMutations.chunks.size())
		{
			auto& replacement = mRuntimeMapMutations.chunks[chunk.chunkIndex];
			nri_scene::CaptureMapChunkMutationBaseline(chunk, replacement.baseline);
			replacement.baselineSignature = replacement.baseline.signature;
			replacement.liveSignature = replacement.baselineSignature;
			replacement.reasonMask = 0;
			replacement.sectionDirtyCount = 0;
			replacement.sectorDirty = false;
			replacement.dragged = false;
			replacement.blindSpot = false;
			replacement.lastTraceSignature = UINT64_MAX;
			replacement.lastTraceReasonMask = UINT32_MAX;
			replacement.lastTraceActive = false;
			replacement.lastTraceBlindSpot = false;
			replacement.traceCount = 0;
		}

		nri_scene::SceneView chunkSceneView;
		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		nri_scene::BuildMapChunkSceneView(mMapWorld, chunk, chunkSceneView);
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(chunkSceneView, chunkGeometry);
				AssignGeometryPortalIndices(mMapWorld, chunkGeometry);
			}
		{
			Clocker clock(NriPTMaterialBuild);
			nri_scene::BuildMaterials(chunkSceneView, chunkMaterials);
		}
		if (chunkGeometry.primitives.empty())
		{
			continue;
		}

		StaticMapSceneCache::ChunkCache chunkCache = {};
		chunkCache.chunkIndex = chunk.chunkIndex;
		chunkCache.vertexOffset = (uint32_t)mStaticMapScene.geometry.vertices.size();
		chunkCache.vertexCount = (uint32_t)chunkGeometry.vertices.size();
		chunkCache.indexOffset = (uint32_t)mStaticMapScene.geometry.indices.size();
		chunkCache.indexCount = (uint32_t)chunkGeometry.indices.size();
		chunkCache.primitiveOffset = (uint32_t)mStaticMapScene.geometry.primitives.size();
		chunkCache.primitiveCount = (uint32_t)chunkGeometry.primitives.size();
		chunkCache.materialOffset = (uint32_t)mStaticMapScene.materialBridge.materials.size();
		chunkCache.materialCount = (uint32_t)chunkMaterials.materials.size();

		AppendGeometry(chunkGeometry, chunkCache.materialOffset, mStaticMapScene.geometry);
		AppendMaterialBridge(chunkMaterials, mStaticMapScene.materialBridge);
		mStaticMapScene.lightChunkViews.push_back(std::move(chunkSceneView));
		mStaticMapScene.chunks.push_back(std::move(chunkCache));
	}

	if (mStaticMapScene.geometry.primitives.empty() ||
		!EnsurePaletteTexture(mStaticMapScene.materialBridge) ||
		!EnsureSceneTextures(mStaticMapScene.sceneView, mStaticMapScene.materialBridge, mStaticMapScene.gpuMaterials, false) ||
		!UploadSceneBuffers(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mStaticMapScene.geometry,
			mStaticMapScene.gpuMaterials) ||
		!BuildStaticMapAccelerationStructures())
	{
		return false;
	}

	mStaticMapScene.valid = true;
	mStaticMapScene.texturesResident = true;
	mStaticMapScene.buffersResident = true;
	mStaticMapScene.accelerationResident = true;
	mStaticMapScene.buildSerial = mMapWorld.buildSerial;
	mStaticMapScene.tlasInstanceCount = (uint32_t)mStaticMapScene.chunks.size();
	mStaticMapScene.sceneBuildCount++;
	mStaticMapScene.gpuUploadCount++;
	mStaticMapScene.accelerationBuildCount++;
	mUploadedStaticMapSceneLastFrame = true;
	mBuiltStaticMapSceneASLastFrame = true;

	Printf("NRI PT static scene resident: level=%s build_serial=%llu chunks=%u tris=%u materials=%u uploads=%u as_builds=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mStaticMapScene.buildSerial,
		(uint32_t)mStaticMapScene.chunks.size(),
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		mStaticMapScene.gpuUploadCount,
		mStaticMapScene.accelerationBuildCount);
	return true;
}

bool NRIRenderer::EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky)
{
	if (mSkyLevel != currentLevel)
	{
		mActiveSkyTextureIndex = UINT32_MAX;
		mSkyTextureKey = 0;
		mSkyState = {};
		mSkyLevel = currentLevel;
	}

	auto findCachedSkyTexture = [this](uint64_t key, uint32_t width, uint32_t height) -> uint32_t
	{
		for (uint32_t i = 0; i < (uint32_t)mSkyTextureCache.size(); ++i)
		{
			const CachedSkyTexture& cached = mSkyTextureCache[i];
			if (cached.key == key &&
				cached.resource.width == width &&
				cached.resource.height == height)
			{
				return i;
			}
		}

		return UINT32_MAX;
	};

	auto activateCachedSky = [this](uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode)
	{
		mActiveSkyTextureIndex = index;
		mSkyTextureKey = key;
		mSkyState.mode = mode;
		mSkyState.sourceType = sourceView.sky.sourceType;
		mSkyState.texture = sourceView.sky.texture;
		mSkyState.faceMask = sourceView.sky.faceMask;
		mSkyState.flipTop = sourceView.sky.flipTop;
	};

	auto createCachedSky = [this, &findCachedSkyTexture](const SkyUpload& upload, nri_scene::PTSkyMode mode) -> uint32_t
	{
		const uint32_t existing = findCachedSkyTexture(upload.key, upload.width, upload.height);
		if (existing != UINT32_MAX)
		{
			return existing;
		}

		CachedSkyTexture cacheEntry = {};
		cacheEntry.key = upload.key;
		cacheEntry.mode = mode;
		if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, upload.width, upload.height, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureType::TEXTURE_2D, 6, nri::TextureView::TEXTURE_CUBE))
		{
			return UINT32_MAX;
		}

		std::array<nri::TextureSubresourceUploadDesc, 6> subresources = {};
		for (uint32_t i = 0; i < 6; ++i)
		{
			subresources[i].slices = upload.faces[i].pixels.data();
			subresources[i].sliceNum = 1;
			subresources[i].rowPitch = upload.faces[i].width * 4u;
			subresources[i].slicePitch = upload.faces[i].width * upload.faces[i].height * 4u;
		}

		if (!mFrameBuffer->UploadTextureSubresources(cacheEntry.resource, subresources.data(), (uint32_t)subresources.size(), upload.width, upload.height))
		{
			mFrameBuffer->DestroyTextureResource(cacheEntry.resource);
			return UINT32_MAX;
		}

		mSkyTextureCache.push_back(std::move(cacheEntry));
		return (uint32_t)mSkyTextureCache.size() - 1;
	};

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	if (preserveExistingSky && activeSkyTexture != nullptr)
	{
		TraceSkyState(sceneView, "preserve-existing", mSkyTextureKey);
		return true;
	}

	if (sceneView.sky.mode == nri_scene::PTSkyMode::Cubemap &&
		activeSkyTexture != nullptr &&
		mSkyState.mode == nri_scene::PTSkyMode::Cubemap &&
		mSkyState.texture == sceneView.sky.texture &&
		mSkyState.faceMask == sceneView.sky.faceMask &&
		mSkyState.flipTop == sceneView.sky.flipTop)
	{
		mSkyLevel = currentLevel;
		TraceSkyState(sceneView, "reuse-active-cubemap", mSkyTextureKey);
		return true;
	}

	SkyProbe probe = {};
	if (ProbeCubemapSky(sceneView, probe))
	{
		if (activeSkyTexture != nullptr &&
			mSkyTextureKey == probe.key &&
			activeSkyTexture->width == probe.width &&
			activeSkyTexture->height == probe.height)
		{
			mSkyLevel = currentLevel;
			TraceSkyState(sceneView, "reuse-active-probe", probe.key);
			return true;
		}

		const uint32_t cachedIndex = findCachedSkyTexture(probe.key, probe.width, probe.height);
		if (cachedIndex != UINT32_MAX)
		{
			activateCachedSky(cachedIndex, probe.key, sceneView, nri_scene::PTSkyMode::Cubemap);
			mSkyLevel = currentLevel;
			TraceSkyState(sceneView, "activate-cached-cubemap", probe.key);
			return true;
		}

		SkyUpload upload = {};
		if (!BuildCubemapUpload(sceneView, probe, upload))
		{
			return false;
		}

		const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::Cubemap);
		if (createdIndex == UINT32_MAX)
		{
			return false;
		}

		activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::Cubemap);
		mSkyLevel = currentLevel;
		TraceSkyState(sceneView, "create-cached-cubemap", upload.key);
		return true;
	}

	const bool shouldKeepLastCubemap =
		activeSkyTexture != nullptr &&
		mSkyState.mode == nri_scene::PTSkyMode::Cubemap &&
		(sceneView.sky.mode == nri_scene::PTSkyMode::None ||
			sceneView.sky.texture == mSkyState.texture ||
			(sceneView.sky.texture == nullptr && sceneView.stats.skySurfaces > 0) ||
			(mSkyLevel == currentLevel &&
				sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
				sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal &&
				sceneView.stats.skySurfaces > 0));
	if (shouldKeepLastCubemap)
	{
		if (sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
			sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal)
		{
			TraceSkyState(sceneView, "hold-level-cubemap", mSkyTextureKey);
			return true;
		}

		TraceSkyState(sceneView, "keep-last-cubemap", mSkyTextureKey);
		return true;
	}

	SkyUpload upload = {};
	BuildSolidSkyUpload(sceneView.skyColor, upload);
	if (activeSkyTexture != nullptr &&
		mSkyTextureKey == upload.key &&
		activeSkyTexture->width == upload.width &&
		activeSkyTexture->height == upload.height)
	{
		TraceSkyState(sceneView, "reuse-active-solid", upload.key);
		return true;
	}

	const uint32_t cachedIndex = findCachedSkyTexture(upload.key, upload.width, upload.height);
	if (cachedIndex != UINT32_MAX)
	{
		activateCachedSky(cachedIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
		TraceSkyState(sceneView, "activate-cached-solid", upload.key);
		return true;
	}

	const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::SolidColor);
	if (createdIndex == UINT32_MAX)
	{
		return false;
	}

	activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
	TraceSkyState(sceneView, "create-cached-solid", upload.key);
	return true;
}

bool NRIRenderer::EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky)
{
	Clocker clock(NriPTSceneTextures);

	outGpuMaterials = materials.materials;
	ApplyEmissiveMaterialOverrides(materials, outGpuMaterials);
	if (!EnsureSkyTexture(sceneView, preserveExistingSky))
	{
		return false;
	}

	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mPaletteTexture.shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;

	for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)materials.textures.size(), NRI_MAX_SCENE_TEXTURES); ++i)
	{
		const auto& upload = materials.textures[i];
		if (upload.width == 0 || upload.height == 0 || upload.pixels.empty())
		{
			continue;
		}

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

		descriptors[2 + i] = it->resource.shaderView;
	}

	for (auto& material : outGpuMaterials)
	{
		if (material.textureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.textureIndex = 0;
		}
		if (material.emissiveTextureIndex != UINT32_MAX && material.emissiveTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.emissiveTextureIndex = 0;
		}
	}

	return UpdateSceneTextureSet(descriptors);
}

bool NRIRenderer::UseFallbackSceneTextures(bool preserveExistingSky)
{
	if (!preserveExistingSky || GetActiveSkyTexture() == nullptr)
	{
		EnsureSkyTexture(nri_scene::SceneView{}, false);
	}
	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr && GetActiveSkyTexture()->shaderView != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	return UpdateSceneTextureSet(descriptors);
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	if (resource.buffer != nullptr || resource.shaderView != nullptr)
	{
		mFrameBuffer->WaitForCommands(true);
	}

	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;

	if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.size = desc.size;
	resource.usedSize = size;
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

	if (data != nullptr && size != 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, 0, desc.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		if (desc.size > size)
		{
			std::memset(static_cast<uint8_t*>(mapped) + size, 0, (size_t)(desc.size - size));
		}
		mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
	}

	if (mFrameBuffer->mCommandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}

	return true;
}

bool NRIRenderer::EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.stride != stride ||
		resource.size < requiredSize;

	stats.bytesUploadedLastFrame = size;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.uploadCount++;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);

	if (needsGrowth)
	{
		const uint64_t grownSize = GetGrownBufferSize(resource.size, requiredSize, stride);
		if (resource.buffer != nullptr || resource.shaderView != nullptr)
		{
			mFrameBuffer->WaitForCommands(true);
		}
		DestroyBufferResource(resource);

		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(grownSize, stride);
		desc.structureStride = stride;
		desc.usage = usage;

		if (mFrameBuffer->mCore.CreateCommittedBuffer(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE_UPLOAD, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		resource.size = desc.size;
		resource.usedSize = size;
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

		stats.growthCount++;
		stats.growEventsLastFrame = 1;
	}
	else
	{
		resource.usedSize = size;
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	if (data != nullptr && size != 0)
	{
		if (!needsGrowth)
		{
			// Scene buffers are reused persistent DEVICE_UPLOAD allocations. Fence before
			// overwriting them so prior queued frames cannot read partially updated data.
			mFrameBuffer->WaitForCommands(true);
		}

		void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, 0, resource.size);
		if (mapped == nullptr)
		{
			return false;
		}

		std::memcpy(mapped, data, (size_t)size);
		mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
	}

	if (mFrameBuffer->mCommandBuffer != nullptr && after.access != nri::AccessBits::NONE)
	{
		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = resource.buffer;
		barrier.before = {};
		barrier.after = after;

		nri::BarrierDesc barrierDesc = {};
		barrierDesc.buffers = &barrier;
		barrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}

	return true;
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	if (resource.buffer != nullptr)
	{
		mFrameBuffer->WaitForCommands(true);
	}

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
	resource.usedSize = size;
	resource.stride = stride;
	return true;
}

bool NRIRenderer::UploadSceneBuffers(const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	return UploadSceneBuffers(mVertexBuffer, mIndexBuffer, mPrimitiveBuffer, mMaterialBuffer, geometry, materials);
}

bool NRIRenderer::UploadSceneBuffers(
	NRIBufferResource& vertexBuffer,
	NRIBufferResource& indexBuffer,
	NRIBufferResource& primitiveBuffer,
	NRIBufferResource& materialBuffer,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTSceneBuffers);
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;

	return
		EnsureStructuredBuffer(vertexBuffer, mVertexBufferStats, geometry.vertices.data(), geometry.vertices.size() * sizeof(nri_scene::SceneVertex), sizeof(nri_scene::SceneVertex), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(indexBuffer, mIndexBufferStats, geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), sizeof(uint32_t), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(primitiveBuffer, mPrimitiveBufferStats, geometry.primitives.data(), geometry.primitives.size() * sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()) &&
		EnsureStructuredBuffer(materialBuffer, mMaterialBufferStats, materials.data(), materials.size() * sizeof(nri_scene::MaterialData), sizeof(nri_scene::MaterialData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess());
}

bool NRIRenderer::BuildStaticMapAccelerationStructures()
{
	Clocker clock(NriPTAcceleration);

	if (mStaticMapScene.chunks.empty())
	{
		return false;
	}

	const bool needsWait =
		mTopLevelAS.accelerationStructure != nullptr ||
		mEmissiveTopLevelAS.accelerationStructure != nullptr ||
		mDynamicBottomLevelAS.accelerationStructure != nullptr ||
		mTlasInstanceBuffer.buffer != nullptr ||
		mEmissiveTlasInstanceBuffer.buffer != nullptr ||
		mSceneInstanceBuffer.buffer != nullptr ||
		mScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		mFrameBuffer->WaitForCommands(true);
	}

	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	uint64_t maxScratchSize = 0;
	for (auto& chunk : mStaticMapScene.chunks)
	{
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = (uint32_t)mStaticMapScene.geometry.vertices.size();
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)chunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = chunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		maxScratchSize = std::max(maxScratchSize, mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure));
	}

	if (!CreateBufferWithoutView(mScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
	{
		return false;
	}

	std::vector<nri::BufferBarrierDesc> blasBarriers;
	blasBarriers.reserve(mStaticMapScene.chunks.size());
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		auto& chunk = mStaticMapScene.chunks[chunkIndex];
		nri::BottomLevelGeometryDesc geometryDesc = {};
		geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
		geometryDesc.triangles.vertexOffset = 0;
		geometryDesc.triangles.vertexNum = (uint32_t)mStaticMapScene.geometry.vertices.size();
		geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
		geometryDesc.triangles.indexOffset = (uint64_t)chunk.indexOffset * sizeof(uint32_t);
		geometryDesc.triangles.indexNum = chunk.indexCount;
		geometryDesc.triangles.indexType = nri::IndexType::UINT32;

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = mScratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

		if (chunkIndex + 1 < mStaticMapScene.chunks.size())
		{
			// The static chunk path deliberately reuses one scratch buffer across many BLAS builds.
			// Serialize reuse explicitly so later builds do not stomp scratch data that the GPU is still consuming.
			nri::BufferBarrierDesc scratchBarrier = {};
			scratchBarrier.buffer = mScratchBuffer.buffer;
			scratchBarrier.before = NRIAccelerationStructureScratchAccess();
			scratchBarrier.after = NRIAccelerationStructureScratchAccess();

			nri::BarrierDesc scratchBarrierDesc = {};
			scratchBarrierDesc.buffers = &scratchBarrier;
			scratchBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
		}

		nri::BufferBarrierDesc barrier = {};
		barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
		barrier.before = NRIAccelerationStructureWriteAccess();
		barrier.after = NRIAccelerationStructureReadAccess();
		blasBarriers.push_back(barrier);
	}

	if (!blasBarriers.empty())
	{
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers.data();
		blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, blasBarrierDesc);
	}

	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	mStaticAccelerationBuildSerial = mStaticMapScene.buildSerial;
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u);
}

void NRIRenderer::BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances, const std::vector<uint8_t>* replacedChunkMask) const
{
	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(mStaticMapScene.chunks.size());
	outSceneInstances.reserve(mStaticMapScene.chunks.size());

	for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		const auto& chunk = mStaticMapScene.chunks[chunkIndex];
		if (replacedChunkMask != nullptr &&
			chunk.chunkIndex < replacedChunkMask->size() &&
			(*replacedChunkMask)[chunk.chunkIndex] != 0)
		{
			continue;
		}

		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = (uint32_t)outSceneInstances.size();
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		outTlasInstances.push_back(instance);
		outSceneInstances.push_back({ chunk.primitiveOffset, NRI_SCENE_DATA_SOURCE_STATIC, 0u, 0u });
	}
}

bool NRIRenderer::BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeMapLastFrame = {};

	if (!mStaticMapScene.valid ||
		mRuntimeMapMutations.chunks.size() != mMapWorld.chunks.size() ||
		mRuntimeMapMutations.replacedChunkMask.size() != mMapWorld.chunks.size())
	{
		return false;
	}

	std::fill(mRuntimeMapMutations.replacedChunkMask.begin(), mRuntimeMapMutations.replacedChunkMask.end(), 0u);

	for (size_t chunkIndex = 0; chunkIndex < mMapWorld.chunks.size(); ++chunkIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[chunkIndex];
		auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
		const uint64_t cachedSignature = replacement.liveSignature;
		nri_scene::PTMapChunkMutationAnalysis analysis = {};
		if (!nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.baseline, analysis))
		{
			replacement.active = false;
			replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
			replacement.sectionDirtyCount = 0;
			replacement.sectorDirty = false;
			replacement.dragged = false;
			replacement.blindSpot = false;
			TraceRuntimeMapMutationChunk(mapChunk, replacement);
			continue;
		}

		replacement.liveSignature = analysis.signature;
		replacement.reasonMask = analysis.reasonMask;
		replacement.sectionDirtyCount = analysis.sectionDirtyCount;
		replacement.sectorDirty = analysis.sectorDirty;
		replacement.dragged = analysis.dragged;
		replacement.blindSpot = analysis.reasonMask != nri_scene::PTMapChunkMutationReason_None && !analysis.signatureChanged;
		// Section dirty alone is too broad for PT runtime replacement because
		// the raster path can mark transient warped sections dirty during draw
		// prep without producing a stable gameplay map mutation. Keep explicit
		// forced invalidation for sector-dirty and dragged ownership, and let
		// section-dirty-only cases fall back to signature-backed replacement.
		const bool forceTopologyInvalidation =
			(analysis.reasonMask & (nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_Dragged)) != 0;

		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
		{
			mRuntimeMapLastFrame.sectorGeometryChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
		{
			mRuntimeMapLastFrame.sectorMaterialChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
		{
			mRuntimeMapLastFrame.wallGeometryChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
		{
			mRuntimeMapLastFrame.wallMaterialChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
		{
			mRuntimeMapLastFrame.sectorDirtyChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
		{
			mRuntimeMapLastFrame.sectionDirtyChunkCount++;
		}
		if ((analysis.reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
		{
			mRuntimeMapLastFrame.draggedChunkCount++;
		}

		if (analysis.reasonMask == nri_scene::PTMapChunkMutationReason_None)
		{
			replacement.active = false;
			TraceRuntimeMapMutationChunk(mapChunk, replacement);
			continue;
		}

		mRuntimeMapLastFrame.dirtyChunkCount++;
		if (replacement.blindSpot)
		{
			mRuntimeMapLastFrame.blindSpotChunkCount++;
		}

		if (!replacement.valid || cachedSignature != replacement.liveSignature || forceTopologyInvalidation)
		{
			nri_scene::SceneView liveChunkView;
			nri_scene::PTMapWorldStats liveStats = {};
			if (nri_scene::BuildLiveMapChunkSceneView(mapChunk, liveChunkView, &liveStats))
			{
				nri_scene::GeometryData liveGeometry;
				nri_scene::MaterialBridgeData liveMaterials;
				{
					Clocker clock(NriPTGeometryBuild);
					nri_scene::BuildGeometry(liveChunkView, liveGeometry);
					AssignGeometryPortalIndices(mMapWorld, liveGeometry);
				}
				{
					Clocker clock(NriPTMaterialBuild);
					nri_scene::BuildMaterials(liveChunkView, liveMaterials);
				}

				replacement.geometry = std::move(liveGeometry);
				replacement.materialBridge = std::move(liveMaterials);
				replacement.surfaceCount = liveStats.surfaceCount;
				replacement.triangleCount = liveStats.triangleCount;
				replacement.valid = true;
				replacement.active = true;
				mRuntimeMapLastFrame.rebuiltChunkCount++;
			}
			else if (replacement.valid)
			{
				replacement.active = true;
				mRuntimeMapLastFrame.heldChunkCount++;
			}
			else
			{
				replacement.active = false;
				TraceRuntimeMapMutationChunk(mapChunk, replacement);
				continue;
			}
		}
		else
		{
			replacement.active = true;
		}

		mRuntimeMapMutations.replacedChunkMask[chunkIndex] = 1u;
		mRuntimeMapLastFrame.replacedChunkCount++;
		mRuntimeMapLastFrame.replacementSurfaceCount += replacement.surfaceCount;
		mRuntimeMapLastFrame.replacementTriangleCount += replacement.triangleCount;
		mRuntimeMapLastFrame.materialCount += (uint32_t)replacement.materialBridge.materials.size();

		if (!replacement.geometry.primitives.empty())
		{
			AppendGeometry(replacement.geometry, (uint32_t)outMaterials.materials.size(), outGeometry);
		}
		AppendMaterialBridge(replacement.materialBridge, outMaterials);
		TraceRuntimeMapMutationChunk(mapChunk, replacement);
	}

	mRuntimeMapLastFrame.active = mRuntimeMapLastFrame.replacedChunkCount > 0;
	return mRuntimeMapLastFrame.active;
}

bool NRIRenderer::BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount = CountOrphanLocalSpaces(mMapWorld);
	mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount = mMapWorld.stats.runtimePortalCount;

	const auto deactivateRuntimeLinkHistory = [&]()
	{
		if (!mRuntimeChunkTranslationHistory.empty())
		{
			mRuntimeSpaceLinkLastFrame.topologyChanged = true;
			RequestHistoryReset("runtime-link-deactivated", false, true);
		}
	};

	if (!mMapWorld.valid)
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	int effectSectorIndex = -1;
	if (di.Viewpoint.SectNums != nullptr)
	{
		if (di.Viewpoint.SectCount > 0)
		{
			effectSectorIndex = di.Viewpoint.SectNums[0];
			mRuntimeSpaceLinkLastFrame.viewRootSectorCount = (uint32_t)di.Viewpoint.SectCount;
		}
	}
	else
	{
		effectSectorIndex = di.Viewpoint.SectCount;
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount = 1;
	}

	if (effectSectorIndex < 0 || (unsigned)effectSectorIndex >= sector.Size())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (visibleSectors.Check(sectorIndex))
		{
			mRuntimeSpaceLinkLastFrame.visibleSectorCount++;
		}
	}

	mRuntimeSpaceLinkLastFrame.candidateSectorIndex = effectSectorIndex;
	mRuntimeSpaceLinkLastFrame.candidateSectorLotag = sector[(unsigned)effectSectorIndex].lotag;
	mRuntimeSpaceLinkLastFrame.queryAttempted = true;

	GeoEffect effect = {};
	int providerSectorIndex = -1;
	if (gi != nullptr && gi->GetGeoEffect(&effect, &sector[effectSectorIndex]))
	{
		providerSectorIndex = effectSectorIndex;
	}
	else
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
	}

	const auto getLocalSpaceIndex = [&](int sectorIndex) -> uint32_t
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return UINT32_MAX;
		}

		return mMapWorld.chunks[(unsigned)sectorIndex].localSpaceIndex;
	};

	const uint32_t candidateLocalSpaceIndex = getLocalSpaceIndex(effectSectorIndex);
	const auto sectorMatchesVisibleSet = [&](int sectorIndex) -> bool
	{
		return sectorIndex >= 0 &&
			(unsigned)sectorIndex < visibleSectors.Size() &&
			visibleSectors.Check((unsigned)sectorIndex);
	};
	auto groupMatchesCandidate = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesSector = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			if (sectorIndex < 0)
			{
				return false;
			}

			if (sectorIndex == effectSectorIndex)
			{
				return true;
			}

			if (candidateLocalSpaceIndex == UINT32_MAX)
			{
				return false;
			}

			return getLocalSpaceIndex(sectorIndex) == candidateLocalSpaceIndex;
		};

		return
			matchesSector(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};
	auto groupMatchesVisibleSectors = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesVisible = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			return sectorMatchesVisibleSet(sectorIndex);
		};

		return
			matchesVisible(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};

	if (gi != nullptr)
	{
		for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
		{
			if (sector[sectorIndex].lotag != 848)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.providerSectorCount++;

			GeoEffect candidateEffect = {};
			if (!gi->GetGeoEffect(&candidateEffect, &sector[sectorIndex]) || candidateEffect.geocnt <= 0)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.geoProviderCount++;
			mRuntimeSpaceLinkLastFrame.providerGroupCount += (uint32_t)candidateEffect.geocnt;

			bool matched = false;
			bool visibleMatched = false;
			for (int i = 0; i < candidateEffect.geocnt; ++i)
			{
				if (groupMatchesCandidate(candidateEffect, i))
				{
					matched = true;
				}
				if (groupMatchesVisibleSectors(candidateEffect, i))
				{
					visibleMatched = true;
				}
			}

			if (matched)
			{
				mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount++;
			}
			if (visibleMatched)
			{
				mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount++;
			}

			if (providerSectorIndex >= 0 || !matched)
			{
				continue;
			}

			effect = candidateEffect;
			providerSectorIndex = (int)sectorIndex;
			break;
		}
	}

	if (providerSectorIndex < 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	mRuntimeSpaceLinkLastFrame.sourceSectorIndex = providerSectorIndex;
	mRuntimeSpaceLinkLastFrame.reportedGeoCount = effect.geocnt;
	if (effect.geocnt <= 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	struct RuntimeGeoLink
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
		float prevDx = 0.0f;
		float prevDz = 0.0f;
	};

	std::vector<RuntimeGeoLink> links;
	links.reserve((size_t)effect.geocnt * 2u);

	auto appendLink = [&](sectortype* warpedSector, double mapDx, double mapDy)
	{
		if (warpedSector == nullptr)
		{
			return;
		}

		const int32_t sectorIndex = sector.IndexOf(warpedSector);
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return;
		}

		RuntimeGeoLink link = {};
		link.chunkIndex = (uint32_t)sectorIndex;
		link.dx = (float)mapDx;
		link.dz = (float)-mapDy;
		for (const RuntimeGeoLink& existing : links)
		{
			if (existing.chunkIndex == link.chunkIndex &&
				fabs(existing.dx - link.dx) < 0.001f &&
				fabs(existing.dz - link.dz) < 0.001f)
			{
				return;
			}
		}

		links.push_back(link);
	};

	for (int i = 0; i < effect.geocnt; ++i)
	{
		if (!groupMatchesCandidate(effect, i))
		{
			continue;
		}

		appendLink(effect.geosectorwarp != nullptr ? effect.geosectorwarp[i] : nullptr,
			effect.geox != nullptr ? effect.geox[i] : 0.0,
			effect.geoy != nullptr ? effect.geoy[i] : 0.0);
		appendLink(effect.geosectorwarp2 != nullptr ? effect.geosectorwarp2[i] : nullptr,
			effect.geox2 != nullptr ? effect.geox2[i] : 0.0,
			effect.geoy2 != nullptr ? effect.geoy2[i] : 0.0);
	}

	if (links.empty())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const auto findPreviousTranslation = [&](uint32_t chunkIndex, float& outPrevDx, float& outPrevDz) -> bool
	{
		for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
		{
			if (previous.chunkIndex == chunkIndex)
			{
				outPrevDx = previous.dx;
				outPrevDz = previous.dz;
				return true;
			}
		}

		return false;
	};

	for (RuntimeGeoLink& link : links)
	{
		findPreviousTranslation(link.chunkIndex, link.prevDx, link.prevDz);
	}

	const auto runtimeLinkTopologyChanged = [&]() -> bool
	{
		if (links.size() != mRuntimeChunkTranslationHistory.size())
		{
			return true;
		}

		for (const RuntimeGeoLink& link : links)
		{
			bool found = false;
			for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
			{
				if (previous.chunkIndex == link.chunkIndex)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				return true;
			}
		}

		return false;
	};

	mRuntimeSpaceLinkLastFrame.geoEffectActive = true;
	mRuntimeSpaceLinkLastFrame.linkCount = (uint32_t)links.size();
	mRuntimeSpaceLinkLastFrame.topologyChanged = runtimeLinkTopologyChanged();
	if (mRuntimeSpaceLinkLastFrame.topologyChanged)
	{
		RequestHistoryReset("runtime-link-topology");
	}

	std::vector<RuntimeChunkTranslationState> nextRuntimeChunkTranslationHistory;
	nextRuntimeChunkTranslationHistory.reserve(links.size());

	for (const RuntimeGeoLink& link : links)
	{
		if (link.chunkIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		if (!nri_scene::BuildLiveMapChunkSceneView(mMapWorld.chunks[link.chunkIndex], liveChunkView, &liveStats))
		{
			continue;
		}

		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(liveChunkView, chunkGeometry);
			AssignGeometryPortalIndices(mMapWorld, chunkGeometry);
			TranslateGeometry(chunkGeometry, link.dx, 0.0f, link.dz, link.prevDx, 0.0f, link.prevDz);
		}
		{
			Clocker clock(NriPTMaterialBuild);
			nri_scene::BuildMaterials(liveChunkView, chunkMaterials);
		}

		if (!chunkGeometry.primitives.empty())
		{
			AppendGeometry(chunkGeometry, (uint32_t)outMaterials.materials.size(), outGeometry);
		}
		AppendMaterialBridge(chunkMaterials, outMaterials);

		mRuntimeSpaceLinkLastFrame.translatedChunkCount++;
		mRuntimeSpaceLinkLastFrame.surfaceCount += liveStats.surfaceCount;
		mRuntimeSpaceLinkLastFrame.triangleCount += liveStats.triangleCount;
		mRuntimeSpaceLinkLastFrame.materialCount += (uint32_t)chunkMaterials.materials.size();
		nextRuntimeChunkTranslationHistory.push_back({ link.chunkIndex, link.dx, link.dz });
	}

	mRuntimeChunkTranslationHistory = std::move(nextRuntimeChunkTranslationHistory);
	mRuntimeSpaceLinkLastFrame.active = !outGeometry.primitives.empty();
	return mRuntimeSpaceLinkLastFrame.active;
}

bool NRIRenderer::RestoreStaticTopLevelScene()
{
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		UpdateSceneDataSet(
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			mVertexBuffer,
			mIndexBuffer,
			mPrimitiveBuffer,
			mMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u);
}

bool NRIRenderer::RefreshResidentStaticSceneDataSet()
{
	std::vector<SceneInstanceData> sceneInstances;
	std::vector<nri::TopLevelInstance> ignoredInstances;
	BuildStaticMapInstances(ignoredInstances, sceneInstances);
	return UpdateSceneDataSet(
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		mVertexBuffer,
		mIndexBuffer,
		mPrimitiveBuffer,
		mMaterialBuffer,
		sceneInstances,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		0u,
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
		0u);
}

bool NRIRenderer::BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry)
{
	if (geometry.primitives.empty() || geometry.vertices.empty() || geometry.indices.empty())
	{
		return false;
	}

	nri::BottomLevelGeometryDesc dynamicGeometryDesc = {};
	dynamicGeometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
	dynamicGeometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
	dynamicGeometryDesc.triangles.vertexBuffer = mVertexBuffer.buffer;
	dynamicGeometryDesc.triangles.vertexOffset = 0;
	dynamicGeometryDesc.triangles.vertexNum = (uint32_t)geometry.vertices.size();
	dynamicGeometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
	dynamicGeometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
	dynamicGeometryDesc.triangles.indexBuffer = mIndexBuffer.buffer;
	dynamicGeometryDesc.triangles.indexOffset = 0;
	dynamicGeometryDesc.triangles.indexNum = (uint32_t)geometry.indices.size();
	dynamicGeometryDesc.triangles.indexType = nri::IndexType::UINT32;

	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);

	nri::AccelerationStructureDesc blasDesc = {};
	blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
	blasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_BUILD;
	blasDesc.geometryOrInstanceNum = 1;
	blasDesc.geometries = &dynamicGeometryDesc;
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, mDynamicBottomLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	const uint64_t requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mDynamicBottomLevelAS.accelerationStructure);
	if (mScratchBuffer.buffer == nullptr || mScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mScratchBuffer);
		if (!CreateBufferWithoutView(mScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	nri::BuildBottomLevelAccelerationStructureDesc dynamicBuild = {};
	dynamicBuild.dst = mDynamicBottomLevelAS.accelerationStructure;
	dynamicBuild.geometries = &dynamicGeometryDesc;
	dynamicBuild.geometryNum = 1;
	dynamicBuild.scratchBuffer = mScratchBuffer.buffer;
	dynamicBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &dynamicBuild, 1);

	nri::BufferBarrierDesc barrier = {};
	barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mDynamicBottomLevelAS.accelerationStructure);
	barrier.before = NRIAccelerationStructureWriteAccess();
	barrier.after = NRIAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &barrier;
	barrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	mBuiltDynamicSceneASLastFrame = true;
	mDynamicSceneLastFrame.asBuildCount++;
	return true;
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;

	if (!nri_ptemissivetlas ||
		mBoundEmissivePrimitiveRecords.empty() ||
		mBoundSceneInstances.empty())
	{
		DestroyBufferResource(mEmissiveTlasInstanceBuffer);
		DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
		return true;
	}

	std::unordered_map<uint32_t, uint32_t> staticSceneInstanceByPrimitiveOffset;
	staticSceneInstanceByPrimitiveOffset.reserve(mBoundSceneInstances.size());
	uint32_t dynamicSceneInstanceIndex = UINT32_MAX;
	for (uint32_t sceneInstanceIndex = 0; sceneInstanceIndex < (uint32_t)mBoundSceneInstances.size(); ++sceneInstanceIndex)
	{
		const SceneInstanceData& sceneInstance = mBoundSceneInstances[sceneInstanceIndex];
		if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
		{
			staticSceneInstanceByPrimitiveOffset.emplace(sceneInstance.primitiveOffset, sceneInstanceIndex);
		}
		else if (sceneInstance.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC && dynamicSceneInstanceIndex == UINT32_MAX)
		{
			dynamicSceneInstanceIndex = sceneInstanceIndex;
		}
	}

	std::vector<uint8_t> emissiveStaticChunks(mStaticMapScene.chunks.size(), 0u);
	bool includeDynamicInstance = false;
	const auto findStaticChunkIndexForPrimitive = [&](uint32_t primitiveIndex) -> int32_t
	{
		uint32_t low = 0;
		uint32_t high = (uint32_t)mStaticMapScene.chunks.size();
		while (low < high)
		{
			const uint32_t mid = (low + high) >> 1u;
			const auto& chunk = mStaticMapScene.chunks[mid];
			const uint32_t chunkBegin = chunk.primitiveOffset;
			const uint32_t chunkEnd = chunkBegin + chunk.primitiveCount;
			if (primitiveIndex < chunkBegin)
			{
				high = mid;
			}
			else if (primitiveIndex >= chunkEnd)
			{
				low = mid + 1u;
			}
			else
			{
				return (int32_t)mid;
			}
		}

		return -1;
	};

	for (const EmissivePrimitiveDebugRecord& record : mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == NRI_SCENE_DATA_SOURCE_STATIC)
		{
			const int32_t chunkIndex = findStaticChunkIndexForPrimitive(record.primitiveIndex);
			if (chunkIndex >= 0)
			{
				emissiveStaticChunks[(size_t)chunkIndex] = 1u;
			}
		}
		else if (record.dataSource == NRI_SCENE_DATA_SOURCE_DYNAMIC &&
			dynamicSceneInstanceIndex != UINT32_MAX &&
			mDynamicBottomLevelAS.accelerationStructure != nullptr)
		{
			includeDynamicInstance = true;
		}
	}

	std::vector<nri::TopLevelInstance> instances;
	instances.reserve(mStaticMapScene.chunks.size() + (includeDynamicInstance ? 1u : 0u));
	for (size_t chunkIndex = 0; chunkIndex < mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		if (emissiveStaticChunks[chunkIndex] == 0u)
		{
			continue;
		}

		const auto& chunk = mStaticMapScene.chunks[chunkIndex];
		if (chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		const auto sceneInstanceIt = staticSceneInstanceByPrimitiveOffset.find(chunk.primitiveOffset);
		if (sceneInstanceIt == staticSceneInstanceByPrimitiveOffset.end())
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = sceneInstanceIt->second;
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		instances.push_back(instance);
		mEmissiveTlasStaticInstanceCount++;
	}

	if (includeDynamicInstance)
	{
		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = dynamicSceneInstanceIndex;
		instance.mask = 0xFF;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*mDynamicBottomLevelAS.accelerationStructure);
		instances.push_back(instance);
		mEmissiveTlasDynamicInstanceCount = 1;
	}

	if (instances.empty())
	{
		DestroyBufferResource(mEmissiveTlasInstanceBuffer);
		DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
		return true;
	}

	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	if (!EnsureStructuredBuffer(
		mEmissiveTlasInstanceBuffer,
		mEmissiveTlasInstanceBufferStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIAccelerationStructureBuildInputAccess()))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, mEmissiveTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	const uint64_t requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mEmissiveTopLevelAS.accelerationStructure);
	if (mTopLevelScratchBuffer.buffer == nullptr || mTopLevelScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mTopLevelScratchBuffer);
		if (!CreateBufferWithoutView(mTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*mEmissiveTopLevelAS.accelerationStructure, mEmissiveTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = mEmissiveTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = mEmissiveTlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = mTopLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mEmissiveTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIComputeAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &tlasBarrier;
	barrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);

	mEmissiveTlasInstanceCount = (uint32_t)instances.size();
	mEmissiveTlasBuildCount++;
	return true;
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	if (instances.empty())
	{
		return false;
	}

	DestroyAccelerationStructureResource(mTopLevelAS);

	static SceneBufferDebugStats sTlasInstanceStats = { "TLASInstance" };
	if (!EnsureStructuredBuffer(
		mTlasInstanceBuffer,
		sTlasInstanceStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIAccelerationStructureBuildInputAccess()))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, mTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	const uint64_t requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mTopLevelAS.accelerationStructure);
	if (mTopLevelScratchBuffer.buffer == nullptr || mTopLevelScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mTopLevelScratchBuffer);
		if (!CreateBufferWithoutView(mTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*mTopLevelAS.accelerationStructure, mTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = mTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = mTlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = mTopLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIComputeAccelerationStructureReadAccess();

	std::vector<nri::BufferBarrierDesc> barriers;
	barriers.reserve(5);
	barriers.push_back(tlasBarrier);
	if ((sceneBufferMask & SceneDataBufferMask_Static) != 0)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = mStaticVertexBuffer.buffer;
		vertexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = mStaticIndexBuffer.buffer;
		indexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}
	if ((sceneBufferMask & SceneDataBufferMask_Dynamic) != 0)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = mVertexBuffer.buffer;
		vertexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = mIndexBuffer.buffer;
		indexBarrier.before = NRIAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers.data();
	barrierDesc.bufferNum = (uint32_t)barriers.size();
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);

	mActiveTlasInstanceCount = (uint32_t)instances.size();
	if ((sceneBufferMask & SceneDataBufferMask_Static) != 0 &&
		(sceneBufferMask & SceneDataBufferMask_Dynamic) == 0)
	{
		mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
		mStaticMapScene.accelerationResident = true;
		mBuiltStaticMapSceneASLastFrame = true;
	}
	return true;
}

bool NRIRenderer::DispatchFrameGraph(HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	Clocker clock(NriPTFrameGraph);

	static bool sLoggedPhaseBCompositionPath = false;
	static bool sLoggedPhaseGResolvedPresentPath = false;
	static bool sLoggedPhaseGDebugPrepassPath = false;
	static bool sLoggedPhaseFDenoiserPath = false;
	static bool sLoggedPhaseFDenoiserFallback = false;
	static bool sLoggedRawTraceBypass = false;
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool bootstrapRawTracePresent = nri_ptbootstrap && (bootstrapMode == 11u || bootstrapMode == 12u);
	const bool useResolvedPresent = !nri_ptbootstrap && nri_ptdebug == 0;
	const bool useComposedDebugPresent = !nri_ptbootstrap && nri_ptdebug == 15;
	const bool usePostCompositionDebugPresent = !nri_ptbootstrap && (nri_ptdebug == 13 || nri_ptdebug == 14);
	const bool useCompositionPath = useResolvedPresent || useComposedDebugPresent || usePostCompositionDebugPresent;
	const bool useValidationPresent = !nri_ptbootstrap && nri_ptdebug == 9;
	const bool useDenoisedDebugPresent = !nri_ptbootstrap && (nri_ptdebug == 16 || nri_ptdebug == 17);
	const bool useShadowDebugPresent = !nri_ptbootstrap && (nri_ptdebug >= 21 && nri_ptdebug <= 23);
	const bool useFinalDebugPresent = !nri_ptbootstrap &&
		((nri_ptdebug >= 5 && nri_ptdebug <= 8) || (nri_ptdebug >= 18 && nri_ptdebug <= 20) || useShadowDebugPresent || nri_ptdebug == 24 || nri_ptdebug == 25 || nri_ptdebug == 41 || nri_ptdebug == 42 || nri_ptdebug == 43);
	const bool rawTraceDirectPresent = !nri_ptbootstrap && !useCompositionPath && !useValidationPresent && !useDenoisedDebugPresent && !useFinalDebugPresent;
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::Upscaled;
	mUseUpscaledInFinal = false;
	mUseDenoisedCompositionInputs = false;
	mUseSplitShadowDenoiser = useShadowDebugPresent;

	if (!DispatchTraceOpaque(di, geometry, materials))
	{
		return false;
	}

	if (bootstrapRawTracePresent)
	{
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useValidationPresent)
	{
		if (!DispatchDenoiser())
		{
			return false;
		}

		if (!DispatchRawPresent(FrameTextureSlot::Validation))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useDenoisedDebugPresent)
	{
		if (!DispatchDenoiser())
		{
			return false;
		}

		const FrameTextureSlot denoisedSlot = nri_ptdebug == 16 ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::DenoisedSpecular;
		if (!DispatchRawPresent(denoisedSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useShadowDebugPresent)
	{
		if (nri_denoise && !DispatchDenoiser())
		{
			return false;
		}

		mUseUpscaledInFinal = false;
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	auto dispatchCompositionPath = [&]() -> bool
	{
		if (nri_denoise)
		{
			if (!sLoggedPhaseFDenoiserPath)
			{
				Printf("NRI Phase F: the Composition-backed PT paths now route through NRD before Composition when nri_denoise is enabled.\n");
				sLoggedPhaseFDenoiserPath = true;
			}

			if (!DispatchDenoiser())
			{
				if (!sLoggedPhaseFDenoiserFallback)
				{
					Printf(TEXTCOLOR_ORANGE "NRI Phase F: NRD dispatch failed in the composition path; falling back to raw trace inputs for this frame.\n");
					sLoggedPhaseFDenoiserFallback = true;
				}
			}
			else
			{
				mUseDenoisedCompositionInputs = true;
			}
		}

		if (!DispatchComposition())
		{
			return false;
		}

		return true;
	};

	if (useResolvedPresent)
	{
		if (!sLoggedPhaseGResolvedPresentPath)
		{
			Printf("NRI Phase G: ptdebug 0 now routes through Composition, DispatchUpscaleChain, and the minimal FinalPresent presenter.\n");
			sLoggedPhaseGResolvedPresentPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchUpscaleChain())
		{
			return false;
		}

		const FrameTextureSlot resolvedPresentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
		TraceTemporalState("resolved-present", ResolveUpscalerKind(false), false, resolvedPresentSlot, mHistoryOutputSlot);
		if (!DispatchFinalPresent(resolvedPresentSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useComposedDebugPresent)
	{
		if (!sLoggedPhaseBCompositionPath)
		{
			Printf("NRI Phase B: ptdebug 15 now routes through Composition and the minimal FinalPresent presenter.\n");
			sLoggedPhaseBCompositionPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchFinalPresent(FrameTextureSlot::Composed))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (usePostCompositionDebugPresent)
	{
		if (!sLoggedPhaseGDebugPrepassPath)
		{
			Printf("NRI Phase G: ptdebug 13/14 now route through Composition, DispatchUpscaleChain, and direct FinalPresent of the temporal outputs.\n");
			sLoggedPhaseGDebugPrepassPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchUpscaleChain())
		{
			return false;
		}

		const FrameTextureSlot debugSlot = nri_ptdebug == 13 ? mHistoryOutputSlot : mUpscaledInputSlot;
		TraceTemporalState("debug13-14-present", ResolveUpscalerKind(false), false, debugSlot, mHistoryOutputSlot);
		if (!DispatchFinalPresent(debugSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useFinalDebugPresent)
	{
		mUseUpscaledInFinal = false;
		if (!DispatchFinal())
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (rawTraceDirectPresent)
	{
		if (!sLoggedRawTraceBypass)
		{
			Printf("NRI frame-graph bypass: presenting raw TraceOpaque output through the direct present path for non-composition debug views.\n");
			sLoggedRawTraceBypass = true;
		}

		FrameTextureSlot rawPresentSlot = FrameTextureSlot::UnfilteredDiffuse;
		if (nri_ptdebug == 11 || nri_ptdebug == 12)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredSpecular;
		}

		if (nri_ptdebug == 12)
		{
			if (!DispatchRawPresent(rawPresentSlot, FrameTextureSlot::ViewZ, FrameTextureSlot::NormalRoughness))
			{
				return false;
			}
		}
		else if (!DispatchRawPresent(rawPresentSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (!sLoggedRawTraceBypass)
	{
		Printf("NRI frame-graph bypass: presenting raw TraceOpaque output until composition integration is stabilized.\n");
		sLoggedRawTraceBypass = true;
	}

	mUseUpscaledInFinal = false;
	if (!DispatchFinal())
	{
		return false;
	}

	CopyFinalToActiveTarget();
	return true;
}

bool NRIRenderer::DispatchTraceOpaque(HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	Clocker clock(NriPTTraceOpaque);

	if (!UpdateReprojectionBuffer())
	{
		return false;
	}

	NRITraceConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	const bool useTemporalJitter = !nri_ptbootstrap && ShouldUseTemporalJitter(ResolveUpscalerKind(false));
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(nri_ptdirectionallight ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u));
	constants.PortalCount = mBoundPortalCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.PortalDepth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	constants.ReservedTrace0 = (mBoundRuntimeLightTileCountX & 0xffffu) | ((mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)GetSelectedNrdDenoiserMode(),
		std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u));
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::PreFinal), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = GetFrameTexture(FrameTextureSlot::PreFinal).storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[0] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).storageView;
	mOutputDescriptors[3] = GetFrameTexture(FrameTextureSlot::Motion).storageView;
	mOutputDescriptors[4] = GetFrameTexture(FrameTextureSlot::ViewZ).storageView;
	mOutputDescriptors[5] = GetFrameTexture(FrameTextureSlot::NormalRoughness).storageView;
	mOutputDescriptors[6] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).storageView;
	mOutputDescriptors[9] = GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo).storageView;
	mOutputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).storageView;
	mOutputDescriptors[11] = GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance).storageView;
	mOutputDescriptors[12] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).storageView;
	mOutputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectLighting).storageView;
	mOutputDescriptors[14] = GetFrameTexture(FrameTextureSlot::DirectEmission).storageView;
	UpdateOutputSet();

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::TraceOpaque));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchDenoiser()
{
	Clocker clock(NriPTDenoiser);
	const uint32_t nrdMaxFrames = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);

	if (!mNrd.EnsureReady(*mFrameBuffer->mDevice, mRenderWidth, mRenderHeight, 1))
	{
		return false;
	}

	mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = mFrameBuffer->mCommandBuffer;
	desc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.viewZ = &GetFrameTexture(FrameTextureSlot::ViewZ);
	desc.normalRoughness = &GetFrameTexture(FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &GetFrameTexture(FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &GetFrameTexture(FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &GetFrameTexture(FrameTextureSlot::DenoisedShadow);
	desc.validation = &GetFrameTexture(FrameTextureSlot::Validation);
	desc.resourceWidth = mRenderWidth;
	desc.resourceHeight = mRenderHeight;
	desc.frameIndex = mFrameIndex;
	Copy2(mCurrentJitter, desc.cameraJitter);
	Copy2(mPreviousJitter, desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, mCurrentViewToClip, sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, mPreviousViewToClip, sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, mCurrentWorldToView, sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, mPreviousWorldToView, sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = 0.3f;
	desc.lightDirection[1] = 0.85f;
	desc.lightDirection[2] = -0.4f;
	Normalize3(desc.lightDirection);
	desc.denoiserMode = GetSelectedNrdDenoiserMode();
	desc.maxAccumulatedFrameNum = nrdMaxFrames;
	desc.maxFastAccumulatedFrameNum = ClampNrdFastFrameCount((int)nri_nrdfastframes, nrdMaxFrames);
	desc.maxStabilizedFrameNum = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, nrdMaxFrames);
	desc.hitDistanceReconstructionMode = GetNrdHitDistanceReconstructionMode();
	desc.fastHistoryClampingSigmaScale = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	desc.diffusePrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	desc.specularPrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	desc.minBlurRadius = ClampNrdBlurRadius((float)nri_nrdblurmin);
	desc.maxBlurRadius = std::max(desc.minBlurRadius, ClampNrdBlurRadius((float)nri_nrdblurmax));
	desc.sigmaMaxStabilizedFrameNum = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	desc.sigmaPlaneDistanceSensitivity = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);
	desc.resetHistory = mResetHistory;
	desc.enableAntiFirefly = nri_nrdantifirefly;
	desc.enableValidation = nri_validation;
	desc.enableSigmaShadow = mUseSplitShadowDenoiser;
	return mNrd.Denoise(desc);
}

bool NRIRenderer::DispatchComposition()
{
	Clocker clock(NriPTComposition);

	NRITraceConstants constants = {};
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u);
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = GetNrdInputSplitMode();
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	NRITextureResource& diffuse = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = GetFrameTexture(FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = GetFrameTexture(FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = GetFrameTexture(FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = GetFrameTexture(FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = GetFrameTexture(FrameTextureSlot::DirectEmission);
	const FrameTextureSlot filteredDiffuseSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::UnfilteredDiffuse;
	const FrameTextureSlot filteredSpecularSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedSpecular : FrameTextureSlot::UnfilteredSpecular;
	const FrameTextureSlot filteredShadowSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedShadow : FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = GetFrameTexture(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = GetFrameTexture(filteredSpecularSlot);
	NRITextureResource& filteredShadow = GetFrameTexture(filteredShadowSlot);
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);

	mFrameBuffer->TransitionTexture(diffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(specular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(viewZ, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directLighting, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(directEmission, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = viewZ.shaderView;
	mFrameInputDescriptors[3] = normalRoughness.shaderView;
	mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	mFrameInputDescriptors[5] = diffuse.shaderView;
	mFrameInputDescriptors[6] = specular.shaderView;
	mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	mFrameInputDescriptors[10] = rawShadow.shaderView;
	mFrameInputDescriptors[11] = filteredShadow.shaderView;
	mFrameInputDescriptors[12] = directLighting.shaderView;
	mFrameInputDescriptors[13] = directEmission.shaderView;
	UpdateFrameTextureSet(mCompositionFrameTextureSet, mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[1] = composed.storageView;
	UpdateOutputSet(mCompositionOutputSet, mOutputDescriptors);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mCompositionFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mCompositionOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Composition));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot, FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRITraceConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = GetFrameTexture(inputSlot);
	const bool addSecondary = secondarySlot != FrameTextureSlot::Count;
	NRITextureResource& secondary = GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != FrameTextureSlot::Count;
	NRITextureResource& tertiary = GetFrameTexture(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(secondary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(tertiary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::RawPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchFinalPresent(FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	NRITraceConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);

	NRITextureResource& input = GetFrameTexture(inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::FinalPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchUpscaleChain()
{
	Clocker clock(NriPTUpscale);

	const NRIUpscalerKind kind = ResolveUpscalerKind(true);
	const bool taaEligible = kind == NRIUpscalerKind::Off || kind == NRIUpscalerKind::NIS;
	const bool runAppTaa = taaEligible && !!nri_pttaa;
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	TraceTemporalState("upscale-entry", kind, runAppTaa, mHistoryOutputSlot, FrameTextureSlot::Composed);

	if (runAppTaa)
	{
		NRITraceConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.DisplayWidth = mOutputWidth;
		constants.DisplayHeight = mOutputHeight;
		constants.FrameIndex = mFrameIndex;
		constants.DebugMode = (uint32_t)nri_ptdebug;
		constants.Flags =
			(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(runAppTaa ? NRI_FLAG_USE_JITTER : 0u);

		mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[3] = {
			historyInput.shaderView,
			GetFrameTexture(FrameTextureSlot::Motion).shaderView,
			composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		mFrameBuffer->mCore.UpdateDescriptorRanges(&taaOutputUpdate, 1);

		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mTaaOutputSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Taa));
		mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	}
	else if (taaEligible)
	{
		CopyTexture(composed, historyOutput);
	}

	if (kind == NRIUpscalerKind::Off)
	{
		mUseUpscaledInFinal = false;
		mUpscaledInputSlot = mHistoryOutputSlot;
		TraceTemporalState("upscale-native", kind, runAppTaa, mHistoryOutputSlot, mUpscaledInputSlot);
		return true;
	}

	if (kind == NRIUpscalerKind::NIS)
	{
		mFrameBuffer->TransitionTexture(historyOutput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Upscaled), NRIComputeStorageState());
		if (!mUpscaler.EnsureReady(*mFrameBuffer, kind, nri::UpscalerMode::QUALITY, mOutputWidth, mOutputHeight))
		{
			return false;
		}

		NRIUpscalerDispatchDesc desc = {};
		desc.commandBuffer = mFrameBuffer->mCommandBuffer;
		desc.input = &historyOutput;
		desc.output = &GetFrameTexture(FrameTextureSlot::Upscaled);
		desc.currentWidth = mRenderWidth;
		desc.currentHeight = mRenderHeight;
		Copy2(mCurrentJitter, desc.cameraJitter);
		desc.sharpness = Clamp01((float)nri_sharpness);
		desc.resetHistory = mResetHistory;
		if (!mUpscaler.Dispatch(*mFrameBuffer, kind, desc))
		{
			return false;
		}

		mUseUpscaledInFinal = true;
		mUpscaledInputSlot = FrameTextureSlot::Upscaled;
		TraceTemporalState("upscale-nis", kind, runAppTaa, mUpscaledInputSlot, mHistoryOutputSlot);
		return true;
	}

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = composed.shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = GetFrameTexture(FrameTextureSlot::PreFinal).storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[9] = GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo).storageView;
	mOutputDescriptors[10] = GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo).storageView;
	mOutputDescriptors[11] = GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance).storageView;
	UpdateOutputSet();

	{
		NRITraceConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.DisplayWidth = mOutputWidth;
		constants.DisplayHeight = mOutputHeight;
		constants.FrameIndex = mFrameIndex;
		constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	}
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::DlssBefore));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Upscaled), NRIComputeStorageState());

	if (!mUpscaler.EnsureReady(*mFrameBuffer, kind, GetSelectedUpscalerMode(), mOutputWidth, mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc upscalerDesc = {};
	upscalerDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
	upscalerDesc.input = &GetFrameTexture(FrameTextureSlot::Composed);
	upscalerDesc.output = &GetFrameTexture(FrameTextureSlot::Upscaled);
	upscalerDesc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
	upscalerDesc.depth = &GetFrameTexture(FrameTextureSlot::ViewZ);
	upscalerDesc.normalRoughness = &GetFrameTexture(FrameTextureSlot::NormalRoughness);
	upscalerDesc.diffuseAlbedo = &GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo);
	upscalerDesc.specularAlbedo = &GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo);
	upscalerDesc.specularHitDistance = &GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance);
	upscalerDesc.currentWidth = mRenderWidth;
	upscalerDesc.currentHeight = mRenderHeight;
	Copy2(mCurrentJitter, upscalerDesc.cameraJitter);
	std::memcpy(upscalerDesc.viewToClipMatrix, mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
	std::memcpy(upscalerDesc.worldToViewMatrix, mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
	upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
	upscalerDesc.resetHistory = mResetHistory;
	if (!mUpscaler.Dispatch(*mFrameBuffer, kind, upscalerDesc))
	{
		return false;
	}

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Upscaled), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::PreFinal), NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[6] = GetFrameTexture(FrameTextureSlot::Upscaled).shaderView;
	UpdateFrameTextureSet();

	mOutputDescriptors.fill(GetFrameTexture(FrameTextureSlot::PreFinal).storageView);
	mOutputDescriptors[2] = GetFrameTexture(FrameTextureSlot::PreFinal).storageView;
	UpdateOutputSet();

	{
		NRITraceConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.DisplayWidth = mOutputWidth;
		constants.DisplayHeight = mOutputHeight;
		constants.FrameIndex = mFrameIndex;
		constants.Flags = NRI_FLAG_USE_UPSCALED | (mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u);
		mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
		mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
		mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	}
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::DlssAfter));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mOutputWidth), GetDispatchSize(mOutputHeight), 1 });

	mUseUpscaledInFinal = true;
	mUpscaledInputSlot = FrameTextureSlot::PreFinal;
	TraceTemporalState("upscale-vendor", kind, runAppTaa, mUpscaledInputSlot, FrameTextureSlot::Composed);
	return true;
}

bool NRIRenderer::DispatchFinal()
{
	Clocker clock(NriPTFinal);

	NRITraceConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(mCurrentCameraPos, constants.CameraPos);
	Copy3(mCurrentCameraForward, constants.CameraForward);
	Copy3(mCurrentCameraRight, constants.CameraRight);
	Copy3(mCurrentCameraUp, constants.CameraUp);
	Copy3(mPreviousCameraPos, constants.PrevCameraPos);
	Copy3(mPreviousCameraForward, constants.PrevCameraForward);
	Copy3(mPreviousCameraRight, constants.PrevCameraRight);
	Copy3(mPreviousCameraUp, constants.PrevCameraUp);
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.TanHalfFovX = mCurrentTanHalfFovX;
	constants.TanHalfFovY = mCurrentTanHalfFovY;
	constants.PrevTanHalfFovX = mPreviousTanHalfFovX;
	constants.PrevTanHalfFovY = mPreviousTanHalfFovY;
	constants.SceneInstanceCount = mSceneInstanceBuffer.stride != 0 ? (uint32_t)(mSceneInstanceBuffer.usedSize / mSceneInstanceBuffer.stride) : 0u;
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	NRITextureResource& history = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& upscaled = GetFrameTexture(mUpscaledInputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::PreFinal), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	mFrameInputDescriptors.fill(GetFrameTexture(FrameTextureSlot::Composed).shaderView);
	mFrameInputDescriptors[0] = history.shaderView;
	mFrameInputDescriptors[1] = GetFrameTexture(FrameTextureSlot::Motion).shaderView;
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
	mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
	mFrameInputDescriptors[5] = presentRawTrace ? (mUseUpscaledInFinal ? upscaled.shaderView : GetFrameTexture(FrameTextureSlot::Composed).shaderView) : GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors[6] = upscaled.shaderView;
	mFrameInputDescriptors[7] = GetFrameTexture(FrameTextureSlot::Validation).shaderView;
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::DlssSpecularAlbedo).shaderView;
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra).shaderView;
	mFrameInputDescriptors[11] = GetFrameTexture(FrameTextureSlot::DenoisedShadow).shaderView;
	mFrameInputDescriptors[12] = GetFrameTexture(FrameTextureSlot::DirectLighting).shaderView;
	mFrameInputDescriptors[13] = GetFrameTexture(FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		mFrameInputDescriptors[5] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	UpdateFrameTextureSet();

	mOutputDescriptors.fill(final.storageView);
	mOutputDescriptors[2] = final.storageView;
	UpdateOutputSet();

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::Final));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

void NRIRenderer::UpdatePerFrameState(HWDrawInfo& di)
{
	Clocker clock(NriPTUpdateState);

	if (mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	VSMatrix inverseView;
	if (!di.VPUniforms.mViewMatrix.inverseMatrix(inverseView))
	{
		std::memset(mCurrentCameraPos, 0, sizeof(mCurrentCameraPos));
		std::memset(mCurrentCameraForward, 0, sizeof(mCurrentCameraForward));
		std::memset(mCurrentCameraRight, 0, sizeof(mCurrentCameraRight));
		std::memset(mCurrentCameraUp, 0, sizeof(mCurrentCameraUp));
		mCurrentCameraForward[2] = -1.0f;
		mCurrentCameraRight[0] = 1.0f;
		mCurrentCameraUp[1] = 1.0f;
	}
	else
	{
		float origin[4] = {};
		float rightPoint[4] = {};
		float upPoint[4] = {};
		float forwardPoint[4] = {};
		TransformPoint(inverseView, 0.0f, 0.0f, 0.0f, origin);
		TransformPoint(inverseView, 1.0f, 0.0f, 0.0f, rightPoint);
		TransformPoint(inverseView, 0.0f, 1.0f, 0.0f, upPoint);
		TransformPoint(inverseView, 0.0f, 0.0f, -1.0f, forwardPoint);

		const float cameraPos[3] = {
			origin[0],
			origin[1],
			origin[2]
		};
		const float rightDelta[3] = {
			rightPoint[0] - origin[0],
			rightPoint[1] - origin[1],
			rightPoint[2] - origin[2]
		};
		const float upDelta[3] = {
			upPoint[0] - origin[0],
			upPoint[1] - origin[1],
			upPoint[2] - origin[2]
		};
		const float forwardDelta[3] = {
			forwardPoint[0] - origin[0],
			forwardPoint[1] - origin[1],
			forwardPoint[2] - origin[2]
		};

		Copy3(cameraPos, mCurrentCameraPos);
		Copy3(rightDelta, mCurrentCameraRight);
		Copy3(upDelta, mCurrentCameraUp);
		Copy3(forwardDelta, mCurrentCameraForward);

		Normalize3(mCurrentCameraRight);
		Normalize3(mCurrentCameraUp);
		Normalize3(mCurrentCameraForward);
	}

	const float* projection = di.VPUniforms.mProjectionMatrix.get();
	const float projectionScaleX = projection != nullptr ? std::fabs(projection[0]) : 0.0f;
	const float projectionScaleY = projection != nullptr ? std::fabs(projection[5]) : 0.0f;
	if (projectionScaleX > 0.0001f && projectionScaleY > 0.0001f)
	{
		// Match the hardware backend frustum exactly instead of rebuilding Y-FOV from the PT render dimensions.
		mCurrentTanHalfFovX = 1.0f / projectionScaleX;
		mCurrentTanHalfFovY = 1.0f / projectionScaleY;
	}
	else
	{
		const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
		mCurrentTanHalfFovX = tanHalfFovX;
		mCurrentTanHalfFovY = tanHalfFovX * ((float)mRenderHeight / std::max(1.0f, (float)mRenderWidth));
	}
	const NRIUpscalerKind resolvedUpscaler = ResolveUpscalerKind(false);
	if (!nri_ptbootstrap && ShouldUseTemporalJitter(resolvedUpscaler))
	{
		ComputeTemporalJitter(mFrameIndex, mCurrentJitter);
	}
	else
	{
		mCurrentJitter[0] = 0.0f;
		mCurrentJitter[1] = 0.0f;
	}
	FillMatrix(mCurrentViewToClip, di.VPUniforms.mProjectionMatrix);
	FillMatrix(mCurrentWorldToView, di.VPUniforms.mViewMatrix);
	if (nri_pttraceframes > 0)
	{
		const uint32_t targetWidth = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->width : 0u;
		const uint32_t targetHeight = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->height : 0u;
		const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
		const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
		const int32_t sceneWidth = mFrameBuffer->mSceneViewport.width;
		const int32_t sceneHeight = mFrameBuffer->mSceneViewport.height;
		const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - sceneHeight;
		const auto& uniformCameraPos = di.VPUniforms.mCameraPos;
		const FVector3 hwForward(di.Viewpoint.HWAngles);
		Printf("NRI PT camera: frame=%u hw_pitch=%.3f hw_yaw=%.3f hw_roll=%.3f scene_bl=(%d,%d %dx%d) scene_tl=(%u,%u %ux%u) target=%ux%u uniform_pos=(%.3f,%.3f,%.3f) inverse_pos=(%.3f,%.3f,%.3f) hw_forward=(%.3f,%.3f,%.3f) basis_fwd=(%.3f,%.3f,%.3f) basis_right=(%.3f,%.3f,%.3f) basis_up=(%.3f,%.3f,%.3f) tan=(%.6f,%.6f) proj=(%.6f,%.6f,%.6f,%.6f)\n",
			mFrameIndex,
			di.Viewpoint.HWAngles.Pitch.Degrees(),
			di.Viewpoint.HWAngles.Yaw.Degrees(),
			di.Viewpoint.HWAngles.Roll.Degrees(),
			mFrameBuffer->mSceneViewport.left,
			mFrameBuffer->mSceneViewport.top,
			mFrameBuffer->mSceneViewport.width,
			mFrameBuffer->mSceneViewport.height,
			sceneLeft,
			sceneTop,
			sceneWidth,
			sceneHeight,
			targetWidth,
			targetHeight,
			uniformCameraPos.X,
			uniformCameraPos.Y,
			uniformCameraPos.Z,
			mCurrentCameraPos[0],
			mCurrentCameraPos[1],
			mCurrentCameraPos[2],
			hwForward.X,
			hwForward.Y,
			hwForward.Z,
			mCurrentCameraForward[0],
			mCurrentCameraForward[1],
			mCurrentCameraForward[2],
			mCurrentCameraRight[0],
			mCurrentCameraRight[1],
			mCurrentCameraRight[2],
			mCurrentCameraUp[0],
			mCurrentCameraUp[1],
			mCurrentCameraUp[2],
			mCurrentTanHalfFovX,
			mCurrentTanHalfFovY,
			projection != nullptr ? projection[0] : 0.0f,
			projection != nullptr ? projection[5] : 0.0f,
			projection != nullptr ? projection[8] : 0.0f,
			projection != nullptr ? projection[9] : 0.0f);
	}

	if (!mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!nri_ptscenestats)
	{
		mLastStats = stats;
		mHasLoggedStats = true;
		return;
	}

	if (!mHasLoggedStats || StatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey)
{
	if (nri_pttraceframes <= 0)
	{
		return;
	}

	const SkyState tracedState = {
		sceneView.sky.mode,
		sceneView.sky.sourceType,
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		sceneView.sky.flipTop
	};

	const bool changed =
		!mHasTracedSkyState ||
		mLastTracedSkyState.mode != tracedState.mode ||
		mLastTracedSkyState.sourceType != tracedState.sourceType ||
		mLastTracedSkyState.texture != tracedState.texture ||
		mLastTracedSkyState.faceMask != tracedState.faceMask ||
		mLastTracedSkyState.flipTop != tracedState.flipTop ||
		mLastTracedSkyResolvedKey != resolvedKey;

	if (!changed && action == nullptr)
	{
		return;
	}

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	Printf("NRI PT sky: captured_mode=%s source=%s texture=%p face_mask=0x%x flip_top=%s skies=%u color=(%.3f, %.3f, %.3f) action=%s resolved_key=0x%llx active_mode=%s active_key=0x%llx active_size=%ux%u\n",
		GetSkyModeName(sceneView.sky.mode),
		GetSkySourceTypeName(sceneView.sky.sourceType),
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		sceneView.sky.flipTop ? "true" : "false",
		sceneView.stats.skySurfaces,
		sceneView.skyColor[0],
		sceneView.skyColor[1],
		sceneView.skyColor[2],
		action != nullptr ? action : "unchanged",
		(unsigned long long)resolvedKey,
		GetSkyModeName(mSkyState.mode),
		(unsigned long long)mSkyTextureKey,
		activeSkyTexture != nullptr ? activeSkyTexture->width : 0,
		activeSkyTexture != nullptr ? activeSkyTexture->height : 0);

	mLastTracedSkyState = tracedState;
	mLastTracedSkyResolvedKey = resolvedKey;
	mHasTracedSkyState = true;
}

void NRIRenderer::CopyFinalToActiveTarget()
{
	Clocker clock(NriPTCopyFinal);

	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	CopyTextureToActiveTarget(final);
}

void NRIRenderer::CopyTexture(NRITextureResource& source, NRITextureResource& destination)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(destination, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *destination.texture, nullptr, *source.texture, nullptr);
}

void NRIRenderer::CopyTextureToActiveTarget(NRITextureResource& source)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(*mFrameBuffer->mActiveTarget, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *mFrameBuffer->mActiveTarget->texture, nullptr, *source.texture, nullptr);
	mFrameBuffer->mRenderState->NotifyExternalTargetWrite();
}

void NRIRenderer::DestroyCachedTextures()
{
	mStaticMapScene.texturesResident = false;
	for (auto& skyTexture : mSkyTextureCache)
	{
		mFrameBuffer->DestroyTextureResource(skyTexture.resource);
	}
	mSkyTextureCache.clear();
	mActiveSkyTextureIndex = UINT32_MAX;
	mSkyTextureKey = 0;
	mSkyLevel = nullptr;
	mSkyState = {};
	mLastTracedSkyState = {};
	mLastTracedSkyResolvedKey = 0;
	mHasTracedSkyState = false;
	for (auto& texture : mTextureCache)
	{
		mFrameBuffer->DestroyTextureResource(texture.resource);
	}
	mTextureCache.clear();
}

void NRIRenderer::DestroyFrameTextures()
{
	for (auto& texture : mFrameTextures)
	{
		mFrameBuffer->DestroyTextureResource(texture);
	}
	mRenderWidth = 0;
	mRenderHeight = 0;
	mOutputWidth = 0;
	mOutputHeight = 0;
	mTargetWidth = 0;
	mTargetHeight = 0;
	mSceneLeft = 0;
	mSceneTop = 0;
	mOutputFormat = nri::Format::UNKNOWN;
}

void NRIRenderer::DestroySceneBuffers()
{
	mStaticMapScene.buffersResident = false;
	ResetPersistentDynamicEmissiveCache();
	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mPortalBuffer);
	DestroyBufferResource(mRuntimeLightBuffer);
	DestroyBufferResource(mRuntimeLightTileHeaderBuffer);
	DestroyBufferResource(mRuntimeLightTileIndexBuffer);
	DestroyBufferResource(mEmissivePrimitiveHeaderBuffer);
	DestroyBufferResource(mEmissivePrimitiveBuffer);
	DestroyBufferResource(mEmissivePrimitiveCdfBuffer);
	DestroyBufferResource(mSectorLightHeaderBuffer);
	DestroyBufferResource(mSectorLightBuffer);
	DestroyBufferResource(mReprojectionBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mBoundSceneInstances.clear();
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;
}

void NRIRenderer::DestroyAccelerationStructures()
{
	mStaticMapScene.accelerationResident = false;
	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}
	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	mStaticAccelerationBuildSerial = 0;
	mActiveTlasInstanceCount = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
}

void NRIRenderer::DestroyStaticMapSceneCache()
{
	ResetPersistentDynamicEmissiveCache();
	const bool hasResidentStaticSceneResources =
		!mStaticMapScene.chunks.empty() ||
		mStaticVertexBuffer.buffer != nullptr ||
		mStaticIndexBuffer.buffer != nullptr ||
		mStaticPrimitiveBuffer.buffer != nullptr ||
		mStaticMaterialBuffer.buffer != nullptr;
	if (hasResidentStaticSceneResources && mFrameBuffer != nullptr)
	{
		// The resident PT static scene can still be referenced by the previous frame's
		// TLAS and descriptor bindings. Wait before tearing it down for live rebuilds.
		mFrameBuffer->WaitForCommands(true);
	}

	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
	}

	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mRuntimeMapMutations.chunks.clear();
	mRuntimeMapMutations.replacedChunkMask.clear();
	mRuntimeMapLastFrame = {};
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
	resource.usedSize = 0;
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

bool NRIRenderer::IsUpscalerSupported(NRIUpscalerKind kind) const
{
	if (kind == NRIUpscalerKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIUpscalerKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, ToUpscalerType(kind));
}

NRIUpscalerKind NRIRenderer::ResolveUpscalerKind(bool logFallback)
{
	const NRIUpscalerKind requested = GetSelectedUpscalerKind();
	NRIUpscalerKind resolved = requested;

	switch (requested)
	{
	case NRIUpscalerKind::DLRR:
		if (!IsUpscalerSupported(NRIUpscalerKind::DLRR))
		{
			resolved =
				IsUpscalerSupported(NRIUpscalerKind::DLSR) ? NRIUpscalerKind::DLSR :
				IsUpscalerSupported(NRIUpscalerKind::NIS) ? NRIUpscalerKind::NIS :
				NRIUpscalerKind::Off;
		}
		break;

	case NRIUpscalerKind::DLSR:
		if (!IsUpscalerSupported(NRIUpscalerKind::DLSR))
		{
			resolved =
				IsUpscalerSupported(NRIUpscalerKind::NIS) ? NRIUpscalerKind::NIS :
				NRIUpscalerKind::Off;
		}
		break;

	case NRIUpscalerKind::NIS:
		if (!IsUpscalerSupported(NRIUpscalerKind::NIS))
		{
			resolved = NRIUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastUpscalerRequest != (int)nri_upscaler || mLastUpscalerResolved != resolved))
	{
		Printf("NRI upscaler fallback: requested %s is unavailable on %s, using %s\n",
			GetUpscalerName(requested),
			(const char*)nri_api,
			GetUpscalerName(resolved));
		mLastUpscalerRequest = (int)nri_upscaler;
		mLastUpscalerResolved = resolved;
	}

	return resolved;
}

const char* NRIRenderer::GetFrameTextureSlotName(FrameTextureSlot slot) const
{
	switch (slot)
	{
	case FrameTextureSlot::ViewZ: return "ViewZ";
	case FrameTextureSlot::Motion: return "Motion";
	case FrameTextureSlot::NormalRoughness: return "NormalRoughness";
	case FrameTextureSlot::BaseColorMetalness: return "BaseColorMetalness";
	case FrameTextureSlot::UnfilteredDiffuse: return "UnfilteredDiffuse";
	case FrameTextureSlot::UnfilteredSpecular: return "UnfilteredSpecular";
	case FrameTextureSlot::UnfilteredPenumbra: return "UnfilteredPenumbra";
	case FrameTextureSlot::DenoisedDiffuse: return "DenoisedDiffuse";
	case FrameTextureSlot::DenoisedSpecular: return "DenoisedSpecular";
	case FrameTextureSlot::DenoisedShadow: return "DenoisedShadow";
	case FrameTextureSlot::Composed: return "Composed";
	case FrameTextureSlot::DirectLighting: return "DirectLighting";
	case FrameTextureSlot::DirectEmission: return "DirectEmission";
	case FrameTextureSlot::TaaHistoryPing: return "TaaHistoryPing";
	case FrameTextureSlot::TaaHistoryPong: return "TaaHistoryPong";
	case FrameTextureSlot::Validation: return "Validation";
	case FrameTextureSlot::DlssDiffuseAlbedo: return "DlssDiffuseAlbedo";
	case FrameTextureSlot::DlssSpecularAlbedo: return "DlssSpecularAlbedo";
	case FrameTextureSlot::DlssSpecularHitDistance: return "DlssSpecularHitDistance";
	case FrameTextureSlot::DlssNormalRoughness: return "DlssNormalRoughness";
	case FrameTextureSlot::Upscaled: return "Upscaled";
	case FrameTextureSlot::PreFinal: return "PreFinal";
	case FrameTextureSlot::Final: return "Final";
	case FrameTextureSlot::Count: return "Count";
	default: return "Unknown";
	}
}

NRIUpscalerKind NRIRenderer::GetSelectedUpscalerKind() const
{
	switch ((int)nri_upscaler)
	{
	default:
	case 0: return NRIUpscalerKind::Off;
	case 1: return NRIUpscalerKind::NIS;
	case 2: return NRIUpscalerKind::DLSR;
	case 3: return NRIUpscalerKind::DLRR;
	}
}

NRIUpscalerKind NRIRenderer::GetResolvedUpscalerKindForStatus() const
{
	const NRIUpscalerKind requested = GetSelectedUpscalerKind();

	switch (requested)
	{
	case NRIUpscalerKind::DLRR:
		if (!IsUpscalerSupported(NRIUpscalerKind::DLRR))
		{
			return
				IsUpscalerSupported(NRIUpscalerKind::DLSR) ? NRIUpscalerKind::DLSR :
				IsUpscalerSupported(NRIUpscalerKind::NIS) ? NRIUpscalerKind::NIS :
				NRIUpscalerKind::Off;
		}
		break;

	case NRIUpscalerKind::DLSR:
		if (!IsUpscalerSupported(NRIUpscalerKind::DLSR))
		{
			return IsUpscalerSupported(NRIUpscalerKind::NIS) ? NRIUpscalerKind::NIS : NRIUpscalerKind::Off;
		}
		break;

	case NRIUpscalerKind::NIS:
		if (!IsUpscalerSupported(NRIUpscalerKind::NIS))
		{
			return NRIUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	return requested;
}

nri::UpscalerMode NRIRenderer::GetSelectedUpscalerMode() const
{
	switch ((int)nri_upscalermode)
	{
	default:
	case 0: return nri::UpscalerMode::NATIVE;
	case 1: return nri::UpscalerMode::ULTRA_QUALITY;
	case 2: return nri::UpscalerMode::QUALITY;
	case 3: return nri::UpscalerMode::BALANCED;
	case 4: return nri::UpscalerMode::PERFORMANCE;
	case 5: return nri::UpscalerMode::ULTRA_PERFORMANCE;
	}
}

void NRIRenderer::FillMatrix(float* outMatrix, const VSMatrix& matrix) const
{
	const_cast<VSMatrix&>(matrix).copy(outMatrix);
}
