#include "Include/SmokeResources.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.OutputWidth == 0u || gSmokeConstants.OutputHeight == 0u ||
		gSmokeConstants.RenderWidth == 0u || gSmokeConstants.RenderHeight == 0u ||
		gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;
	uint sceneWidth, sceneHeight, viewZWidth, viewZHeight, outputWidth, outputHeight;
	uint localFroxelCount, integratedFroxelCount, ignoredStride;
	gSmokeSceneInput.GetDimensions(sceneWidth, sceneHeight);
	gSmokeViewZInput.GetDimensions(viewZWidth, viewZHeight);
	gSmokeOutput.GetDimensions(outputWidth, outputHeight);
	gSmokeFroxelLocal.GetDimensions(localFroxelCount, ignoredStride);
	gSmokeFroxelIntegrated.GetDimensions(integratedFroxelCount, ignoredStride);
	const uint2 inputDimensions = min(uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight), min(uint2(sceneWidth, sceneHeight), uint2(viewZWidth, viewZHeight)));
	const uint2 outputDimensions = min(uint2(gSmokeConstants.OutputWidth, gSmokeConstants.OutputHeight), uint2(outputWidth, outputHeight));
	if (any(inputDimensions == 0u) || any(outputDimensions == 0u))
		return;

	if (dispatchThreadId.x >= outputDimensions.x || dispatchThreadId.y >= outputDimensions.y)
		return;

	const uint2 outputPixel = dispatchThreadId.xy;
	const float2 uv = (float2(outputPixel) + 0.5) / float2(outputDimensions);
	const uint2 inputPixel = min((uint2)(uv * float2(inputDimensions)), inputDimensions - 1u);
	const uint2 froxelColumn = min((uint2)(uv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight)), uint2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	const float viewDepth = min(max(gSmokeViewZInput.Load(int3(inputPixel, 0)).x, 0.0), gSmokeConstants.FroxelMaxDistance);
	const uint depthSlice = SmokeDepthSlice(viewDepth);
	const uint froxelIndex = SmokeFroxelIndex(froxelColumn.x, froxelColumn.y, depthSlice);
	const float4 scene = gSmokeSceneInput.Load(int3(inputPixel, 0));
	if (froxelIndex >= min(localFroxelCount, integratedFroxelCount))
	{
		gSmokeOutput[outputPixel] = scene;
		return;
	}
	const float4 medium = gSmokeFroxelIntegrated[froxelIndex];

	float3 color = scene.rgb * saturate(medium.a) + max(medium.rgb, 0.0);
	if (gSmokeConstants.DebugMode == 1u)
		color = (1.0 - saturate(medium.a)).xxx;
	else if (gSmokeConstants.DebugMode == 2u)
		color = saturate(medium.a).xxx;
	gSmokeOutput[outputPixel] = float4(color, scene.a);
}
