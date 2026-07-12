#ifndef NRI_SMOKE_CONSTANTS_HLSLI
#define NRI_SMOKE_CONSTANTS_HLSLI

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
