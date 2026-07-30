#ifndef NRI_SMOKE_VIEW_WORK_DATA_HLSLI
#define NRI_SMOKE_VIEW_WORK_DATA_HLSLI

#define NRI_SMOKE_VIEW_TILE_AXIS 8u
#define NRI_SMOKE_VIEW_MASK_WORDS 2u
#define NRI_SMOKE_VIEW_MAX_DEPTH 64u

struct SmokeViewMask
{
	uint2 Words;
};

struct SmokeViewWorkControl
{
	uint FrameStamp;
	uint SimulationEpoch;
	uint BrickTileTests;
	uint ResidentBrickTileTests;
	uint OpticalCellTests;
	uint ContributingBrickTilePairs;
	uint ProjectedSpheres;
	uint ProjectedSpans;
	uint AttemptedMarks;
	uint DuplicateMerges;
	uint UniqueFroxels;
	uint UniqueColumns;
	uint Overflow;
	uint FalseNegatives;
	uint FalsePositives;
	uint TauErrorBits;
	uint OpacityErrorBits;
	uint RadianceErrorBits;
	uint BoundaryFalseNegatives;
	uint DenseContributing;
	uint OutputHashLo;
	uint OutputHashHi;
	uint EvaluationRoute;
	uint EvaluationDispatched;
	uint EvaluationSelected;
	uint EvaluationSkipped;
	uint NearPlaneSpans;
	uint CameraInsideSpans;
	uint BehindCameraRejects;
	uint OffscreenRejects;
	uint EmptyBrickTilePairs;
	uint CompactCount;
	uint PrefixColumns;
	uint ScatterWrites;
	uint CompactCapacity;
};

struct SmokeViewWorkConstants
{
	uint Pass;
	uint FrameIndex;
	uint SimulationEpoch;
	uint BrickCapacity;

	uint FroxelWidth;
	uint FroxelHeight;
	uint FroxelDepth;
	uint TileCountX;

	uint TileCountY;
	uint FieldPing;
	float CellSize;
	float OpticalThreshold;

	float FroxelMaxDistance;
	float DepthExponent;
	float TanHalfFovX;
	float TanHalfFovY;

	float3 CameraPosition;
	uint ExecutionRoute;

	float3 CameraForward;
	uint FroxelCapacity;

	float3 CameraRight;
	float Padding2;

	float3 CameraUp;
	float Padding3;
};

#endif
