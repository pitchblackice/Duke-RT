#pragma once

#include <cstdint>
#include <vector>

static constexpr uint32_t NRI_SE29_FLOOR_DEFORMER_LOTAG = 29;
static constexpr uint32_t NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_CHUNKS = 1;
static constexpr uint32_t NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_PRIMITIVES = 512;
static constexpr uint64_t NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_UPLOAD_BYTES = 128ull * 1024ull;

enum NRISE29FloorDeformerRejectBits : uint32_t
{
	NRISE29FloorDeformerReject_None = 0,
	NRISE29FloorDeformerReject_InvalidIdentity = 1u << 0,
	NRISE29FloorDeformerReject_UnsupportedLane = 1u << 1,
	NRISE29FloorDeformerReject_InvalidLayout = 1u << 2,
	NRISE29FloorDeformerReject_IdentityChanged = 1u << 3,
	NRISE29FloorDeformerReject_MembershipChanged = 1u << 4,
	NRISE29FloorDeformerReject_CountChanged = 1u << 5,
	NRISE29FloorDeformerReject_TopologyChanged = 1u << 6,
	NRISE29FloorDeformerReject_OrderChanged = 1u << 7,
	NRISE29FloorDeformerReject_ProvenanceChanged = 1u << 8,
	NRISE29FloorDeformerReject_MaterialSlotsChanged = 1u << 9,
	NRISE29FloorDeformerReject_UnsupportedMutableField = 1u << 10,
	NRISE29FloorDeformerReject_SmoothNormals = 1u << 11,
	NRISE29FloorDeformerReject_InvalidStamp = 1u << 12,
	NRISE29FloorDeformerReject_StaleEpoch = 1u << 13,
	NRISE29FloorDeformerReject_StaleDependency = 1u << 14,
	NRISE29FloorDeformerReject_StaleGeneration = 1u << 15,
	NRISE29FloorDeformerReject_InvalidDirtySpans = 1u << 16,
	NRISE29FloorDeformerReject_ChunkBudget = 1u << 17,
	NRISE29FloorDeformerReject_PrimitiveBudget = 1u << 18,
	NRISE29FloorDeformerReject_UploadBudget = 1u << 19,
};

enum NRISE29FloorDeformerVertexMutableBits : uint32_t
{
	NRISE29FloorDeformerVertexMutable_None = 0,
	NRISE29FloorDeformerVertexMutable_Position = 1u << 0,
	NRISE29FloorDeformerVertexMutable_PreviousPosition = 1u << 1,
	NRISE29FloorDeformerVertexMutable_TextureCoordinates = 1u << 2,
};

enum NRISE29FloorDeformerPrimitiveMutableBits : uint32_t
{
	NRISE29FloorDeformerPrimitiveMutable_None = 0,
	NRISE29FloorDeformerPrimitiveMutable_TextureCoordinates = 1u << 0,
	NRISE29FloorDeformerPrimitiveMutable_GeometricNormal = 1u << 1,
	NRISE29FloorDeformerPrimitiveMutable_SmoothNormals = 1u << 2,
	NRISE29FloorDeformerPrimitiveMutable_MaterialIndex = 1u << 3,
	NRISE29FloorDeformerPrimitiveMutable_Flags = 1u << 4,
	NRISE29FloorDeformerPrimitiveMutable_PortalIndex = 1u << 5,
};

struct NRISE29FloorDeformerIdentity
{
	uint64_t stableKey = 0;
	uint32_t chunkIndex = UINT32_MAX;
};

bool operator==(
	const NRISE29FloorDeformerIdentity& a,
	const NRISE29FloorDeformerIdentity& b);

struct NRISE29FloorDeformerLayoutFingerprint
{
	NRISE29FloorDeformerIdentity identity;
	uint32_t memberCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveCount = 0;
	uint64_t membershipFingerprint = 0;
	uint64_t topologyFingerprint = 0;
	uint64_t vertexOrderFingerprint = 0;
	uint64_t indexOrderFingerprint = 0;
	uint64_t primitiveOrderFingerprint = 0;
	uint64_t primitiveProvenanceFingerprint = 0;
	uint64_t materialSlotLayoutFingerprint = 0;
};

struct NRISE29FloorDeformerLayoutComparison
{
	bool compatible = false;
	uint32_t rejectMask = NRISE29FloorDeformerReject_None;
};

NRISE29FloorDeformerLayoutComparison CompareNRISE29FloorDeformerLayouts(
	const NRISE29FloorDeformerLayoutFingerprint& resident,
	const NRISE29FloorDeformerLayoutFingerprint& current);

struct NRISE29FloorDeformerDependencyStamp
{
	uint64_t mapEpoch = 0;
	uint64_t authorityGeneration = 0;
	uint64_t dependencyGeneration = 0;
	uint64_t topologyGeneration = 0;
	uint64_t membershipGeneration = 0;
	uint64_t geometryGeneration = 0;
	uint64_t materialSlotGeneration = 0;
};

struct NRISE29FloorDeformerDirtySpan
{
	uint32_t firstElement = 0;
	uint32_t elementCount = 0;
};

struct NRISE29FloorDeformerDirtySpanPlan
{
	bool valid = false;
	uint32_t dirtyElementCount = 0;
	std::vector<NRISE29FloorDeformerDirtySpan> spans;
};

NRISE29FloorDeformerDirtySpanPlan CoalesceNRISE29FloorDeformerDirtySpans(
	const std::vector<NRISE29FloorDeformerDirtySpan>& spans,
	uint32_t elementCapacity);

struct NRISE29FloorDeformerPendingWork
{
	NRISE29FloorDeformerLayoutFingerprint residentLayout;
	NRISE29FloorDeformerLayoutFingerprint currentLayout;
	NRISE29FloorDeformerDependencyStamp capturedStamp;
	NRISE29FloorDeformerDependencyStamp currentStamp;
	uint64_t pendingSinceFrame = 0;
	uint32_t effectorLotag = 0;
	bool floorPlaneOnly = false;
	bool rayVisible = false;
	bool required = false;
	bool usesSmoothNormals = false;
	uint32_t vertexMutableFieldMask = NRISE29FloorDeformerVertexMutable_None;
	uint32_t primitiveMutableFieldMask = NRISE29FloorDeformerPrimitiveMutable_None;
	uint32_t vertexStrideBytes = 0;
	uint32_t primitiveStrideBytes = 0;
	std::vector<NRISE29FloorDeformerDirtySpan> vertexDirtySpans;
	std::vector<NRISE29FloorDeformerDirtySpan> primitiveDirtySpans;
};

enum class NRISE29FloorDeformerPendingUpdate : uint8_t
{
	Invalid = 0,
	Added,
	ReplacedLatest,
	DuplicateIgnored,
	StaleIgnored,
};

class NRISE29FloorDeformerPendingSet
{
public:
	NRISE29FloorDeformerPendingUpdate QueueLatest(
		const NRISE29FloorDeformerPendingWork& work);
	bool Remove(const NRISE29FloorDeformerIdentity& identity);
	void Clear();

	const std::vector<NRISE29FloorDeformerPendingWork>& Items() const { return m_items; }
	uint32_t Size() const { return static_cast<uint32_t>(m_items.size()); }

private:
	std::vector<NRISE29FloorDeformerPendingWork> m_items;
};

struct NRISE29FloorDeformerBudget
{
	uint32_t maxChunks = NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_CHUNKS;
	uint32_t maxPrimitives = NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_PRIMITIVES;
	uint64_t maxUploadBytes = NRI_SE29_FLOOR_DEFORMER_DEFAULT_MAX_UPLOAD_BYTES;
};

enum class NRISE29FloorDeformerAction : uint8_t
{
	ExactFallback = 0,
	Refit,
};

struct NRISE29FloorDeformerDecision
{
	NRISE29FloorDeformerIdentity identity;
	NRISE29FloorDeformerAction action = NRISE29FloorDeformerAction::ExactFallback;
	uint32_t rejectMask = NRISE29FloorDeformerReject_None;
	uint32_t primitiveCost = 0;
	uint64_t uploadBytes = 0;
	NRISE29FloorDeformerDirtySpanPlan vertexPlan;
	NRISE29FloorDeformerDirtySpanPlan primitivePlan;
};

struct NRISE29FloorDeformerBatchPlan
{
	uint32_t admittedChunks = 0;
	uint32_t admittedPrimitives = 0;
	uint64_t admittedUploadBytes = 0;
	uint32_t exactFallbacks = 0;
	std::vector<NRISE29FloorDeformerDecision> decisions;
};

NRISE29FloorDeformerBatchPlan SelectNRISE29FloorDeformerWork(
	const std::vector<NRISE29FloorDeformerPendingWork>& pending,
	const NRISE29FloorDeformerBudget& budget = {});
