#pragma once

#include "nri_map_movers.h"

#include "../scene/nri_map_material_state_variants.h"
#include "../scene/nri_map_material_variant_route.h"

#include <cstdint>

struct NRIMapMaterialOnlyRouteInput
{
	const NRIMapMoverSystem* movers = nullptr;
	const nri_scene::PTMapWorld* retainedWorld = nullptr;
	const nri_scene::PTMapChunk* retainedChunk = nullptr;
	const nri_scene::SceneView* retainedSceneView = nullptr;
	const nri_scene::CanonicalPTMapMaterialLayout* retainedLayout = nullptr;
	const nri_scene::PTMapWorld* currentWorld = nullptr;
	const nri_scene::PTMapChunk* currentChunk = nullptr;
	const nri_scene::SceneView* currentSceneView = nullptr;
	uint64_t buildSerial = 0;
	uint64_t mapEpoch = 0;
	uint64_t frameIndex = 0;
	bool allowAnimatedMaterialState = false;
};

enum class NRIMapMaterialOnlyRouteKind : uint8_t
{
	None = 0,
	TerminalSE12,
	AnimatedChunk,
};

enum class NRIMapMaterialOnlyRouteReject : uint8_t
{
	None = 0,
	InvalidInput,
	InvalidAuthority,
	RetainedLayout,
	CurrentLayout,
	LayoutMismatch,
	SceneRemap,
	GeometryMismatch,
	UnsupportedTexture,
	TerminalStateMismatch,
	VariantRejected,
};

enum class NRIMapMaterialOnlyRoutePreflightReject : uint8_t
{
	None = 0,
	LiveChunkNotPrepared,
	ResidentStateUnavailable,
	AtlasStateUnavailable,
};

struct NRIMapMaterialOnlyRouteResult
{
	bool admitted = false;
	NRIMapMaterialOnlyRouteKind kind = NRIMapMaterialOnlyRouteKind::None;
	NRIMapMaterialOnlyRouteReject reject = NRIMapMaterialOnlyRouteReject::None;
	uint64_t ownerStableId = UINT64_MAX;
	uint64_t layoutKey = 0;
	uint64_t stateKey = 0;
	nri_scene::PTMapMaterialStateVariantResult variant;
	nri_scene::CanonicalPTMapMaterialLayout retainedLayout;
	nri_scene::CanonicalPTMapMaterialLayout currentLayout;
	nri_scene::SceneView residentOrderSceneView;
	nri_scene::PTMapMaterialVariantRouteValidation validation;
};

struct NRIMapMaterialOnlyRouteFrameStats
{
	uint32_t candidates = 0;
	uint32_t admitted = 0;
	uint32_t terminalAdmissions = 0;
	uint32_t animatedAdmissions = 0;
	uint32_t variantHits = 0;
	uint32_t variantInserts = 0;
	uint32_t variantEvictions = 0;
	uint32_t recordEvictions = 0;
	uint64_t rejectMask = 0;
	uint64_t preflightRejectMask = 0;
	uint64_t validationFailureMask = 0;
};

class NRIMapMaterialOnlyRoute
{
public:
	void BeginFrame(uint64_t frameIndex);
	void NotePreflightReject(NRIMapMaterialOnlyRoutePreflightReject reason);
	NRIMapMaterialOnlyRouteResult TryPrepare(const NRIMapMaterialOnlyRouteInput& input);
	void Reset();

	const NRIMapMaterialOnlyRouteFrameStats& GetFrameStats() const { return mFrameStats; }
	const nri_scene::PTMapMaterialStateVariantStats& GetVariantStats() const { return mVariants.GetStats(); }

private:
	void Reject(NRIMapMaterialOnlyRouteResult& result, NRIMapMaterialOnlyRouteReject reason);

	uint64_t mFrameIndex = UINT64_MAX;
	NRIMapMaterialOnlyRouteFrameStats mFrameStats = {};
	nri_scene::PTMapMaterialStateVariantOwner mVariants;
};

const char* GetNRIMapMaterialOnlyRouteKindName(NRIMapMaterialOnlyRouteKind kind);
const char* GetNRIMapMaterialOnlyRouteRejectName(NRIMapMaterialOnlyRouteReject reject);
