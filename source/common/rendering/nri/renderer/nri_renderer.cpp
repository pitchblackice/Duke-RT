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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <windows.h>

CVAR(Int, nri_ptdebug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_denoise, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscaler, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscalermode, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_sharpness, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_validation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_apivalidation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_dred, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptbootstrap, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptbootstrapmode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptdirectscene, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptlightbounces, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptmirrorbounces, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsurfaceprobe, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 256;
	constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
	constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 9;
	constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 11;
	constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 12;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_DYNAMIC = 1;
	constexpr uint32_t NRI_SAMPLER_DESCRIPTOR_NUM = 4;
	constexpr uint32_t NRI_FLAG_RESET_HISTORY = 0x1u;
	constexpr uint32_t NRI_FLAG_USE_UPSCALED = 0x2u;
	constexpr uint32_t NRI_FLAG_BOOTSTRAP_VIEW = 0x4u;
	constexpr uint32_t NRI_FLAG_PRESENT_RAW_TRACE = 0x8u;
	constexpr uint32_t NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;

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

	static nri::AccessStage NRIAccelerationStructureReadAccess()
	{
		return { nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::ACCELERATION_STRUCTURE };
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

		for (const auto& material : source.materials)
		{
			nri_scene::MaterialData copy = material;
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

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
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
		EnsureFrameResources(mFrameBuffer->mActiveTarget->width, mFrameBuffer->mActiveTarget->height);
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
	RefreshMapWorld();
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
	nri_scene::GeometryData dynamicGeometry;
	nri_scene::GeometryData combinedGeometry;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData combinedMaterialBridge;
	std::vector<nri_scene::MaterialData> capturedGpuMaterials;
	std::vector<nri_scene::MaterialData> dynamicGpuMaterials;
	std::vector<nri_scene::MaterialData> combinedGpuMaterials;
	const nri_scene::SceneView* activeSceneView = nullptr;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;

	if (allowStaticMapScene && EnsureStaticMapScene())
	{
		mUsedStaticMapSceneLastFrame = true;
		activeSceneView = &mStaticMapScene.sceneView;
		activeGeometry = &mStaticMapScene.geometry;
		activeGpuMaterials = &mStaticMapScene.gpuMaterials;
		activeStats = mStaticMapScene.sceneView.stats;

		const bool deferDynamicSceneThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
		const bool hasDynamicScene = !deferDynamicSceneThisFrame && nri_scene::CaptureDynamicScene(di, dynamicSceneView);
		if (hasDynamicScene)
		{
			{
				Clocker clock(NriPTGeometryBuild);
				nri_scene::BuildGeometry(dynamicSceneView, dynamicGeometry);
			}

			if (!dynamicGeometry.primitives.empty())
			{
				{
					Clocker clock(NriPTMaterialBuild);
					nri_scene::BuildMaterials(dynamicSceneView, dynamicMaterialBridge);
				}

				combinedMaterialBridge = mStaticMapScene.materialBridge;
				AppendMaterialBridge(dynamicMaterialBridge, combinedMaterialBridge);
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
				buffersReady = texturesReady && UploadSceneBuffers(dynamicGeometry, dynamicGpuMaterials);
				accelerationReady = false;
				if (buffersReady)
				{
					accelerationReady =
						BuildDynamicAccelerationStructure(dynamicGeometry) &&
						mDynamicBottomLevelAS.accelerationStructure != nullptr;
				}
				if (accelerationReady)
				{
					std::vector<nri::TopLevelInstance> instances;
					std::vector<SceneInstanceData> sceneInstances;
					BuildStaticMapInstances(instances, sceneInstances);

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
							(uint32_t)dynamicGeometry.primitives.size(),
							(uint32_t)mStaticMapScene.gpuMaterials.size(),
							(uint32_t)dynamicGpuMaterials.size());
				}
				if (texturesReady)
				{
					PrepareSceneTextureInputsForCompute();
				}

				if (paletteReady && texturesReady && buffersReady && accelerationReady)
				{
					mUsedDynamicSceneLastFrame = true;
					mGpuSceneHasDynamicOverlay = true;
					mDynamicSceneLastFrame.spriteSurfaceCount = (uint32_t)dynamicSceneView.opaqueSprites.size();
					mDynamicSceneLastFrame.primitiveCount = (uint32_t)dynamicGeometry.primitives.size();
					mDynamicSceneLastFrame.materialCount = (uint32_t)dynamicGpuMaterials.size();
					mDynamicSceneLastFrame.modelCount = dynamicSceneView.stats.modelDrawItems;
					mDynamicSceneLastFrame.unsupportedModelCount = dynamicSceneView.stats.unsupportedModelDrawItems;
					combinedGeometry = mStaticMapScene.geometry;
					AppendGeometry(dynamicGeometry, (uint32_t)mStaticMapScene.materialBridge.materials.size(), combinedGeometry);
					activeGeometry = &combinedGeometry;
					activeGpuMaterials = &combinedGpuMaterials;
					activeStats = MergeSceneStats(mStaticMapScene.sceneView.stats, dynamicSceneView.stats);
				}
				else
				{
					LogFallback("PT dynamic scene update failed; tracing the resident static world only.");
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
		}
		else if (deferDynamicSceneThisFrame)
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
			activeStats = mStaticMapScene.sceneView.stats;
		}
		else
		{
			mGpuSceneHasDynamicOverlay = false;
		}
	}
	else
	{
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
		activeStats = capturedSceneView.stats;

		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(capturedSceneView, capturedGeometry);
		}

		{
			Clocker clock(NriPTMaterialBuild);
			nri_scene::BuildMaterials(capturedSceneView, materialBridge);
		}

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
	}

	if (activeSceneView == nullptr || activeGeometry == nullptr || activeGpuMaterials == nullptr)
	{
		LogFallback("PT scene selection failed.");
		if (preserveHistory)
		{
			restoreHistory();
		}
		return false;
	}

	LogBridgeStats(activeStats);
	if (activeStats.unsupportedModelDrawItems > 0)
	{
		LogFallback("generic GLDL_MODELS content is unsupported in the PT bridge; rendering the supported PT scene without those model draws.");
	}

	Copy3(activeSceneView->skyColor, mSkyColor);
	Copy3(activeSceneView->groundColor, mGroundColor);

	if (!preserveHistory)
	{
		UpdateSurfaceProbe(*activeGeometry, true);
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
	mResetHistory = true;
	mHasPreviousCameraState = false;
}

void NRIRenderer::PrintStatus() const
{
	const NRIUpscalerKind requested = GetSelectedUpscalerKind();
	const NRIUpscalerKind resolved = GetResolvedUpscalerKindForStatus();
	const uint32_t bootstrapMode = GetBootstrapMode();

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
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u surface_probe=%d\n",
		nri_ptdirectscene ? "on" : "off",
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		(int)nri_ptsurfaceprobe);
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
	PrintStaticMapSceneStatus();
	PrintDynamicSceneStatus();
	PrintSceneBufferStatus();
	PrintSurfaceProbeStatus();
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
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
	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount);
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
}

void NRIRenderer::UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, bool allowLogging)
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
	Printf("NRI PT surface probe: hit source=%s drawlist=%s sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s\n",
		GetSurfaceSourceTypeName(result.provenance.sourceType),
		GetDrawListTypeName(result.provenance.drawListType),
		result.provenance.sectorIndex,
		result.provenance.wallIndex,
		result.provenance.nextSectorIndex,
		result.provenance.actorIndex,
		result.provenance.cstat,
		result.primitiveIndex,
		result.materialIndex,
		result.distance,
		result.position[0], result.position[1], result.position[2],
		result.normal[0], result.normal[1], result.normal[2],
		flags,
		(flags & nri_scene::MaterialFlag_Indexed) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Fullbright) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Flat) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Sprite) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Mirror) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Sky) != 0 ? "yes" : "no",
		(flags & nri_scene::MaterialFlag_Portal) != 0 ? "yes" : "no");
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
	Printf("NRI PT surface probe: source=%s drawlist=%s sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x\n",
		GetSurfaceSourceTypeName(mLastSurfaceProbe.provenance.sourceType),
		GetDrawListTypeName(mLastSurfaceProbe.provenance.drawListType),
		mLastSurfaceProbe.provenance.sectorIndex,
		mLastSurfaceProbe.provenance.wallIndex,
		mLastSurfaceProbe.provenance.nextSectorIndex,
		mLastSurfaceProbe.provenance.actorIndex,
		mLastSurfaceProbe.provenance.cstat,
		mLastSurfaceProbe.primitiveIndex,
		mLastSurfaceProbe.materialIndex,
		mLastSurfaceProbe.distance,
		mLastSurfaceProbe.position[0],
		mLastSurfaceProbe.position[1],
		mLastSurfaceProbe.position[2],
		flags);
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
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		return;
	}

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
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mTaaOutputSet, 1, 0) == nri::Result::SUCCESS;
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
	if (sceneInstances.empty())
	{
		return false;
	}

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

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	const nri::Descriptor* descriptors[NRI_SCENE_DATA_DESCRIPTOR_NUM] = {
		selectView(staticVertexBuffer, dynamicVertexBuffer),
		selectView(staticIndexBuffer, dynamicIndexBuffer),
		selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer),
		selectView(staticMaterialBuffer, dynamicMaterialBuffer),
		selectView(dynamicVertexBuffer, staticVertexBuffer),
		selectView(dynamicIndexBuffer, staticIndexBuffer),
		selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer),
		selectView(dynamicMaterialBuffer, staticMaterialBuffer),
		mSceneInstanceBuffer.shaderView,
	};

	for (const nri::Descriptor* descriptor : descriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mSceneDataSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors;
	update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);

	mBoundStaticPrimitiveCount = staticPrimitiveCount;
	mBoundDynamicPrimitiveCount = dynamicPrimitiveCount;
	mBoundStaticMaterialCount = staticMaterialCount;
	mBoundDynamicMaterialCount = dynamicMaterialCount;
	return true;
}

bool NRIRenderer::UpdateFrameTextureSet()
{
	return UpdateFrameTextureSet(mFrameTextureSet, mFrameInputDescriptors);
}

bool NRIRenderer::UpdateFrameTextureSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 11>& descriptors)
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

bool NRIRenderer::UpdateOutputSet(nri::DescriptorSet* set, const std::array<nri::Descriptor*, 12>& descriptors)
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

bool NRIRenderer::EnsureFrameResources(uint32_t outputWidth, uint32_t outputHeight)
{
	Clocker clock(NriPTFrameResources);

	if (outputWidth == 0 || outputHeight == 0)
	{
		return false;
	}

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
	mOutputFormat = outputFormat;
	mResetHistory = true;

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
		CreateFrameTexture(FrameTextureSlot::DenoisedDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DenoisedSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Composed, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::ComposedSpecViewZ, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssDiffuseAlbedo, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssSpecularAlbedo, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssSpecularHitDistance, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DlssNormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::Upscaled, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::PreFinal, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Final, outputWidth, outputHeight, finalFormat);
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
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mOutputWidth), GetDispatchSize(mOutputHeight), 1 });
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
	mStaticMapScene.geometry = {};
	mStaticMapScene.materialBridge = {};
	mStaticMapScene.chunks.clear();
	mStaticMapScene.chunks.reserve(mMapWorld.chunks.size());

	for (const nri_scene::PTMapChunk& chunk : mMapWorld.chunks)
	{
		nri_scene::SceneView chunkSceneView;
		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		nri_scene::BuildMapChunkSceneView(mMapWorld, chunk, chunkSceneView);
		{
			Clocker clock(NriPTGeometryBuild);
			nri_scene::BuildGeometry(chunkSceneView, chunkGeometry);
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
		mDynamicBottomLevelAS.accelerationStructure != nullptr ||
		mTlasInstanceBuffer.buffer != nullptr ||
		mSceneInstanceBuffer.buffer != nullptr ||
		mScratchBuffer.buffer != nullptr;
	if (needsWait)
	{
		mFrameBuffer->WaitForCommands(true);
	}

	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyAccelerationStructureResource(mDynamicBottomLevelAS);
	DestroyAccelerationStructureResource(mTopLevelAS);

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

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = mScratchBuffer.buffer;
		build.scratchOffset = 0;
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);

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

void NRIRenderer::BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	outTlasInstances.clear();
	outSceneInstances.clear();
	outTlasInstances.reserve(mStaticMapScene.chunks.size());
	outSceneInstances.reserve(mStaticMapScene.chunks.size());

	for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		const auto& chunk = mStaticMapScene.chunks[chunkIndex];
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
	static bool sLoggedPhaseFDenoiserPath = false;
	static bool sLoggedPhaseFDenoiserFallback = false;
	static bool sLoggedRawTraceBypass = false;
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool bootstrapRawTracePresent = nri_ptbootstrap && (bootstrapMode == 11u || bootstrapMode == 12u);
	const bool useCompositionPresent = !nri_ptbootstrap && (nri_ptdebug == 0 || nri_ptdebug == 15);
	const bool useValidationPresent = !nri_ptbootstrap && nri_ptdebug == 9;
	const bool useFinalDebugPresent = !nri_ptbootstrap &&
		((nri_ptdebug >= 5 && nri_ptdebug <= 8) || nri_ptdebug == 13 || nri_ptdebug == 14);
	const bool rawTraceDirectPresent = !nri_ptbootstrap && !useCompositionPresent && !useValidationPresent && !useFinalDebugPresent;
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::Upscaled;
	mUseUpscaledInFinal = false;
	mUseDenoisedCompositionInputs = false;

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

	if (useCompositionPresent)
	{
		if (!sLoggedPhaseBCompositionPath)
		{
			Printf("NRI Phase B: ptdebug 0/15 now routes through Composition and the minimal FinalPresent presenter.\n");
			sLoggedPhaseBCompositionPath = true;
		}

		if (nri_denoise)
		{
			if (!sLoggedPhaseFDenoiserPath)
			{
				Printf("NRI Phase F: ptdebug 0/15 now routes through NRD before Composition when nri_denoise is enabled.\n");
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

		if (!DispatchFinalPresent(FrameTextureSlot::Composed))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useFinalDebugPresent)
	{
		if (!DispatchFinalPresent(FrameTextureSlot::UnfilteredDiffuse))
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

		if (!DispatchRawPresent(rawPresentSlot))
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

	NRITraceConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
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
	constants.Flags = (mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) | (directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u));
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
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
	desc.diffuse = &GetFrameTexture(FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &GetFrameTexture(FrameTextureSlot::DenoisedSpecular);
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
	desc.resetHistory = mResetHistory;
	desc.enableValidation = nri_validation;
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
	constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	Normalize3(constants.LightDirection);

	const FrameTextureSlot diffuseSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::UnfilteredDiffuse;
	const FrameTextureSlot specularSlot = mUseDenoisedCompositionInputs ? FrameTextureSlot::DenoisedSpecular : FrameTextureSlot::UnfilteredSpecular;
	NRITextureResource& diffuse = GetFrameTexture(diffuseSlot);
	NRITextureResource& specular = GetFrameTexture(specularSlot);
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);

	mFrameBuffer->TransitionTexture(diffuse, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(specular, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[5] = diffuse.shaderView;
	mFrameInputDescriptors[6] = specular.shaderView;
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

bool NRIRenderer::DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRITraceConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = (uint32_t)nri_ptdebug;

	NRITextureResource& input = GetFrameTexture(inputSlot);
	const bool addSecondary = secondarySlot != FrameTextureSlot::Count;
	NRITextureResource& secondary = GetFrameTexture(addSecondary ? secondarySlot : inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(secondary, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mTaaFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mTaaOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mTaaOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::RawPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mOutputWidth), GetDispatchSize(mOutputHeight), 1 });
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
	inputUpdate.descriptorSet = mTaaFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mTaaOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mTaaFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mTaaOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::FinalPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mOutputWidth), GetDispatchSize(mOutputHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchUpscaleChain()
{
	Clocker clock(NriPTUpscale);

	const bool temporalOnly = !nri_ptbootstrap;
	const NRIUpscalerKind kind = temporalOnly ? NRIUpscalerKind::Off : ResolveUpscalerKind(true);
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	NRITextureResource& preFinal = GetFrameTexture(FrameTextureSlot::PreFinal);

	if (kind == NRIUpscalerKind::Off || kind == NRIUpscalerKind::NIS)
	{
		if (temporalOnly)
		{
			CopyTexture(composed, historyOutput);
		}
		else
		{
			NRITraceConstants constants = {};
			constants.RenderWidth = mRenderWidth;
			constants.RenderHeight = mRenderHeight;
			constants.DisplayWidth = mOutputWidth;
			constants.DisplayHeight = mOutputHeight;
			constants.FrameIndex = mFrameIndex;
			constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;

			mFrameBuffer->TransitionTexture(composed, NRIComputeShaderResourceState());
			mFrameBuffer->TransitionTexture(historyInput, NRIComputeShaderResourceState());
			mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
			mFrameBuffer->TransitionTexture(historyOutput, NRIComputeStorageState());
			if (nri_ptdebug == 15)
			{
				mFrameBuffer->TransitionTexture(composed, NRIComputeStorageState());
			}

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

			const nri::Descriptor* taaOutputs[1] = { (nri_ptdebug == 15) ? composed.storageView : historyOutput.storageView };
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
	}

	if (kind == NRIUpscalerKind::Off)
	{
		if (temporalOnly)
		{
			mUseUpscaledInFinal = false;
			mUpscaledInputSlot = FrameTextureSlot::Composed;
			return true;
		}
		mUseUpscaledInFinal = false;
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
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = (uint32_t)nri_ptdebug;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
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
	mFrameInputDescriptors[10] = GetFrameTexture(FrameTextureSlot::DlssSpecularHitDistance).shaderView;
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
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mOutputWidth), GetDispatchSize(mOutputHeight), 1 });
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

	const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
	mCurrentTanHalfFovX = tanHalfFovX;
	mCurrentTanHalfFovY = tanHalfFovX * ((float)mRenderHeight / std::max(1.0f, (float)mRenderWidth));
	mCurrentJitter[0] = 0.0f;
	mCurrentJitter[1] = 0.0f;
	FillMatrix(mCurrentViewToClip, di.VPUniforms.mProjectionMatrix);
	FillMatrix(mCurrentWorldToView, di.VPUniforms.mViewMatrix);

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
	mOutputFormat = nri::Format::UNKNOWN;
}

void NRIRenderer::DestroySceneBuffers()
{
	mStaticMapScene.buffersResident = false;
	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	DestroyBufferResource(mTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mTopLevelScratchBuffer);
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
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
	mStaticAccelerationBuildSerial = 0;
	mActiveTlasInstanceCount = 0;
}

void NRIRenderer::DestroyStaticMapSceneCache()
{
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
