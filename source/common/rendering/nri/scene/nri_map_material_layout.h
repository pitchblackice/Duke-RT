#pragma once

#include "nri_scene_surface_types.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
struct PTMapSurface;

enum class PTMapMaterialLayoutFailure : uint8_t
{
	None = 0,
	EmptyInput,
	InvalidSurfaceReference,
	UnsupportedSurface,
	MissingSemanticSlot,
	InvalidSemanticSlot,
	DuplicateSemanticSlot,
	AmbiguousSemanticSlot,
	InvalidEmittedSlot,
	DuplicateEmittedSlot,
	MissingEmittedSlot,
	MissingMaterialIdentity,
	NonFiniteMaterialState,
	LayoutMismatch,
};

struct PTMapMaterialSemanticSlot
{
	uint32_t sourceType = 0;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	uint32_t surfaceKind = UINT32_MAX;
	uint32_t keyPrimary = UINT32_MAX;
	uint32_t keySecondary = UINT32_MAX;
};

bool operator==(const PTMapMaterialSemanticSlot& a, const PTMapMaterialSemanticSlot& b);

struct PTMapMaterialSurfaceSlot
{
	const PTMapSurface* surface = nullptr;
	uint32_t emittedMaterialSlot = UINT32_MAX;
};

// Lightweight equivalent used by standalone validation and by callers that
// already own a semantic surface view.
struct PTMapMaterialSurfaceView
{
	const SurfaceRef* surface = nullptr;
	uint32_t surfaceKind = UINT32_MAX;
	uint32_t keyPrimary = UINT32_MAX;
	uint32_t keySecondary = UINT32_MAX;
	uint32_t chunkIndex = UINT32_MAX;
	uint32_t emittedMaterialSlot = UINT32_MAX;
};

using PTMapMaterialTextureIdentityResolver = bool (*)(
	const FGameTexture* texture,
	uint64_t& outStableIdentity,
	void* userData);

struct PTMapMaterialLayoutOptions
{
	PTMapMaterialTextureIdentityResolver resolveTextureIdentity = nullptr;
	void* textureIdentityUserData = nullptr;
	// UINT32_MAX means one emitted material slot per supplied surface.
	uint32_t emittedMaterialCount = UINT32_MAX;
};

struct PTMapMaterialLayoutValidation
{
	bool valid = false;
	PTMapMaterialLayoutFailure failure = PTMapMaterialLayoutFailure::None;
	uint32_t surfaceIndex = UINT32_MAX;
	uint32_t emittedMaterialSlot = UINT32_MAX;
	uint32_t canonicalMaterialSlot = UINT32_MAX;
};

struct CanonicalPTMapMaterialLayout
{
	bool valid = false;
	uint32_t chunkIndex = UINT32_MAX;
	uint64_t layoutKey = 0;
	uint64_t stateKey = 0;
	std::vector<uint64_t> canonicalSlotIds;
	std::vector<PTMapMaterialSemanticSlot> canonicalSemanticSlots;
	std::vector<uint64_t> canonicalMaterialStateKeys;
	// Both remaps contain dense zero-based slot indices. emittedToCanonical is
	// indexed by the material slot produced by BuildGeometry.
	std::vector<uint32_t> emittedToCanonical;
	std::vector<uint32_t> canonicalToEmitted;
};

bool BuildCanonicalPTMapMaterialLayout(
	const std::vector<PTMapMaterialSurfaceSlot>& surfaces,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialLayoutValidation& outValidation,
	const PTMapMaterialLayoutOptions& options = {});

bool BuildCanonicalPTMapMaterialLayoutFromViews(
	const std::vector<PTMapMaterialSurfaceView>& surfaces,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialLayoutValidation& outValidation,
	const PTMapMaterialLayoutOptions& options = {});

// Compatibility is an exact canonical slot-set comparison. Matching material
// counts or matching 64-bit layout hashes alone are intentionally insufficient.
bool ValidatePTMapMaterialLayoutCompatibility(
	const CanonicalPTMapMaterialLayout& retained,
	const CanonicalPTMapMaterialLayout& current,
	PTMapMaterialLayoutValidation& outValidation);

const char* GetPTMapMaterialLayoutFailureName(PTMapMaterialLayoutFailure failure);
}
