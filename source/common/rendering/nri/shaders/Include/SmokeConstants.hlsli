#ifndef NRI_SMOKE_CONSTANTS_HLSLI
#define NRI_SMOKE_CONSTANTS_HLSLI

#define NRI_SMOKE_VISIBILITY_FILTERED_EFFECTIVE 0x1u
#define NRI_SMOKE_VISIBILITY_FILTERED_RESOURCES_READY 0x2u
#define NRI_SMOKE_VISIBILITY_TLAS_READY 0x4u
#define NRI_SMOKE_VISIBILITY_FILTERED_REQUESTED 0x8u

struct SmokeConstants
{
    uint Pass;
    uint FrameIndex;
    uint SimulationEpoch;
    uint ParticleCapacity;

    uint CommandCount;
    uint StyleCount;
    uint FroxelWidth;
    uint FroxelHeight;

    uint FroxelDepth;
    uint DirectionalColorPacked;
    uint RenderWidth;
    uint RenderHeight;

    uint OutputWidth;
    uint OutputHeight;
    uint DebugMode;
    uint Flags;

    float DeltaTime;
    float SimulationTime;
    float FroxelMaxDistance;
    float DepthExponent;

    float DensityScale;
    float RadianceScale;
    float TanHalfFovX;
    float TanHalfFovY;

    float3 CameraPosition;
    float TimeScale;

    float3 CameraForward;
    float DirectionalDirectionX;

    float3 CameraRight;
    float DirectionalDirectionY;

    float3 CameraUp;
    float DirectionalDirectionZ;

    float3 Wind;
    float DirectionalAngularSize;

    uint LightMode;
    uint LightSamples;
    uint MaxLightCandidates;
    uint RuntimeLightCount;

    uint RuntimeLightTileCountX;
    uint RuntimeLightTileCountY;
    uint LightSourceFlags;
    uint FilteredVisibilityEnabled;

    float2 CurrentJitter;
};

#endif
