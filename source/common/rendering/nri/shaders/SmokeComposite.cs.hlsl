#include "Include/SmokeResources.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gSmokeConstants.OutputWidth || dispatchThreadId.y >= gSmokeConstants.OutputHeight)
		return;

	const uint2 outputPixel = dispatchThreadId.xy;
	const float2 uv = (float2(outputPixel) + 0.5) / float2(gSmokeConstants.OutputWidth, gSmokeConstants.OutputHeight);
	const uint2 inputPixel = min((uint2)(uv * float2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight)), uint2(gSmokeConstants.RenderWidth - 1u, gSmokeConstants.RenderHeight - 1u));
	const uint2 froxelColumn = min((uint2)(uv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight)), uint2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	const float viewDepth = min(max(gSmokeViewZInput.Load(int3(inputPixel, 0)).x, 0.0), gSmokeConstants.FroxelMaxDistance);
	const uint depthSlice = SmokeDepthSlice(viewDepth);
	const float4 medium = gSmokeFroxelIntegrated[SmokeFroxelIndex(froxelColumn.x, froxelColumn.y, depthSlice)];
	const float4 scene = gSmokeSceneInput.Load(int3(inputPixel, 0));

	float3 color = scene.rgb * saturate(medium.a) + max(medium.rgb, 0.0);
	if (gSmokeConstants.DebugMode == 1u)
		color = medium.aaa;
	else if (gSmokeConstants.DebugMode == 2u)
		color = medium.rgb;
	gSmokeOutput[outputPixel] = float4(color, scene.a);
}
