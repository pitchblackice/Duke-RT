#include "nri_map_material_variant_route.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace nri_scene
{
namespace
{
	bool IsFlatKind(PTMapSurfaceKind kind)
	{
		return kind == PTMapSurfaceKind::Floor || kind == PTMapSurfaceKind::Ceiling;
	}

	bool IsWallKind(uint32_t kind)
	{
		return kind >= (uint32_t)PTMapSurfaceKind::WallOneSided &&
			kind <= (uint32_t)PTMapSurfaceKind::WallLower;
	}

	bool IsSupportedKind(PTMapSurfaceKind kind)
	{
		return IsFlatKind(kind) || IsWallKind((uint32_t)kind);
	}

	void SetFailure(
		PTMapMaterialVariantRouteValidation& validation,
		PTMapMaterialVariantRouteFailure failure,
		uint32_t emittedSlot = UINT32_MAX,
		uint32_t authoredSurfaceIndex = UINT32_MAX,
		uint32_t canonicalSlot = UINT32_MAX)
	{
		validation = {};
		validation.failure = failure;
		validation.emittedMaterialSlot = emittedSlot;
		validation.authoredSurfaceIndex = authoredSurfaceIndex;
		validation.canonicalMaterialSlot = canonicalSlot;
	}

	bool SameAuthoredProvenance(const SurfaceProvenance& emitted, const SurfaceProvenance& authored)
	{
		return emitted.sourceType == authored.sourceType &&
			emitted.sectorIndex == authored.sectorIndex &&
			emitted.wallIndex == authored.wallIndex &&
			emitted.sectionIndex == authored.sectionIndex &&
			emitted.mapChunkIndex == authored.mapChunkIndex;
	}

	bool ValidateLayoutRemaps(const CanonicalPTMapMaterialLayout& layout)
	{
		const size_t slotCount = layout.canonicalSlotIds.size();
		if (!layout.valid || slotCount == 0 ||
			layout.canonicalSemanticSlots.size() != slotCount ||
			layout.canonicalMaterialStateKeys.size() != slotCount ||
			layout.emittedToCanonical.size() != slotCount ||
			layout.canonicalToEmitted.size() != slotCount)
		{
			return false;
		}
		std::vector<bool> emittedSlots(slotCount, false);
		for (uint32_t canonicalSlot = 0; canonicalSlot < (uint32_t)slotCount; ++canonicalSlot)
		{
			const uint32_t emittedSlot = layout.canonicalToEmitted[canonicalSlot];
			if (emittedSlot >= slotCount || emittedSlots[emittedSlot] ||
				layout.emittedToCanonical[emittedSlot] != canonicalSlot)
			{
				return false;
			}
			emittedSlots[emittedSlot] = true;
		}
		return true;
	}

	bool ValidateCompatibility(
		const CanonicalPTMapMaterialLayout& retainedLayout,
		const CanonicalPTMapMaterialLayout& currentLayout,
		PTMapMaterialVariantRouteValidation& validation)
	{
		if (!ValidateLayoutRemaps(retainedLayout) || !ValidateLayoutRemaps(currentLayout))
		{
			SetFailure(validation, PTMapMaterialVariantRouteFailure::InvalidLayoutRemap);
			return false;
		}
		if (retainedLayout.chunkIndex == UINT32_MAX || retainedLayout.chunkIndex != currentLayout.chunkIndex)
		{
			SetFailure(validation, PTMapMaterialVariantRouteFailure::LayoutMismatch);
			return false;
		}
		PTMapMaterialLayoutValidation layoutValidation;
		if (!ValidatePTMapMaterialLayoutCompatibility(retainedLayout, currentLayout, layoutValidation))
		{
			SetFailure(validation, PTMapMaterialVariantRouteFailure::LayoutMismatch,
				UINT32_MAX, UINT32_MAX, layoutValidation.canonicalMaterialSlot);
			validation.layoutValidation = layoutValidation;
			return false;
		}
		return true;
	}

	const SurfaceRef* GetEmittedSurface(const SceneView& view, uint32_t emittedSlot)
	{
		if (emittedSlot < view.opaqueWalls.size())
		{
			return &view.opaqueWalls[emittedSlot];
		}
		const uint32_t flatSlot = emittedSlot - (uint32_t)view.opaqueWalls.size();
		return flatSlot < view.opaqueFlats.size() ? &view.opaqueFlats[flatSlot] : nullptr;
	}

	bool SameProvenance(const SurfaceProvenance& retained, const SurfaceProvenance& current)
	{
		if (retained.sourceType != current.sourceType ||
			retained.sectorIndex != current.sectorIndex ||
			retained.wallIndex != current.wallIndex ||
			retained.sectionIndex != current.sectionIndex ||
			retained.mapChunkIndex != current.mapChunkIndex ||
			retained.nextSectorIndex != current.nextSectorIndex ||
			retained.actorIndex != current.actorIndex ||
			retained.drawListType != current.drawListType ||
			retained.cstat != current.cstat ||
			retained.materialFlags != current.materialFlags ||
			retained.actorOverlayRuleCount != current.actorOverlayRuleCount)
		{
			return false;
		}
		for (uint32_t rule = 0; rule < MaxActorOverlayRuleIdsPerSurface; ++rule)
		{
			if (retained.actorOverlayRuleIds[rule] != current.actorOverlayRuleIds[rule])
			{
				return false;
			}
		}
		return true;
	}

	bool SamePrimitiveLayout(const SurfaceRef& retained, const SurfaceRef& current)
	{
		if (retained.material.flags != current.material.flags ||
			!SameProvenance(retained.provenance, current.provenance) ||
			retained.vertices.size() != current.vertices.size() ||
			retained.indices != current.indices)
		{
			return false;
		}
		for (size_t vertex = 0; vertex < retained.vertices.size(); ++vertex)
		{
			const CapturedVertex& a = retained.vertices[vertex];
			const CapturedVertex& b = current.vertices[vertex];
			if (std::memcmp(a.position, b.position, sizeof(a.position)) != 0 ||
				std::memcmp(a.prevPosition, b.prevPosition, sizeof(a.prevPosition)) != 0 ||
				std::memcmp(a.uv, b.uv, sizeof(a.uv)) != 0)
			{
				return false;
			}
		}
		return true;
	}
}

bool BuildCanonicalPTMapMaterialLayoutForChunkSceneView(
	const PTMapWorld& mapWorld,
	const PTMapChunk& chunk,
	const SceneView& sceneView,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialVariantRouteValidation& outValidation,
	const PTMapMaterialLayoutOptions& options)
{
	outLayout = {};
	outValidation = {};
	const uint64_t endSurface64 = (uint64_t)chunk.firstSurface + chunk.surfaceCount;
	if (chunk.chunkIndex == UINT32_MAX || chunk.surfaceCount == 0 ||
		endSurface64 > mapWorld.surfaces.size())
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::InvalidChunk);
		return false;
	}
	if (!sceneView.opaqueSprites.empty())
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::UnsupportedSceneView);
		return false;
	}

	const uint32_t firstSurface = chunk.firstSurface;
	const uint32_t endSurface = (uint32_t)endSurface64;
	uint32_t authoredWallCount = 0;
	uint32_t authoredFlatCount = 0;
	std::vector<bool> authoredEligible(chunk.surfaceCount, false);
	for (uint32_t surfaceIndex = firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
	{
		const PTMapSurface& authored = mapWorld.surfaces[surfaceIndex];
		const uint32_t unsupportedFlags = MaterialFlag_Mirror | MaterialFlag_Sky |
			MaterialFlag_Portal | MaterialFlag_PlainMirror;
		if (authored.chunkIndex != chunk.chunkIndex)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::UnsupportedAuthoredSurface,
				UINT32_MAX, surfaceIndex);
			return false;
		}
		// Portal/sky/mirror surfaces are not material rows in the opaque resident
		// SceneView. Exclude them from the canonical material domain, but continue
		// to require exact ownership/provenance for every authored chunk surface.
		if (!IsSupportedKind(authored.kind) ||
			(authored.surface.material.flags & unsupportedFlags) != 0)
		{
			continue;
		}
		if (authored.surface.provenance.mapChunkIndex != (int32_t)chunk.chunkIndex)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::InvalidProvenance,
				UINT32_MAX, surfaceIndex);
			return false;
		}
		authoredEligible[surfaceIndex - firstSurface] = true;
		if (IsFlatKind(authored.kind))
		{
			authoredFlatCount++;
		}
		else
		{
			authoredWallCount++;
		}
	}
	if (sceneView.opaqueWalls.size() != authoredWallCount ||
		sceneView.opaqueFlats.size() != authoredFlatCount)
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::IncompleteSceneView);
		return false;
	}

	const uint32_t emittedCount = authoredWallCount + authoredFlatCount;
	std::vector<bool> authoredUsed(chunk.surfaceCount, false);
	std::vector<PTMapMaterialSurfaceView> views;
	views.reserve(emittedCount);
	for (uint32_t emittedSlot = 0; emittedSlot < emittedCount; ++emittedSlot)
	{
		const SurfaceRef* emitted = GetEmittedSurface(sceneView, emittedSlot);
		if (emitted == nullptr || emitted->provenance.mapChunkIndex != (int32_t)chunk.chunkIndex)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::InvalidProvenance, emittedSlot);
			return false;
		}

		uint32_t matchingSurfaceIndex = UINT32_MAX;
		uint32_t matchCount = 0;
		for (uint32_t surfaceIndex = firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (!authoredEligible[surfaceIndex - firstSurface])
			{
				continue;
			}
			const PTMapSurface& authored = mapWorld.surfaces[surfaceIndex];
			if (SameAuthoredProvenance(emitted->provenance, authored.surface.provenance))
			{
				matchingSurfaceIndex = surfaceIndex;
				matchCount++;
			}
		}
		if (matchCount == 0)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::MissingAuthoredSurface, emittedSlot);
			return false;
		}
		if (matchCount != 1)
		{
			uint32_t geometryMatchCount = 0;
			for (uint32_t surfaceIndex = firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
			{
				if (!authoredEligible[surfaceIndex - firstSurface])
				{
					continue;
				}
				const PTMapSurface& authored = mapWorld.surfaces[surfaceIndex];
				if (SameAuthoredProvenance(emitted->provenance, authored.surface.provenance) &&
					SamePrimitiveLayout(authored.surface, *emitted))
				{
					matchingSurfaceIndex = surfaceIndex;
					geometryMatchCount++;
				}
			}
			if (geometryMatchCount != 1)
			{
				SetFailure(outValidation, PTMapMaterialVariantRouteFailure::AmbiguousAuthoredSurface,
					emittedSlot, matchingSurfaceIndex);
				return false;
			}
		}
		const uint32_t authoredOffset = matchingSurfaceIndex - firstSurface;
		if (authoredUsed[authoredOffset])
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::DuplicateEmittedSurface,
				emittedSlot, matchingSurfaceIndex);
			return false;
		}
		authoredUsed[authoredOffset] = true;

		const PTMapSurface& authored = mapWorld.surfaces[matchingSurfaceIndex];
		const bool emittedIsFlat = emittedSlot >= authoredWallCount;
		if (emittedIsFlat != IsFlatKind(authored.kind))
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewClassMismatch,
				emittedSlot, matchingSurfaceIndex);
			return false;
		}
		PTMapMaterialSurfaceView view = {};
		view.surface = emitted;
		view.surfaceKind = (uint32_t)authored.kind;
		view.keyPrimary = authored.key.primary;
		view.keySecondary = authored.key.secondary;
		view.chunkIndex = authored.chunkIndex;
		view.emittedMaterialSlot = emittedSlot;
		views.push_back(view);
	}
	for (uint32_t surfaceOffset = 0; surfaceOffset < chunk.surfaceCount; ++surfaceOffset)
	{
		if (authoredEligible[surfaceOffset] && !authoredUsed[surfaceOffset])
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::IncompleteSceneView);
			return false;
		}
	}

	PTMapMaterialLayoutOptions exactOptions = options;
	exactOptions.emittedMaterialCount = emittedCount;
	PTMapMaterialLayoutValidation layoutValidation;
	if (!BuildCanonicalPTMapMaterialLayoutFromViews(views, outLayout, layoutValidation, exactOptions))
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::MaterialLayoutRejected,
			layoutValidation.emittedMaterialSlot, UINT32_MAX, layoutValidation.canonicalMaterialSlot);
		outValidation.layoutValidation = layoutValidation;
		return false;
	}
	outValidation.valid = true;
	outValidation.layoutValidation = layoutValidation;
	return true;
}

bool RemapPTMapMaterialBridgeToRetainedLayout(
	const CanonicalPTMapMaterialLayout& retainedLayout,
	const CanonicalPTMapMaterialLayout& currentLayout,
	const MaterialBridgeData& currentMaterials,
	MaterialBridgeData& outMaterials,
	PTMapMaterialVariantRouteValidation& outValidation)
{
	outValidation = {};
	if (!ValidateCompatibility(retainedLayout, currentLayout, outValidation))
	{
		return false;
	}
	const uint32_t slotCount = (uint32_t)currentLayout.canonicalSlotIds.size();
	if (currentMaterials.materials.size() != slotCount || currentMaterials.lightMetadata.size() != slotCount)
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::MaterialBridgeCountMismatch);
		return false;
	}

	MaterialBridgeData remapped = currentMaterials;
	for (uint32_t canonicalSlot = 0; canonicalSlot < slotCount; ++canonicalSlot)
	{
		const uint32_t sourceSlot = currentLayout.canonicalToEmitted[canonicalSlot];
		const uint32_t destinationSlot = retainedLayout.canonicalToEmitted[canonicalSlot];
		remapped.materials[destinationSlot] = currentMaterials.materials[sourceSlot];
		remapped.lightMetadata[destinationSlot] = currentMaterials.lightMetadata[sourceSlot];
	}
	outMaterials = std::move(remapped);
	outValidation.valid = true;
	return true;
}

bool ValidatePTMapMaterialVariantGeometryLayout(
	const SceneView& retainedSceneView,
	const SceneView& remappedCurrentSceneView,
	PTMapMaterialVariantRouteValidation& outValidation)
{
	outValidation = {};
	if (!retainedSceneView.opaqueSprites.empty() || !remappedCurrentSceneView.opaqueSprites.empty() ||
		retainedSceneView.opaqueWalls.size() != remappedCurrentSceneView.opaqueWalls.size() ||
		retainedSceneView.opaqueFlats.size() != remappedCurrentSceneView.opaqueFlats.size() ||
		retainedSceneView.primitiveFlags != remappedCurrentSceneView.primitiveFlags)
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::GeometryLayoutMismatch);
		return false;
	}
	uint32_t emittedSlot = 0;
	for (size_t wallIndex = 0; wallIndex < retainedSceneView.opaqueWalls.size(); ++wallIndex, ++emittedSlot)
	{
		if (!SamePrimitiveLayout(retainedSceneView.opaqueWalls[wallIndex], remappedCurrentSceneView.opaqueWalls[wallIndex]))
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::GeometryLayoutMismatch, emittedSlot);
			return false;
		}
	}
	for (size_t flatIndex = 0; flatIndex < retainedSceneView.opaqueFlats.size(); ++flatIndex, ++emittedSlot)
	{
		if (!SamePrimitiveLayout(retainedSceneView.opaqueFlats[flatIndex], remappedCurrentSceneView.opaqueFlats[flatIndex]))
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::GeometryLayoutMismatch, emittedSlot);
			return false;
		}
	}
	outValidation.valid = true;
	return true;
}

bool RemapPTMapMaterialSceneViewToRetainedLayout(
	const CanonicalPTMapMaterialLayout& retainedLayout,
	const CanonicalPTMapMaterialLayout& currentLayout,
	const SceneView& currentSceneView,
	SceneView& outSceneView,
	PTMapMaterialVariantRouteValidation& outValidation)
{
	outValidation = {};
	if (!ValidateCompatibility(retainedLayout, currentLayout, outValidation))
	{
		return false;
	}
	const uint32_t slotCount = (uint32_t)currentLayout.canonicalSlotIds.size();
	if (!currentSceneView.opaqueSprites.empty() ||
		currentSceneView.opaqueWalls.size() + currentSceneView.opaqueFlats.size() != slotCount)
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewCountMismatch);
		return false;
	}

	uint32_t wallCount = 0;
	for (const PTMapMaterialSemanticSlot& semantic : retainedLayout.canonicalSemanticSlots)
	{
		if (IsWallKind(semantic.surfaceKind))
		{
			wallCount++;
		}
		else if (semantic.surfaceKind != (uint32_t)PTMapSurfaceKind::Floor &&
			semantic.surfaceKind != (uint32_t)PTMapSurfaceKind::Ceiling)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewClassMismatch);
			return false;
		}
	}
	if (currentSceneView.opaqueWalls.size() != wallCount ||
		currentSceneView.opaqueFlats.size() != slotCount - wallCount)
	{
		SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewCountMismatch);
		return false;
	}

	SceneView remapped = currentSceneView;
	remapped.opaqueWalls.resize(wallCount);
	remapped.opaqueFlats.resize(slotCount - wallCount);
	for (uint32_t canonicalSlot = 0; canonicalSlot < slotCount; ++canonicalSlot)
	{
		const bool semanticIsWall = IsWallKind(retainedLayout.canonicalSemanticSlots[canonicalSlot].surfaceKind);
		const uint32_t sourceSlot = currentLayout.canonicalToEmitted[canonicalSlot];
		const uint32_t destinationSlot = retainedLayout.canonicalToEmitted[canonicalSlot];
		if ((sourceSlot < wallCount) != semanticIsWall ||
			(destinationSlot < wallCount) != semanticIsWall)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewClassMismatch,
				sourceSlot, UINT32_MAX, canonicalSlot);
			return false;
		}
		const SurfaceRef* source = GetEmittedSurface(currentSceneView, sourceSlot);
		if (source == nullptr)
		{
			SetFailure(outValidation, PTMapMaterialVariantRouteFailure::SceneViewCountMismatch,
				sourceSlot, UINT32_MAX, canonicalSlot);
			return false;
		}
		if (destinationSlot < wallCount)
		{
			remapped.opaqueWalls[destinationSlot] = *source;
		}
		else
		{
			remapped.opaqueFlats[destinationSlot - wallCount] = *source;
		}
	}
	outSceneView = std::move(remapped);
	outValidation.valid = true;
	return true;
}

const char* GetPTMapMaterialVariantRouteFailureName(PTMapMaterialVariantRouteFailure failure)
{
	switch (failure)
	{
	case PTMapMaterialVariantRouteFailure::None: return "none";
	case PTMapMaterialVariantRouteFailure::InvalidChunk: return "invalid-chunk";
	case PTMapMaterialVariantRouteFailure::UnsupportedSceneView: return "unsupported-scene-view";
	case PTMapMaterialVariantRouteFailure::UnsupportedAuthoredSurface: return "unsupported-authored-surface";
	case PTMapMaterialVariantRouteFailure::IncompleteSceneView: return "incomplete-scene-view";
	case PTMapMaterialVariantRouteFailure::InvalidProvenance: return "invalid-provenance";
	case PTMapMaterialVariantRouteFailure::MissingAuthoredSurface: return "missing-authored-surface";
	case PTMapMaterialVariantRouteFailure::DuplicateEmittedSurface: return "duplicate-emitted-surface";
	case PTMapMaterialVariantRouteFailure::AmbiguousAuthoredSurface: return "ambiguous-authored-surface";
	case PTMapMaterialVariantRouteFailure::MaterialLayoutRejected: return "material-layout-rejected";
	case PTMapMaterialVariantRouteFailure::LayoutMismatch: return "layout-mismatch";
	case PTMapMaterialVariantRouteFailure::InvalidLayoutRemap: return "invalid-layout-remap";
	case PTMapMaterialVariantRouteFailure::MaterialBridgeCountMismatch: return "material-bridge-count-mismatch";
	case PTMapMaterialVariantRouteFailure::SceneViewCountMismatch: return "scene-view-count-mismatch";
	case PTMapMaterialVariantRouteFailure::SceneViewClassMismatch: return "scene-view-class-mismatch";
	case PTMapMaterialVariantRouteFailure::GeometryLayoutMismatch: return "geometry-layout-mismatch";
	}
	return "unknown";
}
}
