#pragma once

#include "nri_map_material_layout.h"
#include "nri_map_world.h"
#include "nri_material_bridge.h"

#include <cstdint>

namespace nri_scene
{
enum class PTMapMaterialVariantRouteFailure : uint8_t
{
	None = 0,
	InvalidChunk,
	UnsupportedSceneView,
	UnsupportedAuthoredSurface,
	IncompleteSceneView,
	InvalidProvenance,
	MissingAuthoredSurface,
	DuplicateEmittedSurface,
	AmbiguousAuthoredSurface,
	MaterialLayoutRejected,
	LayoutMismatch,
	InvalidLayoutRemap,
	MaterialBridgeCountMismatch,
	SceneViewCountMismatch,
	SceneViewClassMismatch,
	GeometryLayoutMismatch,
};

struct PTMapMaterialVariantRouteValidation
{
	bool valid = false;
	PTMapMaterialVariantRouteFailure failure = PTMapMaterialVariantRouteFailure::None;
	uint32_t emittedMaterialSlot = UINT32_MAX;
	uint32_t authoredSurfaceIndex = UINT32_MAX;
	uint32_t canonicalMaterialSlot = UINT32_MAX;
	PTMapMaterialLayoutValidation layoutValidation;
};

// Builds the semantic/material layout for the complete opaque material view of
// one map chunk. Authored kind/key data supplies semantic identity while the
// emitted SceneView supplies current material state. Provenance matching is
// exact and one-to-one; unsupported or partial chunk views fail closed.
bool BuildCanonicalPTMapMaterialLayoutForChunkSceneView(
	const PTMapWorld& mapWorld,
	const PTMapChunk& chunk,
	const SceneView& sceneView,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialVariantRouteValidation& outValidation,
	const PTMapMaterialLayoutOptions& options = {});

// Reorders current material rows into the retained emitted-slot order after an
// exact semantic-layout compatibility check. Texture uploads and palette data
// are copied unchanged from currentMaterials.
bool RemapPTMapMaterialBridgeToRetainedLayout(
	const CanonicalPTMapMaterialLayout& retainedLayout,
	const CanonicalPTMapMaterialLayout& currentLayout,
	const MaterialBridgeData& currentMaterials,
	MaterialBridgeData& outMaterials,
	PTMapMaterialVariantRouteValidation& outValidation);

// Reorders the current chunk SceneView into the retained emitted-slot order so
// material rows, light views, and surface signatures stay aligned. Wall and
// flat groups must remain exactly compatible with BuildGeometry slot order.
bool RemapPTMapMaterialSceneViewToRetainedLayout(
	const CanonicalPTMapMaterialLayout& retainedLayout,
	const CanonicalPTMapMaterialLayout& currentLayout,
	const SceneView& currentSceneView,
	SceneView& outSceneView,
	PTMapMaterialVariantRouteValidation& outValidation);

// Proves that a remapped current view has the exact resident-order primitive
// layout of the retained view without rebuilding GeometryData. Only MaterialRef
// state may differ; primitive flags, provenance, vertices, and indices must be
// identical, including material/provenance flags that affect geometry payloads.
bool ValidatePTMapMaterialVariantGeometryLayout(
	const SceneView& retainedSceneView,
	const SceneView& remappedCurrentSceneView,
	PTMapMaterialVariantRouteValidation& outValidation);

const char* GetPTMapMaterialVariantRouteFailureName(PTMapMaterialVariantRouteFailure failure);
}
