#include "nri_renderer.h"

#include "../framegen/nri_framegen.h"
#include "nri_renderstate.h"
#include "../scene/nri_map_builder.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "skyboxtexture.h"
#include "image.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "d_eventbase.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <chrono>
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
CVAR(Int, nri_postsharpen, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_upscalermode, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_pttaa, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_sharpness, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_validation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Int, nri_ptspherelongs)
EXTERN_CVAR(Int, nri_ptspherelats)

namespace
{
	static constexpr uint32_t NriPtDebugSphereLimit = 64u;

	static uint64_t HashCombineLightOverlay(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)x);
		key = HashCombineLightOverlay(key, (uint64_t)y);
		key = HashCombineLightOverlay(key, (uint64_t)z);
		return key;
	}

	static void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}

	static uint64_t HashLightOverlayText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	static uint32_t BuildActorOverlayRuleId(const ResolvedLightOverlayActorRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.actorClassName.GetChars(), rule.source);
	}

	static bool IsSupportedActorOverlayRule(const ResolvedLightOverlayActorRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	static bool IsSupportedMapOverlayRule(const ResolvedLightOverlayMapLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	static bool IsUsableDirectionalVector(const float direction[3])
	{
		if (!std::isfinite(direction[0]) || !std::isfinite(direction[1]) || !std::isfinite(direction[2]))
		{
			return false;
		}

		const float lengthSq = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
		return lengthSq > 0.000001f;
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		if (!std::isfinite(angularSize))
		{
			return 0.03f;
		}

		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint64_t QuantizeDirectionalLightScalar(float value, float scale)
	{
		return (uint64_t)(int64_t)std::llround((double)value * (double)scale);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static uint64_t BuildDirectionalLightStateHash(const NRIDirectionalLightState& state)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombineLightOverlay(hash, state.enabled ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, state.shadow ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, state.fromOverlay ? 1ull : 0ull);
		hash = HashCombineLightOverlay(hash, (uint64_t)state.ruleId);
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[0], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[1], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.direction[2], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[0], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[1], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.color[2], 4096.0f));
		hash = HashCombineLightOverlay(hash, QuantizeDirectionalLightScalar(state.angularSize, 4096.0f));
		return hash;
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

	static NRIDirectionalLightState BuildDirectionalLightState(const ResolvedLightOverlaySet& resolved, bool directionalLightEnabled)
	{
		NRIDirectionalLightState state = {};
		state.enabled = directionalLightEnabled;
		state.shadow = true;

		if (resolved.directionalRules.Size() > 0)
		{
			const ResolvedLightOverlayDirectionalRule& rule = resolved.directionalRules.Last();
			state.fromOverlay = true;
			state.ruleId = BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
			state.enabled = directionalLightEnabled;
			state.shadow = !rule.hasShadow || rule.shadow;

			if (rule.hasDirection && IsUsableDirectionalVector(rule.direction))
			{
				state.direction[0] = rule.direction[0];
				state.direction[1] = rule.direction[1];
				state.direction[2] = rule.direction[2];
				const float invLength = 1.0f / sqrtf(
					state.direction[0] * state.direction[0] +
					state.direction[1] * state.direction[1] +
					state.direction[2] * state.direction[2]);
				state.direction[0] *= invLength;
				state.direction[1] *= invLength;
				state.direction[2] *= invLength;
			}
			else
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasColor)
			{
				state.color[0] = std::max(rule.color[0], 0.0f);
				state.color[1] = std::max(rule.color[1], 0.0f);
				state.color[2] = std::max(rule.color[2], 0.0f);
			}

			const float intensity = rule.hasIntensity ? std::max(rule.intensity, 0.0f) : 1.0f;
			state.color[0] *= intensity;
			state.color[1] *= intensity;
			state.color[2] *= intensity;
			if (intensity <= 0.0f)
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasAngularSize)
			{
				state.angularSize = ClampDirectionalAngularSize(rule.angularSize);
			}
		}

		if (!state.enabled)
		{
			state.color[0] = 0.0f;
			state.color[1] = 0.0f;
			state.color[2] = 0.0f;
		}

		state.stateHash = BuildDirectionalLightStateHash(state);
		return state;
	}

	enum ActorShadowOverrideBits : uint32_t
	{
		ActorShadowOverride_None = 0,
		ActorShadowOverride_NoShadowReceive = 1u << 0,
		ActorShadowOverride_NoShadowCast = 1u << 1,
	};

	static void BuildActorShadowOverrideMap(const ResolvedLightOverlaySet& resolved, std::unordered_map<int32_t, uint32_t>& outOverrides)
	{
		if (resolved.actorRules.Size() == 0 && resolved.actorOverrideRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			uint32_t overrideBits = ActorShadowOverride_None;
			bool touched = false;
			const uint32_t actorTextureId = (unsigned)actor->spr.picnum < MAXTILES ? (uint32_t)tileGetTextureID(actor->spr.picnum).GetIndex() : 0u;
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				if (resolvedRule.hasTileFilter && actorTextureId != (uint32_t)resolvedRule.tileFilter)
				{
					continue;
				}

				if (resolvedRule.hasShadowReceive)
				{
					touched = true;
					if (resolvedRule.shadowReceive)
					{
						overrideBits &= ~ActorShadowOverride_NoShadowReceive;
					}
					else
					{
						overrideBits |= ActorShadowOverride_NoShadowReceive;
					}
				}

				if (resolvedRule.hasShadowCast)
				{
					touched = true;
					if (resolvedRule.shadowCast)
					{
						overrideBits &= ~ActorShadowOverride_NoShadowCast;
					}
					else
					{
						overrideBits |= ActorShadowOverride_NoShadowCast;
					}
				}
			}

			for (const auto& resolvedRule : resolved.actorOverrideRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				if (resolvedRule.hasShadowReceive)
				{
					touched = true;
					if (resolvedRule.shadowReceive)
					{
						overrideBits &= ~ActorShadowOverride_NoShadowReceive;
					}
					else
					{
						overrideBits |= ActorShadowOverride_NoShadowReceive;
					}
				}

				if (resolvedRule.hasShadowCast)
				{
					touched = true;
					if (resolvedRule.shadowCast)
					{
						overrideBits &= ~ActorShadowOverride_NoShadowCast;
					}
					else
					{
						overrideBits |= ActorShadowOverride_NoShadowCast;
					}
				}
			}

			if (touched && overrideBits != ActorShadowOverride_None)
			{
				outOverrides[(int32_t)actor->GetIndex()] = overrideBits;
			}
		}
	}

	static uint32_t BuildMapOverlayRuleId(const ResolvedLightOverlayMapLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)ruleId);
		key = HashCombineLightOverlay(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	static bool TryResolveSectorMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, float outPosition[3])
	{
		const nri_scene::PTMapChunk* matchedChunk = nullptr;
		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.sectorIndex == sectorIndex)
			{
				matchedChunk = &chunk;
				break;
			}
		}
		if (matchedChunk == nullptr)
		{
			return false;
		}

		float flatCenterSum[3] = {};
		int flatCenterCount = 0;
		float anyCenterSum[3] = {};
		int anyCenterCount = 0;
		const uint32_t endSurface = matchedChunk->firstSurface + matchedChunk->surfaceCount;
		for (uint32_t surfaceIndex = matchedChunk->firstSurface; surfaceIndex < endSurface && surfaceIndex < mapWorld.surfaces.size(); ++surfaceIndex)
		{
			const auto& surface = mapWorld.surfaces[surfaceIndex].surface;
			if (surface.provenance.sectorIndex != sectorIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(surface, center);
			anyCenterSum[0] += center[0];
			anyCenterSum[1] += center[1];
			anyCenterSum[2] += center[2];
			anyCenterCount++;

			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapFloorSection ||
				surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				flatCenterSum[0] += center[0];
				flatCenterSum[1] += center[1];
				flatCenterSum[2] += center[2];
				flatCenterCount++;
			}
		}

		const float* sum = flatCenterCount > 0 ? flatCenterSum : anyCenterSum;
		const int count = flatCenterCount > 0 ? flatCenterCount : anyCenterCount;
		if (count <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)count;
		outPosition[0] = sum[0] * invCount;
		outPosition[1] = sum[1] * invCount;
		outPosition[2] = sum[2] * invCount;
		return true;
	}

	static bool TryResolveWallMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t wallIndex, float outPosition[3])
	{
		float centerSum[3] = {};
		int centerCount = 0;
		for (const auto& mapSurface : mapWorld.surfaces)
		{
			if (mapSurface.surface.provenance.wallIndex != wallIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(mapSurface.surface, center);
			centerSum[0] += center[0];
			centerSum[1] += center[1];
			centerSum[2] += center[2];
			centerCount++;
		}

		if (centerCount <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)centerCount;
		outPosition[0] = centerSum[0] * invCount;
		outPosition[1] = centerSum[1] * invCount;
		outPosition[2] = centerSum[2] * invCount;
		return true;
	}

	static bool TryResolveMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, const ResolvedLightOverlayMapLightRule& rule, float outPosition[3])
	{
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			if (!rule.hasAnchorPosition)
			{
				return false;
			}
			outPosition[0] = rule.anchorPosition[0];
			outPosition[1] = rule.anchorPosition[1];
			outPosition[2] = rule.anchorPosition[2];
			return true;

		case LightOverlayAnchorType::Sector:
			return rule.anchorIndex >= 0 && TryResolveSectorMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		case LightOverlayAnchorType::Wall:
			return rule.anchorIndex >= 0 && TryResolveWallMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		default:
			return false;
		}
	}

	static void BuildActorAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>>& outRules)
	{
		if (resolved.actorRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr ||
				!actor->exists() ||
				(actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			auto& actorRules = outRules[(int32_t)actor->GetIndex()];
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					!IsSupportedActorOverlayRule(resolvedRule) ||
					resolvedRule.intensity <= 0.0f ||
					resolvedRule.radius <= 0.0f ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
				actorRule.ruleId = BuildActorOverlayRuleId(resolvedRule);
				actorRule.hasTileFilter = resolvedRule.hasTileFilter;
				actorRule.tileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0 ? (uint32_t)resolvedRule.tileFilter : 0u;
				actorRule.color[0] = resolvedRule.color[0];
				actorRule.color[1] = resolvedRule.color[1];
				actorRule.color[2] = resolvedRule.color[2];
				actorRule.intensity = resolvedRule.intensity;
				actorRule.radius = resolvedRule.radius;
				actorRule.offset[0] = resolvedRule.offset[0];
				actorRule.offset[1] = resolvedRule.offset[1];
				actorRule.offset[2] = resolvedRule.offset[2];
				actorRule.flickerFrames = resolvedRule.flickerFrames;
				actorRules.push_back(actorRule);
			}

			if (actorRules.empty())
			{
				outRules.erase((int32_t)actor->GetIndex());
			}
		}
	}

	static void BuildStaticMapAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld& mapWorld,
		std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule>& outRules)
	{
		for (const auto& resolvedRule : resolved.mapLightRules)
		{
			if (!IsSupportedMapOverlayRule(resolvedRule) ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			float anchorPosition[3] = {};
			if (!TryResolveMapOverlayAnchorPosition(mapWorld, resolvedRule, anchorPosition))
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::StaticMapScene;
			overlayRule.position[0] = anchorPosition[0] + resolvedRule.offset[0];
			overlayRule.position[1] = anchorPosition[1] + resolvedRule.offset[1];
			overlayRule.position[2] = anchorPosition[2] + resolvedRule.offset[2];
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.flickerFrames = resolvedRule.flickerFrames;
			outRules.push_back(overlayRule);
		}
	}

	static bool ResolveSurfaceProbeTextureDebugInfo(uint32_t textureId, FString& outTextureName, int32_t& outLegacyTile)
	{
		outTextureName = "(none)";
		outLegacyTile = -1;
		if (textureId == 0)
		{
			return false;
		}

		auto texture = TexMan.GameByIndex((int)textureId);
		if (texture == nullptr)
		{
			return false;
		}

		outTextureName = texture->GetName();
		if (textureId >= (uint32_t)firstarttile && textureId <= (uint32_t)(firstarttile + maxarttile))
		{
			outLegacyTile = legacyTileNum(FSetTextureID((int)textureId));
		}
		return true;
	}

	static void RefreshActiveFrameGenerationSwapChain()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->SetVSync(vid_vsync);
		}
	}

	static void NotifyActiveGlowControlChange()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->NotifyPathTracingGlowControlChange();
		}
	}

	static void NotifyActiveDebugSphereTessellationChange()
	{
		if (screen != nullptr && screen->Backend() == 4)
		{
			static_cast<NRIRenderDevice*>(screen)->NotifyPathTracingDebugSphereTessellationChange();
		}
	}

	static uint32_t GetRuntimeDebugSphereLongitudeSegments()
	{
		return (uint32_t)clamp<int>(nri_ptspherelongs, 8, 256);
	}

	static uint32_t GetRuntimeDebugSphereLatitudeSegments()
	{
		return (uint32_t)clamp<int>(nri_ptspherelats, 4, 128);
	}

	static uint32_t GetRuntimeDebugSphereTriangleCount()
	{
		return GetRuntimeDebugSphereLongitudeSegments() * 2u * (GetRuntimeDebugSphereLatitudeSegments() - 1u);
	}

	static void PathTracingToWorldPosition(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}
}

CUSTOM_CVAR(Bool, nri_framegen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RefreshActiveFrameGenerationSwapChain();
}

CUSTOM_CVAR(Int, nri_ptspherelongs, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 8)
	{
		self = 8;
		return;
	}
	else if (self > 256)
	{
		self = 256;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_ptspherelats, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 4)
	{
		self = 4;
		return;
	}
	else if (self > 128)
	{
		self = 128;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_framegenprovider, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}

	RefreshActiveFrameGenerationSwapChain();
}
CUSTOM_CVAR(Int, nri_framegenui, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 3)
	{
		self = 3;
	}
}
CVAR(Bool, nri_framegenasync, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, nri_framegenlatency, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RefreshActiveFrameGenerationSwapChain();
}
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
CUSTOM_CVAR(Float, nri_ptglowscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
CUSTOM_CVAR(Float, nri_ptglowreach, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}
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
CVAR(Bool, nri_ptvisiblechunkgate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_pttraceframes)

namespace
{
	constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 256;
	constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = 2 + NRI_MAX_SCENE_TEXTURES;
	constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 20;
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
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT = 34;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_SR_INPUT = 35;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_SR_DEPTH = 36;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_VENDOR_OUTPUT = 37;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_VENDOR_FINAL_PRESENT = 38;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_RR_INPUT = 39;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_RR_DIFFUSE_ALBEDO = 40;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_RR_SPECULAR_ALBEDO = 41;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_RR_NORMAL_ROUGHNESS = 42;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_RR_SPECULAR_HIT_DISTANCE = 43;
	constexpr uint32_t NRI_PTDEBUG_UPSCALER_POST_SHARPEN_OUTPUT = 44;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_STATIC = 0;
	constexpr uint32_t NRI_SCENE_DATA_SOURCE_DYNAMIC = 1;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_UNKNOWN = 0;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_STATIC_MAP = 1;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE = 2;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK = 3;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION = 4;
	constexpr uint32_t NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY = 5;
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
	constexpr uint32_t NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS = 0x200u;
	constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW = 0x400u;
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8;
	constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
	constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;
	constexpr uint32_t NRI_EMISSIVE_SAMPLING_FLAG_AUTO_ONLY = 0x1u;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;

	struct RendererSkyPerfTraceStats
	{
		uint32_t ensureSceneTexturesCalls = 0;
		uint32_t ensureSceneTexturesPreserveTrueCalls = 0;
		uint32_t ensureSceneTexturesPreserveFalseCalls = 0;
		uint32_t ensureSkyCalls = 0;
		uint32_t preserveExistingHits = 0;
		uint32_t reuseActiveCubemapHits = 0;
		uint32_t probeAttempts = 0;
		uint32_t probeSuccesses = 0;
		uint32_t reuseActiveProbeHits = 0;
		uint32_t activateCachedCubemapHits = 0;
		uint32_t createCachedCubemapHits = 0;
		uint32_t keepLastCubemapHits = 0;
		uint32_t holdLevelCubemapHits = 0;
		uint32_t solidReuseHits = 0;
		uint32_t solidActivateHits = 0;
		uint32_t solidCreateHits = 0;
		uint32_t probeFaceCalls = 0;
		uint32_t buildCubemapUploadCalls = 0;
		uint32_t residentStaticSceneTextureBuilds = 0;
		uint32_t combinedOverlayTextureBuilds = 0;
		uint32_t lightingInvalidationRequests = 0;
		uint32_t lightingInvalidationsApplied = 0;
		uint32_t emissiveMaterialDirtyEvents = 0;
		uint64_t ensureSkyTimeUs = 0;
		uint64_t probeCubemapTimeUs = 0;
		uint64_t probeFaceTimeUs = 0;
		uint64_t buildCubemapUploadTimeUs = 0;
	};

	RendererSkyPerfTraceStats gRendererSkyPerfTraceStats = {};

	bool ShouldTraceSkyPerf()
	{
		return nri_pttraceframes > 0;
	}

	bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || nri_pttraceframes > 0;
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldTracePtPerf() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	class ScopedSkyPerfTimer
	{
	public:
		explicit ScopedSkyPerfTimer(uint64_t& targetUs)
			: mTarget(ShouldTraceSkyPerf() ? &targetUs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSkyPerfTimer()
		{
			if (mTarget != nullptr)
			{
				const auto elapsed = std::chrono::steady_clock::now() - mStart;
				*mTarget += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			}
		}

		ScopedSkyPerfTimer(const ScopedSkyPerfTimer&) = delete;
		ScopedSkyPerfTimer& operator=(const ScopedSkyPerfTimer&) = delete;

	private:
		uint64_t* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	void ResetRendererSkyPerfTraceStats()
	{
		gRendererSkyPerfTraceStats = {};
	}

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

	const char* GetSurfaceProbeSceneOwnerName(uint32_t owner)
	{
		switch (owner)
		{
		case NRI_SURFACE_PROBE_OWNER_STATIC_MAP: return "static_map";
		case NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE: return "captured_scene";
		case NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK: return "runtime_link_overlay";
		case NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION: return "runtime_mutation_overlay";
		case NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY: return "dynamic_overlay";
		default: return "unknown";
		}
	}

	uint32_t CountSurfaceTriangles(const nri_scene::SurfaceRef& surface)
	{
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2 : 0u;
	}

	struct ChunkCompareSurfaceKey
	{
		uint32_t kind = UINT32_MAX;
		uint32_t sourceType = (uint32_t)nri_scene::SurfaceSourceType::Unknown;
		int32_t sectorIndex = -1;
		int32_t wallIndex = -1;
		int32_t sectionIndex = -1;
		int32_t nextSectorIndex = -1;
		int32_t actorIndex = -1;
		uint32_t cstat = 0;
		uint32_t materialFlags = 0;
		uint32_t primaryKey = UINT32_MAX;
		uint32_t secondaryKey = UINT32_MAX;

		bool operator==(const ChunkCompareSurfaceKey& other) const
		{
			return kind == other.kind &&
				sourceType == other.sourceType &&
				sectorIndex == other.sectorIndex &&
				wallIndex == other.wallIndex &&
				sectionIndex == other.sectionIndex &&
				nextSectorIndex == other.nextSectorIndex &&
				actorIndex == other.actorIndex &&
				cstat == other.cstat &&
				materialFlags == other.materialFlags &&
				primaryKey == other.primaryKey &&
				secondaryKey == other.secondaryKey;
		}
	};

	struct ChunkCompareSurfaceKeyHash
	{
		size_t operator()(const ChunkCompareSurfaceKey& key) const
		{
			size_t h = 1469598103934665603ull;
			const auto mix = [&h](uint64_t value)
			{
				h ^= (size_t)value;
				h *= 1099511628211ull;
			};
			mix(key.kind);
			mix(key.sourceType);
			mix((uint32_t)key.sectorIndex);
			mix((uint32_t)key.wallIndex);
			mix((uint32_t)key.sectionIndex);
			mix((uint32_t)key.nextSectorIndex);
			mix((uint32_t)key.actorIndex);
			mix(key.cstat);
			mix(key.materialFlags);
			mix(key.primaryKey);
			mix(key.secondaryKey);
			return h;
		}
	};

	struct ChunkCompareSurfaceMetrics
	{
		float centroid[3] = {};
		float normal[3] = {};
		float area = 0.0f;
		float aabbMin[3] = {};
		float aabbMax[3] = {};
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint32_t textureId = 0;
		int palette = 0;
		int shade = 0;
		float alpha = 1.0f;
		uint32_t materialFlags = 0;
	};

	struct ChunkCompareMatchRecord
	{
		uint32_t staticSurfaceIndex = UINT32_MAX;
		uint32_t liveSurfaceIndex = UINT32_MAX;
		ChunkCompareSurfaceKey key = {};
		ChunkCompareSurfaceMetrics staticMetrics = {};
		ChunkCompareSurfaceMetrics liveMetrics = {};
		float delta[3] = {};
		float deltaDistance = 0.0f;
		float areaRatio = 1.0f;
		float normalDot = 1.0f;
		float materialScore = 0.0f;
		float deviationFromMean = 0.0f;
		float score = 0.0f;
	};

	static ChunkCompareSurfaceKey BuildChunkCompareSurfaceKey(const nri_scene::PTMapSurface& surface)
	{
		ChunkCompareSurfaceKey key = {};
		key.kind = (uint32_t)surface.kind;
		key.sourceType = (uint32_t)surface.surface.provenance.sourceType;
		key.sectorIndex = surface.surface.provenance.sectorIndex;
		key.wallIndex = surface.surface.provenance.wallIndex;
		key.sectionIndex = surface.surface.provenance.sectionIndex;
		key.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		key.actorIndex = surface.surface.provenance.actorIndex;
		key.cstat = surface.surface.provenance.cstat;
		key.materialFlags = surface.surface.provenance.materialFlags;
		key.primaryKey = surface.key.primary;
		key.secondaryKey = surface.key.secondary;
		return key;
	}

	static uint32_t GetSurfaceTextureId(const nri_scene::PTMapSurface& surface)
	{
		return
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
	}

	static float Distance3(const float a[3], const float b[3])
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	static float Dot3(const float a[3], const float b[3])
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	static float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
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

	static void ComputeTriangleNormal(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c, float outNormal[3])
	{
		outNormal[0] = 0.0f;
		outNormal[1] = 0.0f;
		outNormal[2] = 0.0f;
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		const float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
		if (length <= 0.0001f)
		{
			return;
		}

		outNormal[0] = crossX / length;
		outNormal[1] = crossY / length;
		outNormal[2] = crossZ / length;
	}

	static ChunkCompareSurfaceMetrics ComputeChunkCompareSurfaceMetrics(const nri_scene::PTMapSurface& surface)
	{
		ChunkCompareSurfaceMetrics metrics = {};
		const auto& vertices = surface.surface.vertices;
		metrics.vertexCount = (uint32_t)vertices.size();
		metrics.triangleCount = CountSurfaceTriangles(surface.surface);
		metrics.textureId = GetSurfaceTextureId(surface);
		metrics.palette = surface.surface.material.palette;
		metrics.shade = surface.surface.material.shade;
		metrics.alpha = surface.surface.material.alpha;
		metrics.materialFlags = surface.surface.material.flags;
		if (vertices.empty())
		{
			return metrics;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.aabbMin[axis] = vertices[0].position[axis];
			metrics.aabbMax[axis] = vertices[0].position[axis];
		}

		for (const auto& vertex : vertices)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				metrics.centroid[axis] += vertex.position[axis];
				metrics.aabbMin[axis] = std::min(metrics.aabbMin[axis], vertex.position[axis]);
				metrics.aabbMax[axis] = std::max(metrics.aabbMax[axis], vertex.position[axis]);
			}
		}

		const float invCount = 1.0f / (float)vertices.size();
		for (int axis = 0; axis < 3; ++axis)
		{
			metrics.centroid[axis] *= invCount;
		}

		if (vertices.size() >= 3)
		{
			if ((surface.surface.material.flags & nri_scene::MaterialFlag_Flat) != 0 &&
				(vertices.size() % 3u) == 0u)
			{
				for (size_t i = 0; i + 2 < vertices.size(); i += 3)
				{
					metrics.area += ComputeTriangleArea(vertices[i], vertices[i + 1], vertices[i + 2]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(vertices[i], vertices[i + 1], vertices[i + 2], metrics.normal);
					}
				}
			}
			else
			{
				const auto& root = vertices[0];
				for (size_t i = 1; i + 1 < vertices.size(); ++i)
				{
					metrics.area += ComputeTriangleArea(root, vertices[i], vertices[i + 1]);
					if (metrics.normal[0] == 0.0f && metrics.normal[1] == 0.0f && metrics.normal[2] == 0.0f)
					{
						ComputeTriangleNormal(root, vertices[i], vertices[i + 1], metrics.normal);
					}
				}
			}
		}

		return metrics;
	}

	const char* GetMapSurfaceKindName(nri_scene::PTMapSurfaceKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor: return "floor";
		case nri_scene::PTMapSurfaceKind::Ceiling: return "ceiling";
		case nri_scene::PTMapSurfaceKind::WallOneSided: return "wall_one_sided";
		case nri_scene::PTMapSurfaceKind::WallUpper: return "wall_upper";
		case nri_scene::PTMapSurfaceKind::WallMiddle: return "wall_middle";
		case nri_scene::PTMapSurfaceKind::WallLower: return "wall_lower";
		case nri_scene::PTMapSurfaceKind::Portal: return "portal";
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

	static uint32_t GetEffectivePtDebugMode()
	{
		return (nri_ptdebug >= 0 && nri_ptdebug <= (int)NRI_PTDEBUG_UPSCALER_POST_SHARPEN_OUTPUT) ? (uint32_t)nri_ptdebug : 0u;
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

	static int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex)
	{
		if (!mapWorld.valid || sectorIndex < 0)
		{
			return -1;
		}

		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.kind == nri_scene::PTMapChunkKind::Sector && chunk.sectorIndex == sectorIndex)
			{
				return (int32_t)chunk.chunkIndex;
			}
		}

		return -1;
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

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackUInt16Pair(uint32_t lo, uint32_t hi)
	{
		return (lo & 0xffffu) | ((hi & 0xffffu) << 16u);
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

	static void AppendGeometryChunk(
		const nri_scene::GeometryData& source,
		uint32_t sourceVertexOffset,
		uint32_t sourceVertexCount,
		uint32_t sourceIndexOffset,
		uint32_t sourceIndexCount,
		uint32_t sourcePrimitiveOffset,
		uint32_t sourcePrimitiveCount,
		nri_scene::GeometryData& destination)
	{
		if (sourceVertexOffset >= source.vertices.size() ||
			sourcePrimitiveOffset >= source.primitives.size() ||
			sourceVertexCount == 0 ||
			sourcePrimitiveCount == 0)
		{
			return;
		}

		sourceVertexCount = std::min(sourceVertexCount, (uint32_t)source.vertices.size() - sourceVertexOffset);
		if (sourceIndexOffset >= source.indices.size())
		{
			sourceIndexCount = 0;
		}
		else
		{
			sourceIndexCount = std::min(sourceIndexCount, (uint32_t)source.indices.size() - sourceIndexOffset);
		}
		sourcePrimitiveCount = std::min(sourcePrimitiveCount, (uint32_t)source.primitives.size() - sourcePrimitiveOffset);
		const uint32_t sourcePrimitiveProvenanceCount =
			sourcePrimitiveOffset < source.primitiveProvenance.size() ?
			std::min(sourcePrimitiveCount, (uint32_t)source.primitiveProvenance.size() - sourcePrimitiveOffset) :
			0u;

		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(
			destination.vertices.end(),
			source.vertices.begin() + sourceVertexOffset,
			source.vertices.begin() + sourceVertexOffset + sourceVertexCount);

		if (sourceIndexCount > 0)
		{
			destination.indices.reserve(destination.indices.size() + sourceIndexCount);
			for (uint32_t i = 0; i < sourceIndexCount; ++i)
			{
				destination.indices.push_back(vertexBase + source.indices[sourceIndexOffset + i] - sourceVertexOffset);
			}
		}

		destination.primitives.reserve(destination.primitives.size() + sourcePrimitiveCount);
		for (uint32_t i = 0; i < sourcePrimitiveCount; ++i)
		{
			nri_scene::PrimitiveData copy = source.primitives[sourcePrimitiveOffset + i];
			copy.indices[0] = vertexBase + copy.indices[0] - sourceVertexOffset;
			copy.indices[1] = vertexBase + copy.indices[1] - sourceVertexOffset;
			copy.indices[2] = vertexBase + copy.indices[2] - sourceVertexOffset;
			destination.primitives.push_back(copy);
		}

		if (sourcePrimitiveProvenanceCount > 0)
		{
			destination.primitiveProvenance.insert(
				destination.primitiveProvenance.end(),
				source.primitiveProvenance.begin() + sourcePrimitiveOffset,
				source.primitiveProvenance.begin() + sourcePrimitiveOffset + sourcePrimitiveProvenanceCount);
		}
	}

	static void AppendMaterialBridge(const nri_scene::MaterialBridgeData& source, nri_scene::MaterialBridgeData& destination)
	{
		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(destination.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)destination.textures.size(); ++i)
		{
			textureLookup.emplace(destination.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &destination, &textureLookup](uint32_t textureIndex) -> uint32_t
		{
			if (textureIndex == UINT32_MAX)
			{
				return UINT32_MAX;
			}
			if (textureIndex >= source.textures.size())
			{
				return textureIndex;
			}

			const auto& texture = source.textures[textureIndex];
			auto it = textureLookup.find(texture.key);
			if (it == textureLookup.end())
			{
				const uint32_t newIndex = (uint32_t)destination.textures.size();
				textureLookup.emplace(texture.key, newIndex);
				destination.textures.push_back(texture);
				return newIndex;
			}

			return it->second;
		};

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			const bool hasLightMetadata = materialIndex < source.lightMetadata.size();
			copy.textureIndex = remapTextureIndex(material.textureIndex);
			copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
			copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
			copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
			copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);

			destination.materials.push_back(copy);
			if (hasLightMetadata)
			{
				nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
				metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
				metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
				metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
				metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
				metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
				metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
				destination.lightMetadata.push_back(metadata);
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

	static uint32_t GetUpscalerJitterPhaseCount(nri::UpscalerMode mode)
	{
		switch (mode)
		{
		case nri::UpscalerMode::NATIVE: return 8u;
		case nri::UpscalerMode::ULTRA_QUALITY: return 14u;
		case nri::UpscalerMode::QUALITY: return 18u;
		case nri::UpscalerMode::BALANCED: return 23u;
		case nri::UpscalerMode::PERFORMANCE: return 32u;
		case nri::UpscalerMode::ULTRA_PERFORMANCE: return 72u;
		default: return 8u;
		}
	}

	static nri::UpscalerMode ResolveUpscalerModeForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLRR:
			return requestedMode;
		case NRIMainUpscalerKind::DLSR:
			return requestedMode;
		default:
			return requestedMode;
		}
	}

	static float ResolveRenderScaleForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode, float manualRenderScale)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR:
			return GetUpscalerRenderScale(requestedMode);
		case NRIMainUpscalerKind::DLRR:
			return GetUpscalerRenderScale(requestedMode);
		default:
			return manualRenderScale;
		}
	}

	static const char* GetRenderResolutionPolicyName(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "sr-mode-scale";
		case NRIMainUpscalerKind::DLRR: return "rr-mode-scale";
		default: return "manual-scale";
		}
	}

	static void SyncLegacyUpscalerConfig(bool logMigration)
	{
		if ((int)nri_upscaler == 1)
		{
			nri_upscaler = 0;
			if ((int)nri_postsharpen == 0)
			{
				nri_postsharpen = 1;
			}

			static bool loggedLegacyNisMigration = false;
			if (logMigration && !loggedLegacyNisMigration)
			{
				Printf("NRI upscaler config: migrated legacy nri_upscaler=1 (NIS) to nri_upscaler=0 + nri_postsharpen=1\n");
				loggedLegacyNisMigration = true;
			}
		}

		const int clampedMainUpscaler =
			(int)nri_upscaler == 0 || (int)nri_upscaler == 2 || (int)nri_upscaler == 3
			? (int)nri_upscaler
			: 0;
		if ((int)nri_upscaler != clampedMainUpscaler)
		{
			const int invalidValue = (int)nri_upscaler;
			nri_upscaler = clampedMainUpscaler;

			static int lastLoggedInvalidMainUpscaler = std::numeric_limits<int>::min();
			if (logMigration && lastLoggedInvalidMainUpscaler != invalidValue)
			{
				Printf("NRI upscaler config: invalid main upscaler value %d, forcing off\n", invalidValue);
				lastLoggedInvalidMainUpscaler = invalidValue;
			}
		}

		const int clampedPostSharpen = (int)nri_postsharpen == 1 ? 1 : 0;
		if ((int)nri_postsharpen != clampedPostSharpen)
		{
			const int invalidValue = (int)nri_postsharpen;
			nri_postsharpen = clampedPostSharpen;

			static int lastLoggedInvalidPostSharpen = std::numeric_limits<int>::min();
			if (logMigration && lastLoggedInvalidPostSharpen != invalidValue)
			{
				Printf("NRI upscaler config: invalid post sharpen value %d, forcing off\n", invalidValue);
				lastLoggedInvalidPostSharpen = invalidValue;
			}
		}
	}

	static const char* GetMainUpscalerName(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "DLSS-SR";
		case NRIMainUpscalerKind::DLRR: return "DLRR";
		default: return "off";
		}
	}

	static const char* GetPostSharpenName(NRIPostSharpenKind kind)
	{
		switch (kind)
		{
		case NRIPostSharpenKind::NIS: return "NIS";
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

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "vendor-sr";
		case NRIMainUpscalerKind::DLRR: return "vendor-rr";
		default: return runAppTaa ? "native-taa" : "native";
		}
	}

	static nri::UpscalerType ToMainUpscalerType(NRIMainUpscalerKind kind)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return nri::UpscalerType::DLSR;
		case NRIMainUpscalerKind::DLRR: return nri::UpscalerType::DLRR;
		default: return nri::UpscalerType::NIS;
		}
	}

	static nri::UpscalerType ToPostSharpenType(NRIPostSharpenKind kind)
	{
		switch (kind)
		{
		case NRIPostSharpenKind::NIS: return nri::UpscalerType::NIS;
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

	static bool IsAppTaaEligibleUpscaler(NRIMainUpscalerKind kind)
	{
		return kind == NRIMainUpscalerKind::Off;
	}

	static bool ShouldRunAppTaa(NRIMainUpscalerKind kind)
	{
		return IsAppTaaEligibleUpscaler(kind) && !!nri_pttaa;
	}

	static bool ShouldUseTemporalJitter(NRIMainUpscalerKind kind)
	{
		return ShouldRunAppTaa(kind) || kind == NRIMainUpscalerKind::DLSR || kind == NRIMainUpscalerKind::DLRR;
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

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeFaceCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeFaceTimeUs);
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		const int width = baseTexture->GetWidth();
		const int height = baseTexture->GetHeight();
		if (width <= 0 || height <= 0)
		{
			return false;
		}

		FContentIdBuilder contentId = {};
		contentId.imageID = baseTexture->GetImage()->GetId();
		contentId.translation = 0;
		contentId.expand = 0;
		contentId.scaler = 0;
		contentId.scalefactor = 0;

		outFace.texture = texture;
		outFace.width = (uint32_t)width;
		outFace.height = (uint32_t)height;
		outFace.contentId = contentId.id != 0 ? contentId.id : (uint64_t)(uintptr_t)texture;
		return true;
	}

	static bool ProbeCubemapSky(const nri_scene::SceneView& sceneView, SkyProbe& outProbe)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeAttempts++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeCubemapTimeUs);
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeSuccesses++;
		}
		return true;
	}

	static bool BuildCubemapUpload(const nri_scene::SceneView& sceneView, const SkyProbe& probe, SkyUpload& outUpload)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.buildCubemapUploadCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs);
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
		case nri_scene::SurfaceSourceType::DebugSphere: return "debug_sphere";
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
	mFrameGenerationFrameId = 0;
	mHasFrameGenerationRealFrameTime = false;
	mHasPendingFrameGenerationRealFrameTime = false;
	mHasFrameGenerationTimestamp = false;
	mHasFrameGenerationConfigState = false;
	mLastFrameGenerationRealFrameTimeMs = 0.0f;
	mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	mLastFrameGenerationTimestamp = {};
	mPendingFrameGenerationTimestamp = {};
	mLastFrameGenerationRequestedEnabled = false;
	mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;

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
	mUpscalerPrepassFrameTextureSet = nullptr;
	mUpscalerPrepassOutputSet = nullptr;
	mTaaFrameTextureSet = nullptr;
	mTaaOutputSet = nullptr;
	mRawPresentFrameTextureSet = nullptr;
	mRawPresentOutputSet = nullptr;
	mFinalPresentFrameTextureSet = nullptr;
	mFinalPresentOutputSet = nullptr;
}

void NRIRenderer::ResetPerfTraceStats()
{
	mLastPerfShellTraceStats = {};
	mLastPerfResourceTraceStats = {};
}

void NRIRenderer::WaitForCommandsTracked()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool trace = ShouldTracePtPerf();
	const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	mFrameBuffer->WaitForCommands(true);
	if (trace)
	{
		mLastPerfResourceTraceStats.waitCalls++;
		mLastPerfResourceTraceStats.waitMs += DurationMs(start, std::chrono::steady_clock::now());
	}
}

void NRIRenderer::NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth)
{
	if (!ShouldTracePtPerf() || stats == nullptr)
	{
		return;
	}

	auto& perf = mLastPerfResourceTraceStats;
	if (growth)
	{
		perf.growEvents++;
	}
	else
	{
		perf.overwriteEvents++;
	}

	auto noteBytes = [&](uint32_t& callCount, uint64_t& byteCount)
	{
		callCount++;
		byteCount += size;
	};

	if (stats == &mVertexBufferStats || stats == &mIndexBufferStats || stats == &mPrimitiveBufferStats || stats == &mMaterialBufferStats)
	{
		noteBytes(perf.sceneUploadCalls, perf.sceneUploadBytes);
	}
	else if (stats == &mEmissivePrimitiveHeaderBufferStats || stats == &mEmissivePrimitiveBufferStats || stats == &mEmissivePrimitiveCdfBufferStats || stats == &mEmissiveTlasInstanceBufferStats)
	{
		noteBytes(perf.emissiveUploadCalls, perf.emissiveUploadBytes);
	}
	else
	{
		noteBytes(perf.sceneDataUploadCalls, perf.sceneDataUploadBytes);
	}
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

	ResetPerfTraceStats();
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.totalMs);
	Clocker totalClock(NriPTAll);
	const uint32_t traceFrameIndex = mFrameIndex;

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

	bool ready = false;
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		ready =
			Initialize() &&
			EnsureFrameResources(
				std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u),
				std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u),
				mFrameBuffer->mActiveTarget->width,
				mFrameBuffer->mActiveTarget->height);
	}
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
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
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
		mPendingFrameGenerationTimestamp = std::chrono::steady_clock::now();
		mHasPendingFrameGenerationRealFrameTime = false;
		mPendingFrameGenerationRealFrameTimeMs = 0.0f;
		if (mHasFrameGenerationTimestamp)
		{
			const auto elapsed = mPendingFrameGenerationTimestamp - mLastFrameGenerationTimestamp;
			mPendingFrameGenerationRealFrameTimeMs = (float)std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();
			mHasPendingFrameGenerationRealFrameTime = true;
			if (mPendingFrameGenerationRealFrameTimeMs > 250.0f)
			{
				RequestHistoryReset("cadence-break");
			}
		}
	}
	UpdateFrameGenerationHistoryPolicy(debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), preserveHistory);

	RefreshMapWorld();
	if (mPendingStaticMapLightingInvalidation)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied++;
		}
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
			NoteSuccessfulRealFrame();
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
	nri_scene::GeometryData debugSphereGeometry;
	nri_scene::GeometryData overlayGeometry;
	nri_scene::GeometryData combinedGeometry;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData runtimeMutationMaterialBridge;
	nri_scene::MaterialBridgeData runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData debugSphereMaterialBridge;
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
	uint32_t activeStaticProbePrimitiveCount = 0;
	EmissiveSamplingBuildContext emissiveSamplingContext = {};
	bool sceneLightUsesStaticMapScene = false;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;
	bool usingPersistentDynamicEmissiveCache = false;
	bool liveDynamicHasEmissive = false;

	{
		ScopedPtPerfTimer sceneSelectTimer(mLastPerfShellTraceStats.sceneSelectMs);
		if (allowStaticMapScene && EnsureStaticMapScene())
		{
			sceneLightUsesStaticMapScene = true;
			emissiveSamplingContext.staticGeometry = &mStaticMapScene.geometry;
			mUsedStaticMapSceneLastFrame = true;
			activeSceneView = &mStaticMapScene.sceneView;
			activeGeometry = &mStaticMapScene.geometry;
			activeGpuMaterials = &mStaticMapScene.gpuMaterials;
			activeMaterialBridge = &mStaticMapScene.materialBridge;
			activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
			activeStats = mStaticMapScene.sceneView.stats;

		const bool deferOverlayThisFrame = mUploadedStaticMapSceneLastFrame || mBuiltStaticMapSceneASLastFrame;
		const bool hasRuntimeSpaceLinkOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeSpaceLinkMs);
			return BuildRuntimeSpaceLinkOverlay(di, runtimeSpaceLinkGeometry, runtimeSpaceLinkMaterialBridge);
		}();
		mLastPerfShellTraceStats.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
		mLastPerfShellTraceStats.runtimeSpaceLinkMaterialCount = (uint32_t)runtimeSpaceLinkMaterialBridge.materials.size();
		const bool hasRuntimeMutationOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationMs);
			return BuildRuntimeMapMutationOverlay(runtimeMutationGeometry, runtimeMutationMaterialBridge);
		}();
		const bool hasDynamicScene = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicCaptureMs);
			return nri_scene::CaptureDynamicScene(di, dynamicSceneView);
		}();
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
			liveDynamicHasEmissive = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentDynamicMs);
				return RebuildPersistentDynamicEmissiveCache(dynamicSceneView, dynamicMaterialBridge);
			}();
		}

		PrunePersistentDynamicEmissiveCacheToLiveActors();

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
		const bool hasRuntimeDebugSphereOverlay = !deferOverlayThisFrame && [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMs);
			return BuildRuntimeDebugSphereOverlay(debugSphereGeometry, debugSphereMaterialBridge);
		}();

		if (hasRuntimeSpaceLinkOverlay || hasRuntimeMutationOverlay || hasActiveDynamicOverlay || hasRuntimeDebugSphereOverlay)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.overlayAssembleMs);
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

			if (hasRuntimeDebugSphereOverlay)
			{
				AppendGeometry(debugSphereGeometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
				AppendMaterialBridge(debugSphereMaterialBridge, overlayMaterialBridge);
			}

			mLastPerfShellTraceStats.overlayPrimitiveCount = (uint32_t)overlayGeometry.primitives.size();
			mLastPerfShellTraceStats.overlayMaterialCount = (uint32_t)overlayMaterialBridge.materials.size();

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
				if (ShouldTraceSkyPerf())
				{
					gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds++;
				}
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
					const bool useFilteredStaticProbeGeometry =
						hasRuntimeMutationOverlay &&
						!mRuntimeMapMutations.replacedChunkMask.empty();
					nri_scene::GeometryData filteredStaticGeometry;
					if (useFilteredStaticProbeGeometry)
					{
						BuildFilteredStaticMapGeometry(mRuntimeMapMutations.replacedChunkMask, filteredStaticGeometry);
						activeStaticProbePrimitiveCount = (uint32_t)filteredStaticGeometry.primitives.size();
						combinedGeometry = std::move(filteredStaticGeometry);
					}
					else
					{
						combinedGeometry = mStaticMapScene.geometry;
						activeStaticProbePrimitiveCount = (uint32_t)mStaticMapScene.geometry.primitives.size();
					}
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
				material.normalTextureIndex = UINT32_MAX;
				material.metallicTextureIndex = UINT32_MAX;
				material.roughnessTextureIndex = UINT32_MAX;
				material.emissiveTextureIndex = UINT32_MAX;
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
	mSurfaceProbeFrame = {};
	mSurfaceProbeFrame.valid = true;
	mSurfaceProbeFrame.usesStaticMapScene = mUsedStaticMapSceneLastFrame;
	mSurfaceProbeFrame.staticTlasExcludesReplacedChunks = !runtimeMutationGeometry.primitives.empty();
	mSurfaceProbeFrame.staticProbeExcludesReplacedChunks =
		mUsedStaticMapSceneLastFrame &&
		activeGeometry != nullptr &&
		activeGeometry != &mStaticMapScene.geometry &&
		!runtimeMutationGeometry.primitives.empty();
	mSurfaceProbeFrame.staticPrimitiveCount = mUsedStaticMapSceneLastFrame ? activeStaticProbePrimitiveCount : 0u;
	mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount = (uint32_t)runtimeSpaceLinkGeometry.primitives.size();
	mSurfaceProbeFrame.runtimeMutationPrimitiveCount = (uint32_t)runtimeMutationGeometry.primitives.size();
	mSurfaceProbeFrame.dynamicPrimitiveCount = activeDynamicGeometry != nullptr ? (uint32_t)activeDynamicGeometry->primitives.size() : 0u;

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
			NoteSuccessfulRealFrame();
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

	if (success)
	{
		mLastPerfShellTraceStats.activePrimitiveCount = (uint32_t)activeGeometry->primitives.size();
		mLastPerfShellTraceStats.dynamicPrimitiveCount = activeDynamicGeometry != nullptr ? (uint32_t)activeDynamicGeometry->primitives.size() : 0u;
		mLastPerfShellTraceStats.activeMaterialCount = (uint32_t)activeGpuMaterials->size();
		mLastPerfShellTraceStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
		mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
		mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
		mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = usingPersistentDynamicEmissiveCache;
		const double accountedMs =
			mLastPerfShellTraceStats.initResourcesMs +
			mLastPerfShellTraceStats.mapWorldMs +
			mLastPerfShellTraceStats.updateStateMs +
			mLastPerfShellTraceStats.sceneSelectMs +
			mLastPerfShellTraceStats.sceneLightsMs +
			mLastPerfShellTraceStats.residentLightRefreshMs +
			mLastPerfShellTraceStats.emissiveUpdateMs +
			mLastPerfShellTraceStats.emissiveTlasMs +
			mLastPerfShellTraceStats.surfaceProbeMs +
			mLastPerfShellTraceStats.frameGraphMs;
		mLastPerfShellTraceStats.otherMs = std::max(0.0, mLastPerfShellTraceStats.totalMs - accountedMs);
	}

	if (nri_pttraceframes > 0)
	{
		const nri_scene::SkyPerfStats sceneSkyPerf = nri_scene::ConsumeSkyPerfStats();
		Printf("NRI PT sky perf: frame=%u ensure_scene=%u preserve_scene=%u rebuild_scene=%u ensure_sky=%u preserve_hit=%u reuse_active=%u reuse_probe=%u probe=%u/%u face_probes=%u uploads=%u ensure_ms=%.3f probe_ms=%.3f face_ms=%.3f upload_ms=%.3f static_builds=%u overlay_builds=%u\n",
			traceFrameIndex,
			gRendererSkyPerfTraceStats.ensureSceneTexturesCalls,
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls,
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls,
			gRendererSkyPerfTraceStats.ensureSkyCalls,
			gRendererSkyPerfTraceStats.preserveExistingHits,
			gRendererSkyPerfTraceStats.reuseActiveCubemapHits + gRendererSkyPerfTraceStats.solidReuseHits,
			gRendererSkyPerfTraceStats.reuseActiveProbeHits,
			gRendererSkyPerfTraceStats.probeSuccesses,
			gRendererSkyPerfTraceStats.probeAttempts,
			gRendererSkyPerfTraceStats.probeFaceCalls,
			gRendererSkyPerfTraceStats.buildCubemapUploadCalls,
			(double)gRendererSkyPerfTraceStats.ensureSkyTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.probeCubemapTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.probeFaceTimeUs / 1000.0,
			(double)gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs / 1000.0,
			gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds,
			gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds);
		Printf("NRI PT sky scene: frame=%u updates=%u wall=%u flat=%u portal=%u inspects=%u cubemap_candidates=%u solid_candidates=%u inspect_faces=%u avg_base=%u avg_recursive=%u recursive_faces=%u avg_pixels=%llu update_ms=%.3f inspect_ms=%.3f avg_ms=%.3f\n",
			traceFrameIndex,
			sceneSkyPerf.updateCalls,
			sceneSkyPerf.wallUpdateCalls,
			sceneSkyPerf.flatUpdateCalls,
			sceneSkyPerf.portalUpdateCalls,
			sceneSkyPerf.inspectCalls,
			sceneSkyPerf.inspectCubemapCandidates,
			sceneSkyPerf.inspectSolidCandidates,
			sceneSkyPerf.inspectFaceWalks,
			sceneSkyPerf.averageColorBaseCalls,
			sceneSkyPerf.averageColorRecursiveCalls,
			sceneSkyPerf.recursiveSkyboxFaceSamples,
			(unsigned long long)sceneSkyPerf.averageColorPixels,
			(double)sceneSkyPerf.updateTimeUs / 1000.0,
			(double)sceneSkyPerf.inspectTimeUs / 1000.0,
			(double)sceneSkyPerf.averageColorTimeUs / 1000.0);
		Printf("NRI PT sky invalidation: frame=%u requests=%u applied=%u emissive_material_dirty=%u keep_last=%u hold_level=%u cached_cubemap=%u create_cubemap=%u cached_solid=%u create_solid=%u\n",
			traceFrameIndex,
			gRendererSkyPerfTraceStats.lightingInvalidationRequests,
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied,
			gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents,
			gRendererSkyPerfTraceStats.keepLastCubemapHits,
			gRendererSkyPerfTraceStats.holdLevelCubemapHits,
			gRendererSkyPerfTraceStats.activateCachedCubemapHits,
			gRendererSkyPerfTraceStats.createCachedCubemapHits,
			gRendererSkyPerfTraceStats.solidActivateHits,
			gRendererSkyPerfTraceStats.solidCreateHits);
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
	mLastHistoryResetReason = (reason != nullptr && *reason != '\0') ? reason : "unspecified";
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
	Printf("NRI PT analytic lights: active=%u manual=%u rules=%u overlay_rules=%u map_rules=%u matched_surfaces=%u overlay_matches=%u deduped=%u truncated=%u limit=%u\n",
		(uint32_t)analyticLights.activeLights.size(),
		(uint32_t)analyticLights.manualLights.size(),
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.actorOverlayRuleCount,
		analyticLights.mapOverlayRuleCount,
		analyticLights.matchedSurfaceCount,
		analyticLights.actorOverlayMatchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount,
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	if (analyticLights.activeLights.empty())
	{
		return;
	}

	for (const SceneLightSystem::SceneAnalyticLight& light : analyticLights.activeLights)
	{
		const char* sourceBase =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_Manual) != 0 ? "manual" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? "overlay" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? "overlay" :
			"heuristic";
		const char* sourceSuffix =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_SpriteTileHeuristic) != 0 ? ":sprite_tile" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? ":actor" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? ":map" :
			"";
		Printf("NRI PT analytic light %u: id=%u stable=0x%016llx source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			sourceBase,
			sourceSuffix,
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

bool NRIRenderer::AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId)
{
	if (center == nullptr || diameter <= 0.0f)
	{
		return false;
	}

	if (mRuntimeDebugSpheres.size() >= NriPtDebugSphereLimit)
	{
		return false;
	}

	RuntimeDebugSphere sphere = {};
	sphere.id = mNextRuntimeDebugSphereId++;
	nri_scene::Copy3(center, sphere.center);
	sphere.diameter = diameter;
	sphere.metalness = clamp(metalness, 0.0f, 1.0f);
	sphere.roughness = clamp(roughness, 0.0f, 1.0f);
	mRuntimeDebugSpheres.push_back(sphere);
	outId = sphere.id;
	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

bool NRIRenderer::RemoveRuntimeDebugSphere(uint32_t id)
{
	const auto it = std::find_if(mRuntimeDebugSpheres.begin(), mRuntimeDebugSpheres.end(),
		[id](const RuntimeDebugSphere& sphere)
		{
			return sphere.id == id;
		});
	if (it == mRuntimeDebugSpheres.end())
	{
		return false;
	}

	mRuntimeDebugSpheres.erase(it);
	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

void NRIRenderer::ClearRuntimeDebugSpheres()
{
	if (mRuntimeDebugSpheres.empty())
	{
		return;
	}

	mRuntimeDebugSpheres.clear();
	RequestHistoryReset("runtime-debug-sphere-change");
}

void NRIRenderer::PrintRuntimeDebugSpheres() const
{
	Printf("NRI PT debug spheres: active=%u limit=%u tessellation=%ux%u triangles_per_sphere=%u\n",
		(uint32_t)mRuntimeDebugSpheres.size(),
		NriPtDebugSphereLimit,
		GetRuntimeDebugSphereLongitudeSegments(),
		GetRuntimeDebugSphereLatitudeSegments(),
		GetRuntimeDebugSphereTriangleCount());
	for (const RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		float worldPosition[3] = {};
		PathTracingToWorldPosition(sphere.center, worldPosition);
		Printf("NRI PT debug sphere %u: id=%u render_pos=(%.3f, %.3f, %.3f) world_pos=(%.3f, %.3f, %.3f) diameter=%.3f metalness=%.3f roughness=%.3f\n",
			sphere.id,
			sphere.id,
			sphere.center[0],
			sphere.center[1],
			sphere.center[2],
			worldPosition[0],
			worldPosition[1],
			worldPosition[2],
			sphere.diameter,
			sphere.metalness,
			sphere.roughness);
	}
}

uint32_t NRIRenderer::GetRuntimeDebugSphereCount() const
{
	return (uint32_t)mRuntimeDebugSpheres.size();
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

void NRIRenderer::NotifyGlowControlChange()
{
	QueueStaticMapSceneLightingInvalidation();
	ResetPersistentDynamicEmissiveCache();
	RequestHistoryReset("glow-control-change");
}

void NRIRenderer::NotifyDebugSphereTessellationChange()
{
	RequestHistoryReset("debug-sphere-tessellation-change");
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
	SyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = ShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = ResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const bool expectsUpscalerJitter = resolvedMain == NRIMainUpscalerKind::DLSR || resolvedMain == NRIMainUpscalerKind::DLRR;
	const uint32_t expectedJitterPhases = expectsUpscalerJitter ? GetUpscalerJitterPhaseCount(resolvedUpscalerMode) : NRI_TAA_JITTER_PHASE_COUNT;
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
	const NRITextureResource& srInput = GetFrameTexture(FrameTextureSlot::SrInput);
	const NRITextureResource& rrInput = GetFrameTexture(FrameTextureSlot::RrInput);
	const NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	const NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);
	const NRITextureResource& postSharpenOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	const auto& frameGenPolicy = mFrameBuffer->mFrameGeneration.GetPolicy();
	const bool hasFrameGenDesc = mFrameBuffer->mFrameGeneration.HasFrameDesc();
	const auto& frameGenDesc = mFrameBuffer->mFrameGeneration.GetFrameDesc();
	const auto& frameGenAudit = mFrameBuffer->mFrameGeneration.GetInputAudit();
	const auto& frameGenProvider = mFrameBuffer->mFrameGeneration.GetProviderState();

	Printf("NRI PT status: support=%s", mPathTracingSupported ? "available" : "raster-fallback");
	if (!mPathTracingSupported)
	{
		Printf(" (%s)", GetAvailabilityReason());
	}
	Printf("\n");
	Printf("NRI PT frame: index=%u fg_frame_id=%llu render=%ux%u output=%ux%u prev_camera=%s reset_history=%s\n",
		mFrameIndex,
		(unsigned long long)mFrameGenerationFrameId,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		mHasPreviousCameraState ? "yes" : "no",
		mResetHistory ? "yes" : "no");
	Printf("NRI PT features: bootstrap=%s denoise=%s validation=%s api_validation=%s dred=%s main_upscaler=%s->%s post_sharpen=%s->%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f sharpness=%.3f\n",
		nri_ptbootstrap ? "on" : "off",
		nri_denoise ? "on" : "off",
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		GetMainUpscalerName(requestedMain),
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(requestedPost),
		GetPostSharpenName(resolvedPost),
		GetUpscalerModeName(requestedUpscalerMode),
		GetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		resolvedRenderScale,
		(float)nri_sharpness);
	Printf("NRI PT framegen policy: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s frame_desc=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		hasFrameGenDesc ? "captured" : "empty",
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	if (hasFrameGenDesc)
	{
		Printf("NRI PT framegen inputs: frame_id=%llu hudless=%s:%ux%u ui=%ux%u motion=%ux%u depth=%ux%u render_rect=%u,%u+%ux%u output_rect=%u,%u+%ux%u reset=%s prev_camera=%s frame_time=%s frame_time_ms=%.3f\n",
			(unsigned long long)frameGenDesc.frameId,
			NRIFrameGenerationContext::GetColorSourceName(frameGenDesc.hudlessColorSource),
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->width : 0u,
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->height : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->width : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->height : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->width : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->height : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->width : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->height : 0u,
			frameGenDesc.renderRect.left,
			frameGenDesc.renderRect.top,
			frameGenDesc.renderRect.width,
			frameGenDesc.renderRect.height,
			frameGenDesc.outputRect.left,
			frameGenDesc.outputRect.top,
			frameGenDesc.outputRect.width,
			frameGenDesc.outputRect.height,
			frameGenDesc.resetReason[0] != '\0' ? frameGenDesc.resetReason : "none",
			frameGenDesc.hasPreviousCamera ? "yes" : "no",
			frameGenDesc.hasRealFrameTimeMs ? "captured" : "pending",
			frameGenDesc.realFrameTimeMs);
		Printf("NRI PT framegen contract: motion=%s/%s scale=%.3f,%.3f depth=%s inverted=%s infinite=%s jitter=current(%.3f,%.3f) prev(%.3f,%.3f) fsr3=motion:%s depth:%s prepare:%s adapter:%s reason=%s\n",
			NRIFrameGenerationContext::GetMotionVectorSpaceName(frameGenDesc.motionVectorSpace),
			NRIFrameGenerationContext::GetMotionVectorDirectionName(frameGenDesc.motionVectorDirection),
			frameGenDesc.motionVectorScale[0],
			frameGenDesc.motionVectorScale[1],
			NRIFrameGenerationContext::GetDepthTypeName(frameGenDesc.depthType),
			frameGenDesc.depthInverted ? "yes" : "no",
			frameGenDesc.depthInfinite ? "yes" : "no",
			frameGenDesc.cameraJitter[0],
			frameGenDesc.cameraJitter[1],
			frameGenDesc.previousCameraJitter[0],
			frameGenDesc.previousCameraJitter[1],
			frameGenAudit.fsr3MotionCompatible ? "yes" : "no",
			frameGenAudit.fsr3DepthCompatible ? "yes" : "no",
			frameGenAudit.fsr3PrepareInputsRequired ? "yes" : "no",
			NRIFrameGenerationContext::GetAdapterRequirementName(frameGenAudit.adapterRequirement),
			frameGenAudit.statusReason);
	}
	Printf("NRI PT resolution policy: policy=%s render=%ux%u output=%ux%u jitter=%s phases=%u\n",
		GetRenderResolutionPolicyName(resolvedMain),
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		expectsUpscalerJitter ? "upscaler" : (ShouldRunAppTaa(resolvedMain) ? "taa" : "off"),
		expectedJitterPhases);
	Printf("NRI PT output shell: family=%s sr_input=%ux%u rr_input=%ux%u guides=%ux%u vendor=%ux%u post_output=%ux%u post=%s active=%s last_reset_reason=%s\n",
		GetUpscalerFamilyName(resolvedMain, runAppTaa),
		srInput.width,
		srInput.height,
		rrInput.width,
		rrInput.height,
		upscalerDepth.width,
		upscalerDepth.height,
		vendorOutput.width,
		vendorOutput.height,
		postSharpenOutput.width,
		postSharpenOutput.height,
		GetPostSharpenName(resolvedPost),
		resolvedPost == NRIPostSharpenKind::Off ? "pre-post" : "post-sharpen-output",
		mLastHistoryResetReason.c_str());
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d\n",
		nri_ptdirectscene ? "on" : "off",
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		ClampTraceBounceCount((int)nri_ptportaldepth, 8u),
		(int)nri_ptsurfaceprobe);
	Printf("NRI PT lighting shell: directional=%s sector=%s emissive_heuristics=%s\n",
		mDirectionalLightState.enabled ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off",
		nri_ptemissiveheuristics ? "on" : "off");
	Printf("NRI PT directional light: source=%s shadow=%s rule=%u dir=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) angular=%.3f\n",
		GetDirectionalLightSourceName(mDirectionalLightState),
		mDirectionalLightState.enabled && mDirectionalLightState.shadow ? "on" : "off",
		mDirectionalLightState.ruleId,
		mDirectionalLightState.direction[0],
		mDirectionalLightState.direction[1],
		mDirectionalLightState.direction[2],
		mDirectionalLightState.color[0],
		mDirectionalLightState.color[1],
		mDirectionalLightState.color[2],
		mDirectionalLightState.angularSize);
	Printf("NRI PT transparent shell: trace_transparent=placeholder_noop\n");
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
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=trace_transparent 35=sr_input 36=sr_depth 37=vendor_output 38=vendor_output_final 39=rr_input 40=rr_diffuse_albedo 41=rr_specular_albedo 42=rr_normal_roughness 43=rr_specular_hit_distance 44=post_sharpen_output");
	const char* shadowSplitMode =
		!mUseSplitShadowDenoiser ? "off" :
		(GetEffectivePtDebugMode() >= 21 && GetEffectivePtDebugMode() <= 23) ? "sigma-debug" :
		"sigma-beauty";
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		nrdMaxFrames,
		nrdFastFrames,
		nrdStabilizationFrames,
		nri_nrdantifirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(nrdHitDistanceReconstruction),
		GetNrdInputSplitModeName(nrdInputSplit),
		shadowSplitMode);
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
	SyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	Printf("NRI PT temporal: debug=%d requested_main=%s resolved_main=%s requested_post=%s resolved_post=%s taa=%s last_debug=%d last_main=%s last_post=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		(int)nri_ptdebug,
		GetMainUpscalerName(requestedMain),
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(requestedPost),
		GetPostSharpenName(resolvedPost),
		nri_pttaa ? "on" : "off",
		mLastDebugMode,
		GetMainUpscalerName(mLastTemporalHistoryMainUpscaler),
		GetPostSharpenName(mLastTemporalPostSharpen),
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
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	Printf("NRI PT temporal trace: armed=%d reason=%s frame=%u debug=%d resolved_main=%s resolved_post=%s\n",
		(int)nri_pttraceframes,
		reason != nullptr ? reason : "unspecified",
		mFrameIndex,
		(int)nri_ptdebug,
		GetMainUpscalerName(resolvedMain),
		GetPostSharpenName(resolvedPost));
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (nri_pttraceframes <= 0)
	{
		return;
	}

	const NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	const NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	const NRITextureResource& primary = GetFrameTexture(primarySlot);
	const NRITextureResource& secondary = secondarySlot == FrameTextureSlot::Count ? GetFrameTexture(mHistoryOutputSlot) : GetFrameTexture(secondarySlot);
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		stage != nullptr ? stage : "unknown",
		mFrameIndex,
		(int)nri_ptdebug,
		GetMainUpscalerName(resolvedMainUpscaler),
		GetPostSharpenName(resolvedPostSharpen),
		runAppTaa ? "yes" : "no",
		mResetHistory ? "yes" : "no",
		mLastHistoryResetReason.c_str(),
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

void NRIRenderer::PrunePersistentDynamicEmissiveCacheToLiveActors()
{
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return;
	}

	std::unordered_map<int32_t, bool> liveActorIndices;
	liveActorIndices.reserve(256);

	TSpriteIterator<DCoreActor> it;
	while (auto actor = it.Next())
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}

		liveActorIndices[(int32_t)actor->GetIndex()] = true;
	}

	bool needsPrune = false;
	auto detectStaleActorOwnership = [&needsPrune, &liveActorIndices](const auto& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				needsPrune = true;
				return;
			}
		}
	};

	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
	if (!needsPrune)
	{
		return;
	}

	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = mPersistentDynamicEmissiveCache.sceneView.drawInfo;
	next.sceneView.sky = mPersistentDynamicEmissiveCache.sceneView.sky;
	Copy3(mPersistentDynamicEmissiveCache.sceneView.skyColor, next.sceneView.skyColor);
	Copy3(mPersistentDynamicEmissiveCache.sceneView.groundColor, next.sceneView.groundColor);

	auto appendLiveOwnedSurfaces = [&liveActorIndices](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				continue;
			}

			destination.push_back(surface);
		}
	};

	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
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
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.surfaceProbeMs);
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
		const auto& materialData = materials->materials[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.textureId = metadata.textureId;
		result.materialClass = metadata.materialClass;
		result.lightLevel = metadata.lightLevel;
		result.alpha = metadata.alpha;
		result.normalTextureIndex = metadata.normalTextureIndex;
		result.metallicTextureIndex = metadata.metallicTextureIndex;
		result.roughnessTextureIndex = metadata.roughnessTextureIndex;
		result.metalnessHint = materialData.metalnessHint;
		result.roughnessHint = materialData.roughnessHint;
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
		effectiveMaterial.normalTextureIndex = materialData.normalTextureIndex;
		effectiveMaterial.metallicTextureIndex = materialData.metallicTextureIndex;
		effectiveMaterial.roughnessTextureIndex = materialData.roughnessTextureIndex;
		effectiveMaterial.metalnessHint = materialData.metalnessHint;
		effectiveMaterial.roughnessHint = materialData.roughnessHint;
		effectiveMaterial.emissiveTextureIndex = metadata.emissiveTextureIndex;
		mSceneLights.ApplyEmissiveMaterialSettings(metadata, effectiveMaterial);
		result.emissiveMode = effectiveMaterial.emissiveMode;
		result.emissiveTextureIndex = effectiveMaterial.emissiveTextureIndex;
	}

	if (result.hit)
	{
		if (!mSurfaceProbeFrame.valid)
		{
			result.sceneDataSource = UINT32_MAX;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_UNKNOWN;
		}
		else if (!mSurfaceProbeFrame.usesStaticMapScene)
		{
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_DYNAMIC;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_CAPTURED_SCENE;
		}
		else if (result.primitiveIndex < mSurfaceProbeFrame.staticPrimitiveCount)
		{
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_STATIC;
			result.sceneOwner = NRI_SURFACE_PROBE_OWNER_STATIC_MAP;
		}
		else
		{
			uint32_t overlayPrimitiveIndex = result.primitiveIndex - mSurfaceProbeFrame.staticPrimitiveCount;
			result.sceneDataSource = NRI_SCENE_DATA_SOURCE_DYNAMIC;
			if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount)
			{
				result.sceneOwner = NRI_SURFACE_PROBE_OWNER_RUNTIME_LINK;
			}
			else
			{
				overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount);
				if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeMutationPrimitiveCount)
				{
					result.sceneOwner = NRI_SURFACE_PROBE_OWNER_RUNTIME_MUTATION;
				}
				else
				{
					overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeMutationPrimitiveCount);
					result.sceneOwner = overlayPrimitiveIndex < mSurfaceProbeFrame.dynamicPrimitiveCount ?
						NRI_SURFACE_PROBE_OWNER_DYNAMIC_OVERLAY :
						NRI_SURFACE_PROBE_OWNER_UNKNOWN;
				}
			}
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
			a.textureId == b.textureId &&
			a.materialLightingFlags == b.materialLightingFlags &&
			a.primitiveFlags == b.primitiveFlags &&
			a.sceneDataSource == b.sceneDataSource &&
			a.sceneOwner == b.sceneOwner &&
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
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (result.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)result.provenance.mapChunkIndex;
		for (const auto& chunkCache : mStaticMapScene.chunks)
		{
			if (chunkCache.chunkIndex == chunkIndex)
			{
				chunkResidentStatic = true;
				chunkStaticTlasInstanced =
					!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				chunkStaticProbeIncluded =
					!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				break;
			}
		}
		if (chunkIndex < mRuntimeMapMutations.chunks.size())
		{
			const auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
			chunkReplaced = replacement.active;
			chunkSectorDirty = replacement.sectorDirty;
			chunkDragged = replacement.dragged;
			chunkBlindSpot = replacement.blindSpot;
			chunkReasonMask = replacement.reasonMask;
			chunkSectionDirtyCount = replacement.sectionDirtyCount;
			replacementSurfaceCount = replacement.surfaceCount;
			replacementTriangleCount = replacement.triangleCount;
		}
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	FString textureName;
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.textureId, textureName, legacyTile);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s owner=%s data_source=%s chunk=%d static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u texid=%u legacy_tile=%d texture_name=%s distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(result.provenance.sourceType),
		GetDrawListTypeName(result.provenance.drawListType),
		GetSurfaceProbeSceneOwnerName(result.sceneOwner),
		GetSceneDataSourceName(result.sceneDataSource),
		result.provenance.mapChunkIndex,
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
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
		legacyTile,
		textureName.GetChars(),
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
		YesNo(result.normalTextureIndex != UINT32_MAX),
		YesNo(result.metallicTextureIndex != UINT32_MAX),
		YesNo(result.roughnessTextureIndex != UINT32_MAX),
		result.normalTextureIndex != UINT32_MAX ? result.normalTextureIndex : 0u,
		result.metallicTextureIndex != UINT32_MAX ? result.metallicTextureIndex : 0u,
		result.roughnessTextureIndex != UINT32_MAX ? result.roughnessTextureIndex : 0u,
		result.metalnessHint,
		result.roughnessHint,
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
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)mLastSurfaceProbe.provenance.mapChunkIndex;
		for (const auto& chunkCache : mStaticMapScene.chunks)
		{
			if (chunkCache.chunkIndex == chunkIndex)
			{
				chunkResidentStatic = true;
				chunkStaticTlasInstanced =
					!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				chunkStaticProbeIncluded =
					!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
					chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
					mRuntimeMapMutations.replacedChunkMask[chunkIndex] == 0;
				break;
			}
		}
		if (chunkIndex < mRuntimeMapMutations.chunks.size())
		{
			const auto& replacement = mRuntimeMapMutations.chunks[chunkIndex];
			chunkReplaced = replacement.active;
			chunkSectorDirty = replacement.sectorDirty;
			chunkDragged = replacement.dragged;
			chunkBlindSpot = replacement.blindSpot;
			chunkReasonMask = replacement.reasonMask;
			chunkSectionDirtyCount = replacement.sectionDirtyCount;
			replacementSurfaceCount = replacement.surfaceCount;
			replacementTriangleCount = replacement.triangleCount;
		}
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	Printf("NRI PT surface probe: source=%s drawlist=%s owner=%s data_source=%s chunk=%d static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		GetSurfaceSourceTypeName(mLastSurfaceProbe.provenance.sourceType),
		GetDrawListTypeName(mLastSurfaceProbe.provenance.drawListType),
		GetSurfaceProbeSceneOwnerName(mLastSurfaceProbe.sceneOwner),
		GetSceneDataSourceName(mLastSurfaceProbe.sceneDataSource),
		mLastSurfaceProbe.provenance.mapChunkIndex,
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
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
		YesNo(mLastSurfaceProbe.normalTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX),
		YesNo(mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX),
		mLastSurfaceProbe.normalTextureIndex != UINT32_MAX ? mLastSurfaceProbe.normalTextureIndex : 0u,
		mLastSurfaceProbe.metallicTextureIndex != UINT32_MAX ? mLastSurfaceProbe.metallicTextureIndex : 0u,
		mLastSurfaceProbe.roughnessTextureIndex != UINT32_MAX ? mLastSurfaceProbe.roughnessTextureIndex : 0u,
		mLastSurfaceProbe.metalnessHint,
		mLastSurfaceProbe.roughnessHint,
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

void NRIRenderer::PrintMapChunkDump(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk dump: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk dump: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk dump: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& chunk = mMapWorld.chunks[(unsigned)chunkIndex];
	const auto staticChunkIt = std::find_if(
		mStaticMapScene.chunks.begin(),
		mStaticMapScene.chunks.end(),
		[chunkIndex](const StaticMapSceneCache::ChunkCache& cache) { return cache.chunkIndex == (uint32_t)chunkIndex; });
	const bool residentStatic = staticChunkIt != mStaticMapScene.chunks.end();
	const bool staticTlasInstanced =
		residentStatic &&
		(!mSurfaceProbeFrame.staticTlasExcludesReplacedChunks ||
		 (unsigned)chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
		 mRuntimeMapMutations.replacedChunkMask[(unsigned)chunkIndex] == 0);
	const bool staticProbeIncluded =
		residentStatic &&
		(!mSurfaceProbeFrame.staticProbeExcludesReplacedChunks ||
		 (unsigned)chunkIndex >= mRuntimeMapMutations.replacedChunkMask.size() ||
		 mRuntimeMapMutations.replacedChunkMask[(unsigned)chunkIndex] == 0);
	const auto* replacement =
		(unsigned)chunkIndex < mRuntimeMapMutations.chunks.size() ?
		&mRuntimeMapMutations.chunks[(unsigned)chunkIndex] :
		nullptr;

	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t surfaceTriangleCount = 0;
	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		surfaceTriangleCount += CountSurfaceTriangles(surface.surface);
		if ((surface.surface.material.flags & (nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror)) != 0)
		{
			portalSurfaceCount++;
		}
		if ((surface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0)
		{
			skySurfaceCount++;
		}
	}

	uint32_t sourcePortalCount = 0;
	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex == (uint32_t)chunkIndex)
		{
			sourcePortalCount++;
		}
	}

	Printf("NRI PT chunk dump: chunk=%d sector=%d local_space=%u surfaces=%u tris=%u portal_surfaces=%u sky_surfaces=%u source_portals=%u resident_static=%s static_tlas_instanced=%s static_probe_included=%s runtime_replaced=%s replacement_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u\n",
		chunkIndex,
		chunk.sectorIndex,
		chunk.localSpaceIndex,
		chunk.surfaceCount,
		surfaceTriangleCount,
		portalSurfaceCount,
		skySurfaceCount,
		sourcePortalCount,
		YesNo(residentStatic),
		YesNo(staticTlasInstanced),
		YesNo(staticProbeIncluded),
		YesNo(replacement != nullptr && replacement->active),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		replacement != nullptr ? replacement->sectionDirtyCount : 0u,
		YesNo(replacement != nullptr && replacement->sectorDirty),
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->blindSpot),
		replacement != nullptr ? replacement->surfaceCount : 0u,
		replacement != nullptr ? replacement->triangleCount : 0u);

	if (residentStatic)
	{
		Printf("NRI PT chunk dump static: primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u as_ready=%s\n",
			staticChunkIt->primitiveOffset,
			staticChunkIt->primitiveCount,
			staticChunkIt->materialOffset,
			staticChunkIt->materialCount,
			YesNo(staticChunkIt->accelerationStructure.accelerationStructure != nullptr));
	}

	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex != (uint32_t)chunkIndex)
		{
			continue;
		}

		Printf("NRI PT chunk portal: portal=%u source_surface=%u source_sector=%d source_wall=%d source_plane=%d target_count=%u runtime_bound=%s delta=(%.2f, %.2f, %.2f)\n",
			portal.portalIndex,
			portal.sourceSurfaceIndex,
			portal.sourceSectorIndex,
			portal.sourceWallIndex,
			portal.sourcePlane,
			portal.targetCount,
			YesNo(portal.runtimeBoundTarget),
			(float)portal.delta[0],
			(float)portal.delta[1],
			(float)portal.delta[2]);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		const uint32_t flags = surface.surface.material.flags;
		const uint32_t textureId =
			surface.surface.material.texture != nullptr ?
			(uint32_t)surface.surface.material.texture->GetID().GetIndex() :
			0u;
		Printf("NRI PT chunk surface %u: kind=%s source=%s section=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x flags=0x%x flat=%s sprite=%s mirror=%s sky=%s portal=%s one_way=%s tile=%u pal=%d shade=%d alpha=%.3f verts=%u tris=%u\n",
			surfaceIndex,
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.actorIndex,
			surface.surface.provenance.cstat,
			flags,
			YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
			YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
			YesNo((flags & nri_scene::MaterialFlag_OneWay) != 0),
			textureId,
			surface.surface.material.palette,
			surface.surface.material.shade,
			surface.surface.material.alpha,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::PrintMapChunkCompare(int32_t chunkIndex) const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT chunk compare: no authoritative map world has been built yet.\n");
		return;
	}

	if (chunkIndex < 0)
	{
		if (mLastSurfaceProbe.valid && mLastSurfaceProbe.hit && mLastSurfaceProbe.provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mLastSurfaceProbe.provenance.mapChunkIndex;
		}
		else
		{
			Printf("NRI PT chunk compare: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
			return;
		}
	}

	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		Printf("NRI PT chunk compare: chunk %d is out of range [0,%u).\n", chunkIndex, (uint32_t)mMapWorld.chunks.size());
		return;
	}

	const auto& staticChunk = mMapWorld.chunks[(unsigned)chunkIndex];
	nri_scene::PTMapWorld liveWorld = {};
	nri_scene::PTMapWorldStats liveStats = {};
	if (!nri_scene::BuildLiveMapChunkWorld(staticChunk, liveWorld, &liveStats) ||
		liveWorld.chunks.empty())
	{
		Printf("NRI PT chunk compare: failed to build live runtime chunk %d.\n", chunkIndex);
		return;
	}

	const auto& liveChunk = liveWorld.chunks[0];
	const auto* replacement =
		(unsigned)chunkIndex < mRuntimeMapMutations.chunks.size() ?
		&mRuntimeMapMutations.chunks[(unsigned)chunkIndex] :
		nullptr;

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}
		staticSurfaceIndices.push_back(surfaceIndex);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= liveWorld.surfaces.size())
		{
			break;
		}
		liveSurfaceIndices.push_back(surfaceIndex);
	}

	std::unordered_map<ChunkCompareSurfaceKey, std::vector<uint32_t>, ChunkCompareSurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[BuildChunkCompareSurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<ChunkCompareMatchRecord> matches;
	std::vector<uint32_t> unmatchedStaticSurfaceIndices;
	std::vector<uint32_t> unmatchedLiveSurfaceIndices;
	matches.reserve(std::min(staticSurfaceIndices.size(), liveSurfaceIndices.size()));
	unmatchedStaticSurfaceIndices.reserve(staticSurfaceIndices.size());
	unmatchedLiveSurfaceIndices.reserve(liveSurfaceIndices.size());

	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = mMapWorld.surfaces[staticSurfaceIndex];
		const ChunkCompareSurfaceKey key = BuildChunkCompareSurfaceKey(staticSurface);
		auto it = liveSurfaceLookup.find(key);
		if (it == liveSurfaceLookup.end())
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : it->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		const uint32_t liveSurfaceIndex = liveSurfaceIndices[matchedLiveLocalIndex];
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndex];

		ChunkCompareMatchRecord match = {};
		match.staticSurfaceIndex = staticSurfaceIndex;
		match.liveSurfaceIndex = liveSurfaceIndex;
		match.key = key;
		match.staticMetrics = ComputeChunkCompareSurfaceMetrics(staticSurface);
		match.liveMetrics = ComputeChunkCompareSurfaceMetrics(liveSurface);
		for (int axis = 0; axis < 3; ++axis)
		{
			match.delta[axis] = match.liveMetrics.centroid[axis] - match.staticMetrics.centroid[axis];
		}
		match.deltaDistance = Distance3(match.liveMetrics.centroid, match.staticMetrics.centroid);
		if (match.staticMetrics.area > 0.0001f)
		{
			match.areaRatio = match.liveMetrics.area / match.staticMetrics.area;
		}
		else
		{
			match.areaRatio = match.liveMetrics.area > 0.0001f ? 9999.0f : 1.0f;
		}

		const float staticNormalLength = std::sqrt(Dot3(match.staticMetrics.normal, match.staticMetrics.normal));
		const float liveNormalLength = std::sqrt(Dot3(match.liveMetrics.normal, match.liveMetrics.normal));
		if (staticNormalLength > 0.0001f && liveNormalLength > 0.0001f)
		{
			match.normalDot = std::max(-1.0f, std::min(1.0f, Dot3(match.staticMetrics.normal, match.liveMetrics.normal)));
		}
		else
		{
			match.normalDot = staticNormalLength <= 0.0001f && liveNormalLength <= 0.0001f ? 1.0f : 0.0f;
		}

		match.materialScore =
			(match.staticMetrics.textureId == match.liveMetrics.textureId ? 0.0f : 1.0f) +
			(match.staticMetrics.palette == match.liveMetrics.palette ? 0.0f : 1.0f) +
			(match.staticMetrics.shade == match.liveMetrics.shade ? 0.0f : 1.0f) +
			(match.staticMetrics.materialFlags == match.liveMetrics.materialFlags ? 0.0f : 1.0f) +
			(std::fabs(match.staticMetrics.alpha - match.liveMetrics.alpha) > 0.001f ? 1.0f : 0.0f);
		matches.push_back(match);
	}

	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		if (liveSurfaceUsed[liveLocalIndex] == 0u)
		{
			unmatchedLiveSurfaceIndices.push_back(liveSurfaceIndices[liveLocalIndex]);
		}
	}

	float meanDelta[3] = {};
	for (const auto& match : matches)
	{
		meanDelta[0] += match.delta[0];
		meanDelta[1] += match.delta[1];
		meanDelta[2] += match.delta[2];
	}
	if (!matches.empty())
	{
		const float invMatchCount = 1.0f / (float)matches.size();
		meanDelta[0] *= invMatchCount;
		meanDelta[1] *= invMatchCount;
		meanDelta[2] *= invMatchCount;
	}

	std::unordered_map<int32_t, uint32_t> sectorChunkLookup;
	sectorChunkLookup.reserve(mMapWorld.chunks.size());
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.sectorIndex >= 0)
		{
			sectorChunkLookup.emplace(mapChunk.sectorIndex, mapChunk.chunkIndex);
		}
	}

	uint32_t within1 = 0;
	uint32_t within4 = 0;
	uint32_t areaOutlierCount = 0;
	uint32_t normalOutlierCount = 0;
	uint32_t materialDiffCount = 0;
	uint32_t seamSurfaceCount = 0;
	uint32_t seamOutlierCount = 0;
	uint32_t seamAgainstStaticCount = 0;
	uint32_t seamAgainstReplacedCount = 0;
	for (auto& match : matches)
	{
		const float meanDeltaPoint[3] = { meanDelta[0], meanDelta[1], meanDelta[2] };
		match.deviationFromMean = Distance3(match.delta, meanDeltaPoint);
		const float areaDelta = std::fabs(match.areaRatio - 1.0f);
		if (match.deviationFromMean <= 1.0f)
		{
			within1++;
		}
		if (match.deviationFromMean <= 4.0f)
		{
			within4++;
		}
		if (areaDelta > 0.05f)
		{
			areaOutlierCount++;
		}
		if (match.normalDot < 0.98f)
		{
			normalOutlierCount++;
		}
		if (match.materialScore > 0.0f)
		{
			materialDiffCount++;
		}
		match.score = match.deviationFromMean + areaDelta * 10.0f + (1.0f - match.normalDot) * 10.0f + match.materialScore;

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex >= 0 &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Floor &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Ceiling &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Portal)
		{
			seamSurfaceCount++;
			auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
			const bool adjacentReplaced =
				adjacentChunkIt != sectorChunkLookup.end() &&
				adjacentChunkIt->second < mRuntimeMapMutations.chunks.size() &&
				mRuntimeMapMutations.chunks[adjacentChunkIt->second].active;
			if (adjacentReplaced)
			{
				seamAgainstReplacedCount++;
			}
			else
			{
				seamAgainstStaticCount++;
			}

			if (match.deviationFromMean > 0.5f)
			{
				seamOutlierCount++;
			}
		}
	}

	std::sort(matches.begin(), matches.end(), [](const ChunkCompareMatchRecord& a, const ChunkCompareMatchRecord& b)
	{
		return a.score > b.score;
	});

	const bool likelyCoherent =
		!matches.empty() &&
		unmatchedStaticSurfaceIndices.empty() &&
		unmatchedLiveSurfaceIndices.empty() &&
		within4 + std::max<uint32_t>(1u, (uint32_t)matches.size() / 10u) >= (uint32_t)matches.size() &&
		areaOutlierCount == 0 &&
		normalOutlierCount == 0;

	Printf("NRI PT chunk compare: chunk=%d sector=%d static_surfaces=%u live_surfaces=%u matched=%u unmatched_static=%u unmatched_live=%u reasons=%s dragged=%s replacement_active=%s mean_delta=(%.2f, %.2f, %.2f) within_1=%u within_4=%u area_outliers=%u normal_outliers=%u material_diffs=%u likely_coherent=%s live_tris=%u\n",
		chunkIndex,
		staticChunk.sectorIndex,
		(uint32_t)staticSurfaceIndices.size(),
		(uint32_t)liveSurfaceIndices.size(),
		(uint32_t)matches.size(),
		(uint32_t)unmatchedStaticSurfaceIndices.size(),
		(uint32_t)unmatchedLiveSurfaceIndices.size(),
		replacement != nullptr ? GetRuntimeMapMutationReasonSummary(replacement->reasonMask).c_str() : "none",
		YesNo(replacement != nullptr && replacement->dragged),
		YesNo(replacement != nullptr && replacement->active),
		meanDelta[0],
		meanDelta[1],
		meanDelta[2],
		within1,
		within4,
		areaOutlierCount,
		normalOutlierCount,
		materialDiffCount,
		YesNo(likelyCoherent),
		liveChunk.triangleCount);
	Printf("NRI PT chunk seam compare: chunk=%d border_surfaces=%u seam_outliers=%u adjacent_static=%u adjacent_replaced=%u\n",
		chunkIndex,
		seamSurfaceCount,
		seamOutlierCount,
		seamAgainstStaticCount,
		seamAgainstReplacedCount);

	const size_t outlierCount = std::min<size_t>(matches.size(), 8u);
	for (size_t i = 0; i < outlierCount; ++i)
	{
		const auto& match = matches[i];
		if (match.score <= 0.01f && likelyCoherent)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		const auto& liveSurface = liveWorld.surfaces[match.liveSurfaceIndex];
		Printf("NRI PT chunk compare match: static_surface=%u live_surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f tile_static=%u tile_live=%u flags_static=0x%x flags_live=0x%x\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			GetMapSurfaceKindName(staticSurface.kind),
			GetSurfaceSourceTypeName(staticSurface.surface.provenance.sourceType),
			staticSurface.surface.provenance.sectorIndex,
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.sectionIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			staticSurface.surface.provenance.cstat,
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			match.staticMetrics.textureId,
			match.liveMetrics.textureId,
			staticSurface.surface.material.flags,
			liveSurface.surface.material.flags);
	}

	size_t seamPrinted = 0;
	for (const auto& match : matches)
	{
		if (seamPrinted >= 8u)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex < 0 ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Floor ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Ceiling ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Portal)
		{
			continue;
		}

		auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
		const int32_t adjacentChunkIndex = adjacentChunkIt != sectorChunkLookup.end() ? (int32_t)adjacentChunkIt->second : -1;
		const bool adjacentReplaced =
			adjacentChunkIndex >= 0 &&
			(unsigned)adjacentChunkIndex < mRuntimeMapMutations.chunks.size() &&
			mRuntimeMapMutations.chunks[(unsigned)adjacentChunkIndex].active;
		const bool seamOutlier = match.deviationFromMean > 0.5f;
		if (!seamOutlier && seamPrinted >= 4u)
		{
			continue;
		}

		Printf("NRI PT chunk seam match: static_surface=%u live_surface=%u kind=%s wall=%d nextsector=%d adjacent_chunk=%d adjacent_replaced=%s delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f seam_outlier=%s\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			GetMapSurfaceKindName(staticSurface.kind),
			staticSurface.surface.provenance.wallIndex,
			staticSurface.surface.provenance.nextSectorIndex,
			adjacentChunkIndex,
			YesNo(adjacentReplaced),
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			YesNo(seamOutlier));
		seamPrinted++;
	}

	const size_t unmatchedStaticCount = std::min<size_t>(unmatchedStaticSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedStaticCount; ++i)
	{
		const auto& surface = mMapWorld.surfaces[unmatchedStaticSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_static: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedStaticSurfaceIndices[i],
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}

	const size_t unmatchedLiveCount = std::min<size_t>(unmatchedLiveSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedLiveCount; ++i)
	{
		const auto& surface = liveWorld.surfaces[unmatchedLiveSurfaceIndices[i]];
		Printf("NRI PT chunk compare unmatched_live: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			unmatchedLiveSurfaceIndices[i],
			GetMapSurfaceKindName(surface.kind),
			GetSurfaceSourceTypeName(surface.surface.provenance.sourceType),
			surface.surface.provenance.sectorIndex,
			surface.surface.provenance.wallIndex,
			surface.surface.provenance.sectionIndex,
			surface.surface.provenance.nextSectorIndex,
			surface.surface.provenance.cstat,
			GetSurfaceTextureId(surface),
			surface.surface.material.flags,
			(uint32_t)surface.surface.vertices.size(),
			CountSurfaceTriangles(surface.surface));
	}
}

void NRIRenderer::RefreshSceneLightSystem(
	bool usedStaticMapScene,
	const nri_scene::SceneView* capturedSceneView,
	const nri_scene::MaterialBridgeData* capturedMaterials,
	const nri_scene::SceneView* dynamicSceneView,
	const nri_scene::MaterialBridgeData* dynamicMaterials)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneLightsMs);
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

	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	const NRIDirectionalLightState nextDirectionalLightState = BuildDirectionalLightState(resolvedLightOverlays, nri_ptdirectionallight);
	const bool directionalLightStateChanged =
		!mHasDirectionalLightState ||
		nextDirectionalLightState.stateHash != mDirectionalLightState.stateHash;
	const bool hadDirectionalLightState = mHasDirectionalLightState;
	mDirectionalLightState = nextDirectionalLightState;
	mHasDirectionalLightState = true;
	std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>> actorOverlayRules;
	std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule> mapOverlayRules;
	BuildActorAnalyticOverlayRules(resolvedLightOverlays, actorOverlayRules);
	if (mMapWorld.valid)
	{
		BuildStaticMapAnalyticOverlayRules(resolvedLightOverlays, mMapWorld, mapOverlayRules);
	}

	mSceneLights.RebuildAnalyticLights(
		mFrameIndex,
		NRI_MAX_RUNTIME_POINT_LIGHTS,
		actorOverlayRules.empty() ? nullptr : &actorOverlayRules,
		mapOverlayRules.empty() ? nullptr : &mapOverlayRules);
	mSceneLights.RebuildEmissiveSurfaces(NRI_MAX_EMISSIVE_SURFACES);
	mSceneLights.RebuildSectorLighting(mFrameIndex, (uint32_t)sector.Size());
	if (resolvedLightOverlays.resolvedGeneration != 0 &&
		resolvedLightOverlays.resolvedGeneration != mLastResolvedLightOverlayGeneration)
	{
		const bool hadPreviousGeneration = mLastResolvedLightOverlayGeneration != 0;
		mLastResolvedLightOverlayGeneration = resolvedLightOverlays.resolvedGeneration;
		if (hadPreviousGeneration)
		{
			RequestHistoryReset("lightoverlay-resolve");
		}
	}
	if (hadDirectionalLightState && directionalLightStateChanged)
	{
		RequestHistoryReset("directional-light-change");
	}
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents++;
		}
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
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.lightingInvalidationRequests++;
	}
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

void NRIRenderer::ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const
{
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	if (resolvedLightOverlays.actorOverrideRules.Size() == 0)
	{
		return;
	}

	std::unordered_map<int32_t, uint32_t> actorOverrides;
	BuildActorShadowOverrideMap(resolvedLightOverlays, actorOverrides);
	if (actorOverrides.empty())
	{
		return;
	}

	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end())
		{
			continue;
		}

		if ((it->second & ActorShadowOverride_NoShadowReceive) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
		}
		if ((it->second & ActorShadowOverride_NoShadowCast) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
		}
	}
}

void NRIRenderer::InvalidateStaticMapSceneForMaterialLighting()
{
	if (!mStaticMapScene.valid)
	{
		return;
	}

	mPreservedStaticMapSky = {};
	mPreservedStaticMapSky.valid = true;
	mPreservedStaticMapSky.buildSerial = mStaticMapScene.buildSerial;
	mPreservedStaticMapSky.sceneView.sky = mStaticMapScene.sceneView.sky;
	Copy3(mStaticMapScene.sceneView.skyColor, mPreservedStaticMapSky.sceneView.skyColor);
	Copy3(mStaticMapScene.sceneView.groundColor, mPreservedStaticMapSky.sceneView.groundColor);

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

bool NRIRenderer::RefreshPathTracingAvailability()
{
	return CheckPathTracingSupport();
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.mapWorldMs);
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged)
	{
		RequestHistoryReset("map-load", true, true);
	}
	if (levelChanged && mSceneLights.GetManualAnalyticLightCount() > 0)
	{
		const uint32_t clearedCount = mSceneLights.GetManualAnalyticLightCount();
		ClearRuntimePointLights();
		Printf("NRI PT test lights cleared: count=%u reason=level-change\n", clearedCount);
	}
	if (levelChanged && !mRuntimeDebugSpheres.empty())
	{
		const uint32_t clearedCount = (uint32_t)mRuntimeDebugSpheres.size();
		ClearRuntimeDebugSpheres();
		Printf("NRI PT debug spheres cleared: count=%u reason=level-change\n", clearedCount);
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
	FString traceTransparent = FStringf("TraceTransparent.cs.%s", suffix);
	FString taa = FStringf("Taa.cs.%s", suffix);
	FString rawPresent = FStringf("RawPresent.cs.%s", suffix);
	FString finalPresent = FStringf("FinalPresent.cs.%s", suffix);
	FString dlssSrBefore = FStringf("DlssSrBefore.cs.%s", suffix);
	FString dlssBefore = FStringf("DlssBefore.cs.%s", suffix);
	FString dlssAfter = FStringf("DlssAfter.cs.%s", suffix);
	FString final = FStringf("Final.cs.%s", suffix);

	return
		createPipeline(trace.GetChars(), PipelineSlot::TraceOpaque, mPipelineLayout) &&
		createPipeline(composition.GetChars(), PipelineSlot::Composition, mPipelineLayout) &&
		createPipeline(traceTransparent.GetChars(), PipelineSlot::TraceTransparent, mPipelineLayout) &&
		createPipeline(taa.GetChars(), PipelineSlot::Taa, mTaaPipelineLayout) &&
		createPipeline(rawPresent.GetChars(), PipelineSlot::RawPresent, mTaaPipelineLayout) &&
		createPipeline(finalPresent.GetChars(), PipelineSlot::FinalPresent, mTaaPipelineLayout) &&
		createPipeline(dlssSrBefore.GetChars(), PipelineSlot::DlssSrBefore, mPipelineLayout) &&
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
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &mUpscalerPrepassFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &mUpscalerPrepassOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mTaaFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mTaaOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mRawPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mRawPresentOutputSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 0, &mFinalPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		mFrameBuffer->mCore.AllocateDescriptorSets(*mFrameBuffer->mDescriptorPool, *mTaaPipelineLayout, 1, &mFinalPresentOutputSet, 1, 0) == nri::Result::SUCCESS;
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
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

bool NRIRenderer::UpdateVisibleChunkBuffer()
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : mCurrentVisibleChunkWords.size() * sizeof(uint32_t);
	if (!EnsureStructuredBuffer(
		mVisibleChunkBuffer,
		mVisibleChunkBufferStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIComputeShaderResourceAccess()))
	{
		return false;
	}

	if (mSceneDataDescriptors[19] != mVisibleChunkBuffer.shaderView)
	{
		mSceneDataDescriptors[19] = mVisibleChunkBuffer.shaderView;
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

	if (!UpdateVisibleChunkBuffer())
	{
		return false;
	}

	if (!UpdateVisibleChunkBuffer())
	{
		return false;
	}

	if (sceneInstances.empty())
	{
		return false;
	}

	mBoundRuntimeLightCount = 0;

	if (!EnsureStructuredBuffer(
		mSceneInstanceBuffer,
		mSceneInstanceBufferStats,
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
		mVisibleChunkBuffer.shaderView,
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

	const NRIMainUpscalerKind mainUpscalerKind = ResolveMainUpscalerKind(false);
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(mainUpscalerKind, requestedUpscalerMode);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float renderScale = ResolveRenderScaleForMain(mainUpscalerKind, requestedUpscalerMode, requestedRenderScale);

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

	// Frame-resource rebuilds on resize/upscaler mode changes can retire textures that the current
	// command allocator still references. Drain GPU work before destroying frame-sized resources.
	const bool dimensionsChanged =
		mRenderWidth != renderWidth ||
		mRenderHeight != renderHeight ||
		mOutputWidth != outputWidth ||
		mOutputHeight != outputHeight ||
		mTargetWidth != targetWidth ||
		mTargetHeight != targetHeight;
	WaitForCommandsTracked();
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
	RequestHistoryReset(dimensionsChanged ? "resize" : "frame-resources");
	Printf("NRI PT frame resources: main=%s policy=%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f render=%ux%u output=%ux%u jitter=%s phases=%u\n",
		GetMainUpscalerName(mainUpscalerKind),
		GetRenderResolutionPolicyName(mainUpscalerKind),
		GetUpscalerModeName(requestedUpscalerMode),
		GetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		renderScale,
		renderWidth,
		renderHeight,
		outputWidth,
		outputHeight,
		(mainUpscalerKind == NRIMainUpscalerKind::DLSR || mainUpscalerKind == NRIMainUpscalerKind::DLRR) ? "upscaler" : (ShouldRunAppTaa(mainUpscalerKind) ? "taa" : "off"),
		(mainUpscalerKind == NRIMainUpscalerKind::DLSR || mainUpscalerKind == NRIMainUpscalerKind::DLRR) ? GetUpscalerJitterPhaseCount(resolvedUpscalerMode) : NRI_TAA_JITTER_PHASE_COUNT);

	const nri::Format colorFormat = nri::Format::RGBA16_SFLOAT;
	const nri::Format normalRoughnessFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format upscalerDepthFormat = nri::Format::R32_SFLOAT;
	const nri::Format rrGuideAlbedoFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format rrGuideSpecHitDistanceFormat = nri::Format::R16_SFLOAT;
	const nri::Format rrGuideNormalRoughnessFormat = nri::Format::RGBA16_SFLOAT;
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
		CreateFrameTexture(FrameTextureSlot::TraceTransparentOutput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectLighting, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::DirectEmission, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::SrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::UpscalerDepth, renderWidth, renderHeight, upscalerDepthFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance, renderWidth, renderHeight, rrGuideSpecHitDistanceFormat) &&
		CreateFrameTexture(FrameTextureSlot::RrGuideNormalRoughness, renderWidth, renderHeight, rrGuideNormalRoughnessFormat) &&
		CreateFrameTexture(FrameTextureSlot::VendorOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(FrameTextureSlot::PostSharpenOutput, outputWidth, outputHeight, colorFormat) &&
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
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

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
	UpdateFrameTextureSet(mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors);

	mOutputDescriptors.fill(GetFrameTexture(FrameTextureSlot::VendorOutput).storageView);
	mOutputDescriptors[2] = final.storageView;
	UpdateOutputSet(mUpscalerPrepassOutputSet, mOutputDescriptors);

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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.staticSceneMs);
	if (!mMapWorld.valid)
	{
		return false;
	}

	if (mStaticMapScene.buildSerial != mMapWorld.buildSerial)
	{
		DestroyStaticMapSceneCache();
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		mPreservedStaticMapSky = {};
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

	const nri_scene::SceneView* preservedSkyView =
		(mPreservedStaticMapSky.valid && mPreservedStaticMapSky.buildSerial == mMapWorld.buildSerial)
		? &mPreservedStaticMapSky.sceneView
		: nullptr;
	nri_scene::BuildMapSceneView(mMapWorld, mStaticMapScene.sceneView, preservedSkyView);
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
		nri_scene::BuildMapChunkSceneView(mMapWorld, chunk, chunkSceneView, preservedSkyView);
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

	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds++;
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
	mPreservedStaticMapSky = {};

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
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSkyCalls++;
	}
	ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.ensureSkyTimeUs);
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.preserveExistingHits++;
		}
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.reuseActiveCubemapHits++;
		}
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
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.reuseActiveProbeHits++;
			}
			TraceSkyState(sceneView, "reuse-active-probe", probe.key);
			return true;
		}

		const uint32_t cachedIndex = findCachedSkyTexture(probe.key, probe.width, probe.height);
		if (cachedIndex != UINT32_MAX)
		{
			activateCachedSky(cachedIndex, probe.key, sceneView, nri_scene::PTSkyMode::Cubemap);
			mSkyLevel = currentLevel;
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.activateCachedCubemapHits++;
			}
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.createCachedCubemapHits++;
		}
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
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.holdLevelCubemapHits++;
			}
			TraceSkyState(sceneView, "hold-level-cubemap", mSkyTextureKey);
			return true;
		}

		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.keepLastCubemapHits++;
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
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidReuseHits++;
		}
		TraceSkyState(sceneView, "reuse-active-solid", upload.key);
		return true;
	}

	const uint32_t cachedIndex = findCachedSkyTexture(upload.key, upload.width, upload.height);
	if (cachedIndex != UINT32_MAX)
	{
		activateCachedSky(cachedIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidActivateHits++;
		}
		TraceSkyState(sceneView, "activate-cached-solid", upload.key);
		return true;
	}

	const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::SolidColor);
	if (createdIndex == UINT32_MAX)
	{
		return false;
	}

	activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.solidCreateHits++;
	}
	TraceSkyState(sceneView, "create-cached-solid", upload.key);
	return true;
}

bool NRIRenderer::EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky)
{
	Clocker clock(NriPTSceneTextures);
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
		if (upload.width == 0 || upload.height == 0)
		{
			continue;
		}

		auto it = std::find_if(mTextureCache.begin(), mTextureCache.end(), [&upload](const CachedTexture& entry) { return entry.key == upload.key; });
		if (it == mTextureCache.end())
		{
			std::vector<uint8_t> realizedPixels;
			uint32_t realizedWidth = upload.width;
			uint32_t realizedHeight = upload.height;
			const uint8_t* pixelData = upload.pixels.data();
			if (upload.pixels.empty())
			{
				if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
				{
					continue;
				}
				pixelData = realizedPixels.data();
			}
			else
			{
				realizedWidth = upload.width;
				realizedHeight = upload.height;
			}

			if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
			{
				continue;
			}

			CachedTexture cacheEntry = {};
			cacheEntry.key = upload.key;
			const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
			const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
			if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
				!mFrameBuffer->UploadTextureData(cacheEntry.resource, pixelData, realizedWidth, realizedHeight, rowPitch))
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
		if (material.normalTextureIndex != UINT32_MAX && material.normalTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.normalTextureIndex = UINT32_MAX;
		}
		if (material.metallicTextureIndex != UINT32_MAX && material.metallicTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.metallicTextureIndex = UINT32_MAX;
		}
		if (material.roughnessTextureIndex != UINT32_MAX && material.roughnessTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			material.roughnessTextureIndex = UINT32_MAX;
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
		WaitForCommandsTracked();
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
	NotePerfBufferUpload(&stats, size, needsGrowth);

	if (needsGrowth)
	{
		const uint64_t grownSize = GetGrownBufferSize(resource.size, requiredSize, stride);
		if (resource.buffer != nullptr || resource.shaderView != nullptr)
		{
			WaitForCommandsTracked();
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
			WaitForCommandsTracked();
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
		WaitForCommandsTracked();
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
	std::vector<nri_scene::PrimitiveData> gpuPrimitives = geometry.primitives;
	const size_t primitiveCount = std::min(gpuPrimitives.size(), geometry.primitiveProvenance.size());
	for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const int32_t chunkIndex = geometry.primitiveProvenance[primitiveIndex].mapChunkIndex;
		gpuPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
	}
	for (size_t primitiveIndex = primitiveCount; primitiveIndex < gpuPrimitives.size(); ++primitiveIndex)
	{
		gpuPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
	}

	return
		EnsureStructuredBuffer(vertexBuffer, mVertexBufferStats, geometry.vertices.data(), geometry.vertices.size() * sizeof(nri_scene::SceneVertex), sizeof(nri_scene::SceneVertex), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(indexBuffer, mIndexBufferStats, geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), sizeof(uint32_t), NRIFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIAccelerationStructureBuildInputAccess()) &&
		EnsureStructuredBuffer(primitiveBuffer, mPrimitiveBufferStats, gpuPrimitives.data(), gpuPrimitives.size() * sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, NRIComputeShaderResourceAccess()) &&
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
		WaitForCommandsTracked();
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

void NRIRenderer::BuildFilteredStaticMapGeometry(const std::vector<uint8_t>& replacedChunkMask, nri_scene::GeometryData& outGeometry) const
{
	outGeometry = {};
	outGeometry.vertices.reserve(mStaticMapScene.geometry.vertices.size());
	outGeometry.indices.reserve(mStaticMapScene.geometry.indices.size());
	outGeometry.primitives.reserve(mStaticMapScene.geometry.primitives.size());
	outGeometry.primitiveProvenance.reserve(mStaticMapScene.geometry.primitiveProvenance.size());

	for (const auto& chunk : mStaticMapScene.chunks)
	{
		if (chunk.chunkIndex < replacedChunkMask.size() &&
			replacedChunkMask[chunk.chunkIndex] != 0)
		{
			continue;
		}

		AppendGeometryChunk(
			mStaticMapScene.geometry,
			chunk.vertexOffset,
			chunk.vertexCount,
			chunk.indexOffset,
			chunk.indexCount,
			chunk.primitiveOffset,
			chunk.primitiveCount,
			outGeometry);
	}
}

bool NRIRenderer::BuildRuntimeDebugSphereOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mLastPerfShellTraceStats.runtimeDebugSphereCount = (uint32_t)mRuntimeDebugSpheres.size();
	mLastPerfShellTraceStats.runtimeDebugSphereLongitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	mLastPerfShellTraceStats.runtimeDebugSphereLatitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = 0;

	if (mRuntimeDebugSpheres.empty())
	{
		return false;
	}

	nri_scene::SceneView sphereView = {};
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereViewMs);
		AppendRuntimeDebugSpheresToSceneView(sphereView);
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereGeoMs);
		nri_scene::BuildGeometry(sphereView, outGeometry);
	}
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereMaterialMs);
		nri_scene::BuildMaterials(sphereView, outMaterials);
	}

	const size_t materialCount = std::min(mRuntimeDebugSpheres.size(), outMaterials.materials.size());
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeDebugSphereTuneMs);
		for (size_t i = 0; i < materialCount; ++i)
		{
			const RuntimeDebugSphere& sphere = mRuntimeDebugSpheres[i];
			nri_scene::MaterialData& material = outMaterials.materials[i];
			material.lightLevel = 1.0f;
			material.alpha = 1.0f;
			material.metalnessHint = sphere.metalness;
			material.roughnessHint = sphere.roughness;
			material.materialClass = 0;

			if (i < outMaterials.lightMetadata.size())
			{
				nri_scene::MaterialLightingMetadata& metadata = outMaterials.lightMetadata[i];
				metadata.texture = nullptr;
				metadata.textureId = 0;
				metadata.materialFlags = material.flags;
				metadata.materialClass = material.materialClass;
				metadata.alpha = material.alpha;
				metadata.lightLevel = material.lightLevel;
				metadata.averageColor[0] = 1.0f;
				metadata.averageColor[1] = 1.0f;
				metadata.averageColor[2] = 1.0f;

				uint32_t diameterBits = 0;
				uint32_t metalnessBits = 0;
				uint32_t roughnessBits = 0;
				std::memcpy(&diameterBits, &sphere.diameter, sizeof(diameterBits));
				std::memcpy(&metalnessBits, &sphere.metalness, sizeof(metalnessBits));
				std::memcpy(&roughnessBits, &sphere.roughness, sizeof(roughnessBits));
				metadata.materialKey = HashCombine64(metadata.materialKey, sphere.id);
				metadata.materialKey = HashCombine64(metadata.materialKey, ((uint64_t)diameterBits << 32u) | (uint64_t)metalnessBits);
				metadata.materialKey = HashCombine64(metadata.materialKey, (uint64_t)roughnessBits);
			}
		}
	}

	mLastPerfShellTraceStats.runtimeDebugSpherePrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeDebugSphereMaterialCount = (uint32_t)outMaterials.materials.size();

	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}

void NRIRenderer::AppendRuntimeDebugSpheresToSceneView(nri_scene::SceneView& sceneView) const
{
	if (mRuntimeDebugSpheres.empty())
	{
		return;
	}

	const uint32_t longitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	const uint32_t latitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	sceneView.opaqueFlats.reserve(sceneView.opaqueFlats.size() + mRuntimeDebugSpheres.size());
	sceneView.stats.totalDrawItems += (unsigned int)mRuntimeDebugSpheres.size();
	sceneView.stats.flatDrawItems += (unsigned int)mRuntimeDebugSpheres.size();
	sceneView.stats.materialRefs += (unsigned int)mRuntimeDebugSpheres.size();
	sceneView.stats.triangleEstimate += (unsigned int)(mRuntimeDebugSpheres.size() * GetRuntimeDebugSphereTriangleCount());

	constexpr float Pi = 3.14159265358979323846f;
	auto makeVertex = [Pi](const RuntimeDebugSphere& sphere, float u, float v) -> nri_scene::CapturedVertex
	{
		const float theta = u * 2.0f * Pi;
		const float phi = v * Pi;
		const float radius = sphere.diameter * 0.5f;
		const float sinPhi = sinf(phi);
		nri_scene::CapturedVertex vertex = {};
		vertex.position[0] = sphere.center[0] + radius * sinPhi * cosf(theta);
		vertex.position[1] = sphere.center[1] + radius * cosf(phi);
		vertex.position[2] = sphere.center[2] + radius * sinPhi * sinf(theta);
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = u;
		vertex.uv[1] = v;
		return vertex;
	};
	auto appendTriangle = [](nri_scene::SurfaceRef& surface, const RuntimeDebugSphere& sphere, const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		nri_scene::CapturedVertex v0 = a;
		nri_scene::CapturedVertex v1 = b;
		nri_scene::CapturedVertex v2 = c;

		const float abx = v1.position[0] - v0.position[0];
		const float aby = v1.position[1] - v0.position[1];
		const float abz = v1.position[2] - v0.position[2];
		const float acx = v2.position[0] - v0.position[0];
		const float acy = v2.position[1] - v0.position[1];
		const float acz = v2.position[2] - v0.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float centroidX = (v0.position[0] + v1.position[0] + v2.position[0]) / 3.0f;
		const float centroidY = (v0.position[1] + v1.position[1] + v2.position[1]) / 3.0f;
		const float centroidZ = (v0.position[2] + v1.position[2] + v2.position[2]) / 3.0f;
		const float radialX = centroidX - sphere.center[0];
		const float radialY = centroidY - sphere.center[1];
		const float radialZ = centroidZ - sphere.center[2];
		if (nx * radialX + ny * radialY + nz * radialZ < 0.0f)
		{
			std::swap(v1, v2);
		}

		surface.vertices.push_back(v0);
		surface.vertices.push_back(v1);
		surface.vertices.push_back(v2);
	};

	for (const RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		nri_scene::SurfaceRef surface = {};
		surface.material.texture = nullptr;
		surface.material.palette = 0;
		surface.material.shade = 0;
		surface.material.alpha = 1.0f;
		surface.material.flags = nri_scene::MaterialFlag_None;
		surface.provenance.sourceType = nri_scene::SurfaceSourceType::DebugSphere;

		for (uint32_t lat = 0; lat < latitudeSegments; ++lat)
		{
			const float v0 = (float)lat / (float)latitudeSegments;
			const float v1 = (float)(lat + 1u) / (float)latitudeSegments;
			for (uint32_t lon = 0; lon < longitudeSegments; ++lon)
			{
				const float u0 = (float)lon / (float)longitudeSegments;
				const float u1 = (float)(lon + 1u) / (float)longitudeSegments;
				const auto p00 = makeVertex(sphere, u0, v0);
				const auto p01 = makeVertex(sphere, u1, v0);
				const auto p10 = makeVertex(sphere, u0, v1);
				const auto p11 = makeVertex(sphere, u1, v1);

				if (lat == 0u)
				{
					appendTriangle(surface, sphere, p00, p10, p11);
				}
				else if (lat + 1u == latitudeSegments)
				{
					appendTriangle(surface, sphere, p00, p10, p01);
				}
				else
				{
					appendTriangle(surface, sphere, p00, p10, p11);
					appendTriangle(surface, sphere, p00, p11, p01);
				}
			}
		}

		sceneView.opaqueFlats.push_back(std::move(surface));
	}
}

bool NRIRenderer::BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	outGeometry = {};
	outMaterials = {};
	mRuntimeMapLastFrame = {};
	mLastPerfShellTraceStats.runtimeMutationDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationRebuiltChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationHeldChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationReplacedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = 0;

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
		const bool analyzed = [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAnalyzeMs);
			return nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.baseline, analysis);
		}();
		if (!analyzed)
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
		mLastPerfShellTraceStats.runtimeMutationDirtyChunks++;
		if (replacement.blindSpot)
		{
			mRuntimeMapLastFrame.blindSpotChunkCount++;
		}

		if (!replacement.valid || cachedSignature != replacement.liveSignature || forceTopologyInvalidation)
		{
			nri_scene::SceneView liveChunkView;
			nri_scene::PTMapWorldStats liveStats = {};
			const bool builtChunk = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				if (!nri_scene::BuildLiveMapChunkSceneView(mapChunk, liveChunkView, &liveStats))
				{
					return false;
				}

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
				mLastPerfShellTraceStats.runtimeMutationRebuiltChunks++;
				return true;
			}();
			if (!builtChunk && replacement.valid)
			{
				replacement.active = true;
				mRuntimeMapLastFrame.heldChunkCount++;
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
			}
			else if (!builtChunk)
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
		mLastPerfShellTraceStats.runtimeMutationReplacedChunks++;
		mRuntimeMapLastFrame.replacementSurfaceCount += replacement.surfaceCount;
		mRuntimeMapLastFrame.replacementTriangleCount += replacement.triangleCount;
		mRuntimeMapLastFrame.materialCount += (uint32_t)replacement.materialBridge.materials.size();

		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAppendMs);
			if (!replacement.geometry.primitives.empty())
			{
				AppendGeometry(replacement.geometry, (uint32_t)outMaterials.materials.size(), outGeometry);
			}
			AppendMaterialBridge(replacement.materialBridge, outMaterials);
		}
		TraceRuntimeMapMutationChunk(mapChunk, replacement);
	}

	mRuntimeMapLastFrame.active = mRuntimeMapLastFrame.replacedChunkCount > 0;
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = (uint32_t)outMaterials.materials.size();
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.restoreStaticSceneMs);
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.residentLightRefreshMs);
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.dynamicAsMs);
	mLastPerfShellTraceStats.dynamicAsPrimitiveCount = (uint32_t)geometry.primitives.size();
	mLastPerfShellTraceStats.dynamicAsVertexCount = (uint32_t)geometry.vertices.size();
	mLastPerfShellTraceStats.dynamicAsIndexCount = (uint32_t)geometry.indices.size();
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
	const bool createdAs = [&]()
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsCreateMs);
		return mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, mDynamicBottomLevelAS.accelerationStructure) == nri::Result::SUCCESS;
	}();
	if (!createdAs)
	{
		return false;
	}

	uint64_t requiredScratchSize = 0;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsScratchMs);
		requiredScratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*mDynamicBottomLevelAS.accelerationStructure);
	}
	if (mScratchBuffer.buffer == nullptr || mScratchBuffer.size < requiredScratchSize)
	{
		DestroyBufferResource(mScratchBuffer);
		{
			ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsScratchMs);
			if (!CreateBufferWithoutView(mScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
			{
				return false;
			}
		}
	}

	nri::BuildBottomLevelAccelerationStructureDesc dynamicBuild = {};
	dynamicBuild.dst = mDynamicBottomLevelAS.accelerationStructure;
	dynamicBuild.geometries = &dynamicGeometryDesc;
	dynamicBuild.geometryNum = 1;
	dynamicBuild.scratchBuffer = mScratchBuffer.buffer;
	dynamicBuild.scratchOffset = 0;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsBuildMs);
		mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &dynamicBuild, 1);
	}

	nri::BufferBarrierDesc barrier = {};
	barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*mDynamicBottomLevelAS.accelerationStructure);
	barrier.before = NRIAccelerationStructureWriteAccess();
	barrier.after = NRIAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &barrier;
	barrierDesc.bufferNum = 1;
	{
		ScopedPtPerfTimer phaseTimer(mLastPerfShellTraceStats.dynamicAsBarrierMs);
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	mBuiltDynamicSceneASLastFrame = true;
	mDynamicSceneLastFrame.asBuildCount++;
	return true;
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveTlasMs);
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	static bool sLoggedPhaseBCompositionPath = false;
	static bool sLoggedPhaseGResolvedPresentPath = false;
	static bool sLoggedPhaseGDebugPrepassPath = false;
	static bool sLoggedPhaseFDenoiserPath = false;
	static bool sLoggedPhaseFDenoiserFallback = false;
	static bool sLoggedPhaseFTraceTransparentPath = false;
	static bool sLoggedUpscalerProbePath = false;
	static bool sLoggedRawTraceBypass = false;
	static bool sLoggedPhaseHRrInputPath = false;
	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool bootstrapRawTracePresent = nri_ptbootstrap && (bootstrapMode == 11u || bootstrapMode == 12u);
	const bool useResolvedPresent = !nri_ptbootstrap && ptDebugMode == 0;
	const bool useComposedDebugPresent = !nri_ptbootstrap && ptDebugMode == 15;
	const bool usePostCompositionDebugPresent = !nri_ptbootstrap && (ptDebugMode == 13 || ptDebugMode == 14);
	const bool useUpscalerTraceTransparentProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_TRACE_TRANSPARENT;
	const bool useUpscalerSrInputProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_SR_INPUT;
	const bool useUpscalerSrDepthProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_SR_DEPTH;
	const bool useUpscalerVendorProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_VENDOR_OUTPUT;
	const bool useUpscalerVendorFinalProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_VENDOR_FINAL_PRESENT;
	const bool useUpscalerRrInputProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_RR_INPUT;
	const bool useUpscalerRrDiffuseAlbedoProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_RR_DIFFUSE_ALBEDO;
	const bool useUpscalerRrSpecularAlbedoProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_RR_SPECULAR_ALBEDO;
	const bool useUpscalerRrNormalRoughnessProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_RR_NORMAL_ROUGHNESS;
	const bool useUpscalerRrSpecularHitDistanceProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_RR_SPECULAR_HIT_DISTANCE;
	const bool useUpscalerPostSharpenProbe = !nri_ptbootstrap && ptDebugMode == (int)NRI_PTDEBUG_UPSCALER_POST_SHARPEN_OUTPUT;
	const bool useUpscalerPrepassProbe = useUpscalerSrInputProbe || useUpscalerSrDepthProbe || useUpscalerRrInputProbe ||
		useUpscalerRrDiffuseAlbedoProbe || useUpscalerRrSpecularAlbedoProbe || useUpscalerRrNormalRoughnessProbe ||
		useUpscalerRrSpecularHitDistanceProbe;
	const bool useCompositionPath = useResolvedPresent || useComposedDebugPresent || usePostCompositionDebugPresent ||
		useUpscalerTraceTransparentProbe || useUpscalerPrepassProbe || useUpscalerVendorProbe || useUpscalerVendorFinalProbe ||
		useUpscalerPostSharpenProbe;
	const bool useValidationPresent = !nri_ptbootstrap && ptDebugMode == 9;
	const bool useDenoisedDebugPresent = !nri_ptbootstrap && (ptDebugMode == 16 || ptDebugMode == 17);
	const bool useShadowDebugPresent = !nri_ptbootstrap && (ptDebugMode >= 21 && ptDebugMode <= 23);
	const bool useFinalDebugPresent = !nri_ptbootstrap &&
		((ptDebugMode >= 6 && ptDebugMode <= 8) || (ptDebugMode >= 18 && ptDebugMode <= 20) || useShadowDebugPresent || ptDebugMode == 24 || ptDebugMode == 25);
	const bool rawTraceDirectPresent = !nri_ptbootstrap &&
		((ptDebugMode >= 1 && ptDebugMode <= 5) || (ptDebugMode >= 10 && ptDebugMode <= 12) || (ptDebugMode >= 26 && ptDebugMode <= 33));
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	mUseUpscaledInFinal = false;
	mUseDenoisedCompositionInputs = false;
	const bool directionalLightShadowEnabled = mDirectionalLightState.enabled && mDirectionalLightState.shadow;
	mUseSplitShadowDenoiser = directionalLightShadowEnabled && (useShadowDebugPresent || (useCompositionPath && nri_denoise));

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

		const FrameTextureSlot denoisedSlot = ptDebugMode == 16 ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::DenoisedSpecular;
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
		const NRIMainUpscalerKind resolvedMainKind = ResolveMainUpscalerKind(false);
		const bool buildRrInput = resolvedMainKind == NRIMainUpscalerKind::DLRR;
		const bool needStandardComposition =
			!buildRrInput || useComposedDebugPresent || useUpscalerTraceTransparentProbe;

		mUseDenoisedCompositionInputs = false;

		if (buildRrInput)
		{
			if (!sLoggedPhaseHRrInputPath)
			{
				Printf("NRI Phase H: DLRR now builds a separate noisy RrInput before NRD and bypasses opaque denoising for the vendor RR branch.\n");
				sLoggedPhaseHRrInputPath = true;
			}

			mUseSplitShadowDenoiser = false;
			if (!DispatchComposition(FrameTextureSlot::RrInput))
			{
				return false;
			}
		}

		if (!needStandardComposition)
		{
			return true;
		}

		if (!buildRrInput && nri_denoise)
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
				mUseSplitShadowDenoiser = directionalLightShadowEnabled;
			}
		}

		if (!DispatchComposition(FrameTextureSlot::Composed))
		{
			return false;
		}

		if (!sLoggedPhaseFTraceTransparentPath)
		{
			Printf("NRI Phase F.5: Composition-backed PT paths now pass through placeholder TraceTransparent before output-resolution dispatch.\n");
			sLoggedPhaseFTraceTransparentPath = true;
		}

		if (!DispatchTraceTransparent())
		{
			return false;
		}

		return true;
	};

	if (useResolvedPresent)
	{
		if (!sLoggedPhaseGResolvedPresentPath)
		{
			Printf("NRI Phase G: ptdebug 0 now routes through Composition, placeholder TraceTransparent, DispatchUpscaleChain, and the minimal FinalPresent presenter.\n");
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
		TraceTemporalState("resolved-present", ResolveMainUpscalerKind(false), ResolvePostSharpenKind(false), false, resolvedPresentSlot, mHistoryOutputSlot);
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
			Printf("NRI Phase B: ptdebug 15 now routes through Composition, placeholder TraceTransparent, and the minimal FinalPresent presenter.\n");
			sLoggedPhaseBCompositionPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!DispatchFinalPresent(FrameTextureSlot::TraceTransparentOutput))
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
			Printf("NRI Phase G: ptdebug 13/14 now route through Composition, placeholder TraceTransparent, DispatchUpscaleChain, and direct FinalPresent of the temporal outputs.\n");
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

		const FrameTextureSlot debugSlot = ptDebugMode == 13 ? mHistoryOutputSlot : mUpscaledInputSlot;
		TraceTemporalState("debug13-14-present", ResolveMainUpscalerKind(false), ResolvePostSharpenKind(false), false, debugSlot, mHistoryOutputSlot);
		if (!DispatchFinalPresent(debugSlot))
		{
			return false;
		}

		CopyFinalToActiveTarget();
		return true;
	}

	if (useUpscalerTraceTransparentProbe || useUpscalerPrepassProbe || useUpscalerVendorProbe || useUpscalerVendorFinalProbe || useUpscalerPostSharpenProbe)
	{
		if (!sLoggedUpscalerProbePath)
		{
			Printf("NRI Phase I instrumentation: ptdebug 34..44 now expose TraceTransparentOutput, explicit SR/RR inputs, SR depth, RR guides, vendor output, FinalPresent(VendorOutput), and post-sharpen output.\n");
			sLoggedUpscalerProbePath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (useUpscalerTraceTransparentProbe)
		{
			if (!DispatchRawPresent(FrameTextureSlot::TraceTransparentOutput))
			{
				return false;
			}

			CopyFinalToActiveTarget();
			return true;
		}

		const bool useExplicitSrProbe = useUpscalerSrInputProbe || useUpscalerSrDepthProbe;
		const bool useExplicitRrProbe = useUpscalerRrInputProbe || useUpscalerRrDiffuseAlbedoProbe ||
			useUpscalerRrSpecularAlbedoProbe || useUpscalerRrNormalRoughnessProbe || useUpscalerRrSpecularHitDistanceProbe;
		const NRIMainUpscalerKind debugMainKind =
			useExplicitSrProbe ? NRIMainUpscalerKind::DLSR :
			useExplicitRrProbe ? NRIMainUpscalerKind::DLRR :
			ResolveMainUpscalerKind(true);
		if ((useUpscalerVendorProbe || useUpscalerVendorFinalProbe) && debugMainKind == NRIMainUpscalerKind::Off)
		{
			Printf(TEXTCOLOR_ORANGE "NRI upscaler probe view requires a resolved vendor main upscaler. Current resolved_main=off.\n");
			if (!DispatchRawPresent(FrameTextureSlot::TraceTransparentOutput))
			{
				return false;
			}

			CopyFinalToActiveTarget();
			return true;
		}

		if (useUpscalerPrepassProbe)
		{
			if (!DispatchUpscalerPrepass(debugMainKind))
			{
				return false;
			}

			FrameTextureSlot probeSlot = FrameTextureSlot::UpscalerDepth;
			FrameTextureSlot probeSecondarySlot = FrameTextureSlot::Count;
			FrameTextureSlot probeTertiarySlot = FrameTextureSlot::Count;
			switch ((uint32_t)ptDebugMode)
			{
			case NRI_PTDEBUG_UPSCALER_SR_INPUT: probeSlot = FrameTextureSlot::SrInput; break;
			case NRI_PTDEBUG_UPSCALER_SR_DEPTH: probeSlot = FrameTextureSlot::UpscalerDepth; break;
			case NRI_PTDEBUG_UPSCALER_RR_INPUT: probeSlot = FrameTextureSlot::RrInput; break;
			case NRI_PTDEBUG_UPSCALER_RR_DIFFUSE_ALBEDO: probeSlot = FrameTextureSlot::RrGuideDiffuseAlbedo; break;
			case NRI_PTDEBUG_UPSCALER_RR_SPECULAR_ALBEDO:
				probeSlot = FrameTextureSlot::RrGuideSpecularAlbedo;
				probeSecondarySlot = FrameTextureSlot::BaseColorMetalness;
				probeTertiarySlot = FrameTextureSlot::ViewZ;
				break;
			case NRI_PTDEBUG_UPSCALER_RR_NORMAL_ROUGHNESS: probeSlot = FrameTextureSlot::RrGuideNormalRoughness; break;
			case NRI_PTDEBUG_UPSCALER_RR_SPECULAR_HIT_DISTANCE: probeSlot = FrameTextureSlot::RrGuideSpecularHitDistance; break;
			default: break;
			}
			if (!DispatchRawPresent(probeSlot, probeSecondarySlot, probeTertiarySlot))
			{
				return false;
			}

			CopyFinalToActiveTarget();
			return true;
		}

		if (!DispatchUpscaleChain())
		{
			return false;
		}

		if (useUpscalerVendorFinalProbe)
		{
			if (!DispatchFinalPresent(FrameTextureSlot::VendorOutput))
			{
				return false;
			}

			CopyFinalToActiveTarget();
			return true;
		}

		if (useUpscalerPostSharpenProbe)
		{
			const FrameTextureSlot probeSlot =
				ResolvePostSharpenKind(false) == NRIPostSharpenKind::Off ? mUpscaledInputSlot : FrameTextureSlot::PostSharpenOutput;
			if (!DispatchRawPresent(probeSlot))
			{
				return false;
			}

			CopyFinalToActiveTarget();
			return true;
		}

		if (!DispatchRawPresent(FrameTextureSlot::VendorOutput))
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
		if (ptDebugMode == 11 || ptDebugMode == 12)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredSpecular;
		}

		if (ptDebugMode == 12)
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
	const bool useTemporalJitter = !nri_ptbootstrap && ShouldUseTemporalJitter(ResolveMainUpscalerKind(false));
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
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = mBoundStaticPrimitiveCount;
	constants.FrameIndex = mFrameIndex;
	constants.DynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	constants.Flags =
		(mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		ClampTraceBounceCount((int)nri_ptlightbounces, 4u),
		ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u),
		mDirectionalLightState.color);
	constants.PortalCount = mBoundPortalCount;
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.PortalDepth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	constants.ReservedTrace0 = (mBoundRuntimeLightTileCountX & 0xffffu) | ((mBoundRuntimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)GetSelectedNrdDenoiserMode(),
		std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u),
		mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::SrInput), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Validation), NRIComputeStorageState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::Composed).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = GetFrameTexture(FrameTextureSlot::Validation).storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[0] = GetFrameTexture(FrameTextureSlot::UnfilteredDiffuse).storageView;
	mOutputDescriptors[3] = GetFrameTexture(FrameTextureSlot::Motion).storageView;
	mOutputDescriptors[4] = GetFrameTexture(FrameTextureSlot::ViewZ).storageView;
	mOutputDescriptors[5] = GetFrameTexture(FrameTextureSlot::NormalRoughness).storageView;
	mOutputDescriptors[6] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).storageView;
	mOutputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	mOutputDescriptors[10] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).storageView;
	mOutputDescriptors[11] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
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
	desc.lightDirection[0] = mDirectionalLightState.direction[0];
	desc.lightDirection[1] = mDirectionalLightState.direction[1];
	desc.lightDirection[2] = mDirectionalLightState.direction[2];
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

bool NRIRenderer::DispatchComposition(FrameTextureSlot outputSlot)
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
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = GetNrdInputSplitMode();
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)GetSelectedNrdDenoiserMode(), mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

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
	NRITextureResource& composed = GetFrameTexture(outputSlot);

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

bool NRIRenderer::DispatchTraceTransparent()
{
	Clocker clock(NriPTComposition);

	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::Composed);
	NRITextureResource& transparentOutput = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	CopyTexture(composed, transparentOutput);
	return true;
}

bool NRIRenderer::DispatchUpscalerPrepass(NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
		FrameTextureSlot::RrInput;
	NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
	NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		CopyTexture(GetFrameTexture(FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	mFrameInputDescriptors[2] = GetFrameTexture(FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		mFrameInputDescriptors[3] = GetFrameTexture(FrameTextureSlot::NormalRoughness).shaderView;
		mFrameInputDescriptors[4] = GetFrameTexture(FrameTextureSlot::BaseColorMetalness).shaderView;
		mFrameInputDescriptors[6] = GetFrameTexture(FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!UpdateFrameTextureSet(mUpscalerPrepassFrameTextureSet, mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!UpdateOutputSet(mUpscalerPrepassOutputSet, mOutputDescriptors))
	{
		return false;
	}

	NRITraceConstants constants = {};
	constants.RenderWidth = mRenderWidth;
	constants.RenderHeight = mRenderHeight;
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = mResetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	BindSceneRootDescriptors();
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mSamplerSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mSceneTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 2, mSceneDataSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 3, mUpscalerPrepassFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 4, mUpscalerPrepassOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(useSrPrepass ? PipelineSlot::DlssSrBefore : PipelineSlot::DlssBefore));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mRenderWidth), GetDispatchSize(mRenderHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchRawPresent(FrameTextureSlot inputSlot, FrameTextureSlot secondarySlot, FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRITraceConstants constants = {};
	constants.DisplayWidth = mOutputWidth;
	constants.DisplayHeight = mOutputHeight;
	constants.FrameIndex = mFrameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = GetFrameTexture(inputSlot);
	constants.RenderWidth = input.width;
	constants.RenderHeight = input.height;
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
	inputUpdate.descriptorSet = mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mRawPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mRawPresentOutputSet, nri::BindPoint::COMPUTE });
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
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);

	NRITextureResource& input = GetFrameTexture(inputSlot);
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	constants.ReservedTrace1 = PackUInt16Pair(input.width, input.height);

	mFrameBuffer->TransitionTexture(input, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	mFrameBuffer->mCore.UpdateDescriptorRanges(&outputUpdate, 1);

	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mTaaPipelineLayout);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, mFinalPresentFrameTextureSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, mFinalPresentOutputSet, nri::BindPoint::COMPUTE });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *GetPipeline(PipelineSlot::FinalPresent));
	mFrameBuffer->mCore.CmdDispatch(*mFrameBuffer->mCommandBuffer, { GetDispatchSize(mTargetWidth), GetDispatchSize(mTargetHeight), 1 });
	return true;
}

bool NRIRenderer::DispatchUpscaleChain()
{
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = ResolvePostSharpenKind(true);
	const bool runAppTaa = ShouldRunAppTaa(mainKind);
	NRITextureResource& composed = GetFrameTexture(FrameTextureSlot::TraceTransparentOutput);
	const FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? FrameTextureSlot::RrInput :
		FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = GetFrameTexture(mHistoryInputSlot);
	NRITextureResource& historyOutput = GetFrameTexture(mHistoryOutputSlot);
	TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITraceConstants constants = {};
		constants.RenderWidth = mRenderWidth;
		constants.RenderHeight = mRenderHeight;
		constants.DisplayWidth = mOutputWidth;
		constants.DisplayHeight = mOutputHeight;
		constants.FrameIndex = mFrameIndex;
		constants.DebugMode = GetEffectivePtDebugMode();
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
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 meaningful even when app-TAA is intentionally bypassed for vendor SR.
		CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 meaningful for RR as well by exposing the explicit noisy RR input.
		CopyTexture(GetFrameTexture(FrameTextureSlot::RrInput), historyOutput);
	}

	FrameTextureSlot resolvedInputSlot = mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? FrameTextureSlot::SrInput :
			FrameTextureSlot::RrInput;
		NRITextureResource& vendorInput = GetFrameTexture(vendorInputSlot);
		NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = GetFrameTexture(FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);

		if (!DispatchUpscalerPrepass(mainKind))
		{
			return false;
		}

		mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		mFrameBuffer->TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = ResolveUpscalerModeForMain(mainKind, GetSelectedUpscalerMode());
		if (!mUpscaler.EnsureMainUpscaler(*mFrameBuffer, mainKind, resolvedUpscalerMode, mOutputWidth, mOutputHeight))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &GetFrameTexture(FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.currentWidth = mRenderWidth;
		upscalerDesc.currentHeight = mRenderHeight;
		Copy2(mCurrentJitter, upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, mCurrentViewToClip, sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, mCurrentWorldToView, sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = mResetHistory;
		if (!mUpscaler.DispatchMainUpscaler(*mFrameBuffer, mainKind, upscalerDesc))
		{
			return false;
		}

		mUseUpscaledInFinal = true;
		mUpscaledInputSlot = FrameTextureSlot::VendorOutput;
		resolvedInputSlot = FrameTextureSlot::VendorOutput;
		TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		mUseUpscaledInFinal = false;
		mUpscaledInputSlot = mHistoryOutputSlot;
		resolvedInputSlot = mHistoryOutputSlot;
		TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = GetFrameTexture(resolvedInputSlot);
	NRITextureResource& postOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	mFrameBuffer->TransitionTexture(postInput, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(postOutput, NRIComputeStorageState());
	if (!mUpscaler.EnsurePostSharpen(*mFrameBuffer, postSharpenKind, mOutputWidth, mOutputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = mFrameBuffer->mCommandBuffer;
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(mCurrentJitter, postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = mResetHistory;
	if (!mUpscaler.DispatchPostSharpen(*mFrameBuffer, postSharpenKind, postDesc))
	{
		return false;
	}

	mUseUpscaledInFinal = true;
	mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, mUpscaledInputSlot, resolvedInputSlot);
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
		(mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(mDirectionalLightState.enabled && mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = mBoundStaticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = mBoundDynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, mDirectionalLightState.color);
	constants.RuntimeLightCount = mBoundRuntimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)mSceneLeft | ((uint32_t)(uint16_t)(int16_t)mSceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, mDirectionalLightState.angularSize);
	Copy3(mSkyColor, constants.SkyColor);
	Copy3(mGroundColor, constants.GroundColor);
	ApplyDirectionalLightStateToConstants(mDirectionalLightState, constants);

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
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(GetFrameTexture(FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(history, NRIComputeShaderResourceState());
	mFrameBuffer->TransitionTexture(upscaled, NRIComputeShaderResourceState());
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
	mFrameInputDescriptors[8] = GetFrameTexture(FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	mFrameInputDescriptors[9] = GetFrameTexture(FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.updateStateMs);
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
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	if (!nri_ptbootstrap && ShouldUseTemporalJitter(resolvedMainUpscaler))
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
	const BitArray& visibleSectors = di.GetVisibleSectors();
	const size_t visibleChunkWordCount = std::max<size_t>((mMapWorld.chunks.size() + 31u) / 32u, 1u);
	mCurrentVisibleChunkWords.assign(visibleChunkWordCount, 0u);
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		const int32_t chunkIndex = FindMapChunkIndexForSector(mMapWorld, (int32_t)sectorIndex);
		if (chunkIndex < 0)
		{
			continue;
		}

		const uint32_t visibleChunkIndex = (uint32_t)chunkIndex;
		const size_t wordIndex = visibleChunkIndex >> 5u;
		if (wordIndex >= mCurrentVisibleChunkWords.size())
		{
			continue;
		}

		mCurrentVisibleChunkWords[wordIndex] |= 1u << (visibleChunkIndex & 31u);
	}
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

	if (mHasPreviousCameraState && !mResetHistory)
	{
		const float dx = mCurrentCameraPos[0] - mPreviousCameraPos[0];
		const float dy = mCurrentCameraPos[1] - mPreviousCameraPos[1];
		const float dz = mCurrentCameraPos[2] - mPreviousCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		static constexpr float TeleportDistanceThreshold = 2048.0f;
		if (distanceSq > TeleportDistanceThreshold * TeleportDistanceThreshold)
		{
			RequestHistoryReset("camera-teleport", true, false);
		}
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
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.copyFinalMs);
	Clocker clock(NriPTCopyFinal);

	UpdateFrameGenerationFrameDesc();
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	CopyTextureToActiveTarget(final);
}

void NRIRenderer::UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory)
{
	if (preserveHistory)
	{
		return;
	}

	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPostSharpen = ResolvePostSharpenKind(false);
	const bool runAppTaa = ShouldRunAppTaa(resolvedMainUpscaler);
	if (!nri_ptbootstrap &&
		(debugMode != mLastDebugMode ||
		 resolvedMainUpscaler != mLastTemporalHistoryMainUpscaler ||
		 resolvedPostSharpen != mLastTemporalPostSharpen ||
		 runAppTaa != mLastTemporalAppTaaEnabled))
	{
		ArmTemporalTraceBudget("mode-change");
		if (nri_pttraceframes > 0)
		{
			Printf("NRI PT temporal reset: reason=mode-change frame=%u debug=%d->%d main=%s->%s post=%s->%s app_taa=%s->%s\n",
				mFrameIndex,
				mLastDebugMode,
				debugMode,
				GetMainUpscalerName(mLastTemporalHistoryMainUpscaler),
				GetMainUpscalerName(resolvedMainUpscaler),
				GetPostSharpenName(mLastTemporalPostSharpen),
				GetPostSharpenName(resolvedPostSharpen),
				mLastTemporalAppTaaEnabled ? "yes" : "no",
				runAppTaa ? "yes" : "no");
		}
		RequestHistoryReset("mode-change");
	}
	mLastDebugMode = debugMode;
	mLastTemporalHistoryMainUpscaler = resolvedMainUpscaler;
	mLastTemporalPostSharpen = resolvedPostSharpen;
	mLastTemporalAppTaaEnabled = runAppTaa;

	if (!mHasFrameGenerationConfigState)
	{
		mHasFrameGenerationConfigState = true;
		mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
		mLastFrameGenerationRequestedProvider = frameGenPolicy.requestedProvider;
		mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
		return;
	}

	const char* frameGenResetReason = nullptr;
	if (frameGenPolicy.requestedEnabled != mLastFrameGenerationRequestedEnabled)
	{
		frameGenResetReason = "framegen-toggle";
	}
	else if (frameGenPolicy.requestedProvider != mLastFrameGenerationRequestedProvider)
	{
		frameGenResetReason = "framegen-provider-change";
	}
	else if (frameGenPolicy.resolvedUiMode != mLastFrameGenerationResolvedUiMode)
	{
		frameGenResetReason = "framegen-ui-mode-change";
	}

	if (frameGenResetReason != nullptr)
	{
		RequestHistoryReset(frameGenResetReason);
		if (nri_pttraceframes > 0)
		{
			Printf("NRI PT temporal reset: reason=%s frame=%u requested=%s->%s provider=%s->%s ui=%s->%s\n",
				frameGenResetReason,
				mFrameIndex,
				mLastFrameGenerationRequestedEnabled ? "on" : "off",
				frameGenPolicy.requestedEnabled ? "on" : "off",
				NRIFrameGenerationContext::GetProviderName(mLastFrameGenerationRequestedProvider),
				NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
				NRIFrameGenerationContext::GetUiModeName(mLastFrameGenerationResolvedUiMode),
				NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode));
		}
	}

	mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
	mLastFrameGenerationRequestedProvider = frameGenPolicy.requestedProvider;
	mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
}

void NRIRenderer::NoteSuccessfulRealFrame()
{
	mLastFrameGenerationRealFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	mHasFrameGenerationRealFrameTime = mHasPendingFrameGenerationRealFrameTime;
	mLastFrameGenerationTimestamp = mPendingFrameGenerationTimestamp;
	mHasFrameGenerationTimestamp = true;
	++mFrameGenerationFrameId;
}

void NRIRenderer::UpdateFrameGenerationFrameDesc()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	NRIFrameGenerationFrameDesc desc = {};
	desc.frameId = mFrameGenerationFrameId + 1u;
	desc.renderWidth = mRenderWidth;
	desc.renderHeight = mRenderHeight;
	desc.outputWidth = mOutputWidth;
	desc.outputHeight = mOutputHeight;
	desc.renderRect = { 0u, 0u, mRenderWidth, mRenderHeight };
	desc.outputRect = { 0u, 0u, mOutputWidth, mOutputHeight };
	desc.hasPreviousCamera = mHasPreviousCameraState;
	desc.resetHistory = mResetHistory;
	desc.hasRealFrameTimeMs = mHasPendingFrameGenerationRealFrameTime;
	desc.realFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	const char* resetReason = mResetHistory && !mLastHistoryResetReason.empty() ? mLastHistoryResetReason.c_str() : "none";
	std::strncpy(desc.resetReason, resetReason, std::size(desc.resetReason) - 1u);
	desc.resetReason[std::size(desc.resetReason) - 1u] = '\0';
	desc.hudlessColorSource = NRIFrameGenerationColorSource::Final;
	desc.hudlessColor = &GetFrameTexture(FrameTextureSlot::Final);
	desc.uiTexture = nullptr;
	desc.motionVectors = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.depth = &GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	std::memcpy(desc.cameraJitter, mCurrentJitter, sizeof(desc.cameraJitter));
	std::memcpy(desc.previousCameraJitter, mPreviousJitter, sizeof(desc.previousCameraJitter));
	desc.motionVectorScale[0] = 1.0f;
	desc.motionVectorScale[1] = 1.0f;
	desc.motionVectorSpace = NRIFrameGenerationMotionVectorSpace::ScreenPixels;
	desc.motionVectorDirection = NRIFrameGenerationMotionVectorDirection::CurrentToPrevious;
	desc.depthType = NRIFrameGenerationDepthType::ClipDepth;
	desc.depthInverted = false;
	desc.depthInfinite = false;
	std::memcpy(desc.currentViewToClip, mCurrentViewToClip, sizeof(desc.currentViewToClip));
	std::memcpy(desc.previousViewToClip, mPreviousViewToClip, sizeof(desc.previousViewToClip));
	std::memcpy(desc.currentWorldToView, mCurrentWorldToView, sizeof(desc.currentWorldToView));
	std::memcpy(desc.previousWorldToView, mPreviousWorldToView, sizeof(desc.previousWorldToView));
	std::memcpy(desc.cameraPosition, mCurrentCameraPos, sizeof(desc.cameraPosition));
	std::memcpy(desc.cameraForward, mCurrentCameraForward, sizeof(desc.cameraForward));
	std::memcpy(desc.cameraRight, mCurrentCameraRight, sizeof(desc.cameraRight));
	std::memcpy(desc.cameraUp, mCurrentCameraUp, sizeof(desc.cameraUp));
	desc.cameraNear = screen->GetZNear();
	desc.cameraFar = screen->GetZFar();
	desc.cameraFovVerticalRadians = 2.0f * atanf(mCurrentTanHalfFovY);
	desc.viewSpaceToMetersFactor = 1.0f;
	mFrameBuffer->mFrameGeneration.SetFrameDesc(*mFrameBuffer, desc);
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
	DestroyBufferResource(mVisibleChunkBuffer);
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
		WaitForCommandsTracked();
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

bool NRIRenderer::IsMainUpscalerSupported(NRIMainUpscalerKind kind) const
{
	if (kind == NRIMainUpscalerKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIMainUpscalerKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, ToMainUpscalerType(kind));
}

bool NRIRenderer::IsPostSharpenSupported(NRIPostSharpenKind kind) const
{
	if (kind == NRIPostSharpenKind::Off || mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return kind == NRIPostSharpenKind::Off;
	}

	return mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, ToPostSharpenType(kind));
}

NRIMainUpscalerKind NRIRenderer::ResolveMainUpscalerKind(bool logFallback)
{
	SyncLegacyUpscalerConfig(logFallback);
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();
	NRIMainUpscalerKind resolved = requested;

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			resolved =
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			resolved = NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastMainUpscalerRequest != (int)nri_upscaler || mLastMainUpscalerResolved != resolved))
	{
		Printf("NRI main upscaler fallback: requested %s is unavailable on %s, using %s\n",
			GetMainUpscalerName(requested),
			(const char*)nri_api,
			GetMainUpscalerName(resolved));
		mLastMainUpscalerRequest = (int)nri_upscaler;
		mLastMainUpscalerResolved = resolved;
	}

	return resolved;
}

NRIPostSharpenKind NRIRenderer::ResolvePostSharpenKind(bool logFallback)
{
	SyncLegacyUpscalerConfig(logFallback);
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	NRIPostSharpenKind resolved = requested;

	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		resolved = NRIPostSharpenKind::Off;
	}

	if (logFallback &&
		(requested != resolved) &&
		(mLastPostSharpenRequest != (int)nri_postsharpen || mLastPostSharpenResolved != resolved))
	{
		Printf("NRI post sharpen fallback: requested %s is unavailable on %s, using %s\n",
			GetPostSharpenName(requested),
			(const char*)nri_api,
			GetPostSharpenName(resolved));
		mLastPostSharpenRequest = (int)nri_postsharpen;
		mLastPostSharpenResolved = resolved;
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
	case FrameTextureSlot::TraceTransparentOutput: return "TraceTransparentOutput";
	case FrameTextureSlot::DirectLighting: return "DirectLighting";
	case FrameTextureSlot::DirectEmission: return "DirectEmission";
	case FrameTextureSlot::TaaHistoryPing: return "TaaHistoryPing";
	case FrameTextureSlot::TaaHistoryPong: return "TaaHistoryPong";
	case FrameTextureSlot::Validation: return "Validation";
	case FrameTextureSlot::SrInput: return "SrInput";
	case FrameTextureSlot::RrInput: return "RrInput";
	case FrameTextureSlot::UpscalerDepth: return "UpscalerDepth";
	case FrameTextureSlot::RrGuideDiffuseAlbedo: return "RrGuideDiffuseAlbedo";
	case FrameTextureSlot::RrGuideSpecularAlbedo: return "RrGuideSpecularAlbedo";
	case FrameTextureSlot::RrGuideSpecularHitDistance: return "RrGuideSpecularHitDistance";
	case FrameTextureSlot::RrGuideNormalRoughness: return "RrGuideNormalRoughness";
	case FrameTextureSlot::VendorOutput: return "VendorOutput";
	case FrameTextureSlot::PostSharpenOutput: return "PostSharpenOutput";
	case FrameTextureSlot::Final: return "Final";
	case FrameTextureSlot::Count: return "Count";
	default: return "Unknown";
	}
}

NRIMainUpscalerKind NRIRenderer::GetSelectedMainUpscalerKind() const
{
	SyncLegacyUpscalerConfig(false);
	switch ((int)nri_upscaler)
	{
	default:
	case 0: return NRIMainUpscalerKind::Off;
	case 2: return NRIMainUpscalerKind::DLSR;
	case 3: return NRIMainUpscalerKind::DLRR;
	}
}

NRIPostSharpenKind NRIRenderer::GetSelectedPostSharpenKind() const
{
	SyncLegacyUpscalerConfig(false);
	switch ((int)nri_postsharpen)
	{
	default:
	case 0: return NRIPostSharpenKind::Off;
	case 1: return NRIPostSharpenKind::NIS;
	}
}

NRIMainUpscalerKind NRIRenderer::GetResolvedMainUpscalerKindForStatus() const
{
	const NRIMainUpscalerKind requested = GetSelectedMainUpscalerKind();

	switch (requested)
	{
	case NRIMainUpscalerKind::DLRR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR))
		{
			return
				IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR) ? NRIMainUpscalerKind::DLSR :
				NRIMainUpscalerKind::Off;
		}
		break;

	case NRIMainUpscalerKind::DLSR:
		if (!IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR))
		{
			return NRIMainUpscalerKind::Off;
		}
		break;

	default:
		break;
	}

	return requested;
}

NRIPostSharpenKind NRIRenderer::GetResolvedPostSharpenKindForStatus() const
{
	const NRIPostSharpenKind requested = GetSelectedPostSharpenKind();
	if (requested == NRIPostSharpenKind::NIS && !IsPostSharpenSupported(NRIPostSharpenKind::NIS))
	{
		return NRIPostSharpenKind::Off;
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
