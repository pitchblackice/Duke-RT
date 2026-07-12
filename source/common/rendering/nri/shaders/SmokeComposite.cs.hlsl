#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

struct SmokeBilinearFootprint
{
	uint2 p00;
	uint2 p10;
	uint2 p01;
	uint2 p11;
	float2 blend;
};

SmokeBilinearFootprint SmokeMakeBilinearFootprint(float2 stableUv)
{
	const float2 coordinate = SmokeFroxelCoordinate(stableUv);
	const int2 base = int2(floor(coordinate));
	const int2 maximum = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	const int2 p0 = clamp(base, int2(0, 0), maximum);
	const int2 p1 = clamp(base + 1, int2(0, 0), maximum);
	SmokeBilinearFootprint footprint;
	footprint.p00 = uint2(p0.x, p0.y);
	footprint.p10 = uint2(p1.x, p0.y);
	footprint.p01 = uint2(p0.x, p1.y);
	footprint.p11 = uint2(p1.x, p1.y);
	footprint.blend = frac(coordinate);
	return footprint;
}

float4 SmokeBilinearMedium(SmokeBilinearFootprint footprint, uint z)
{
	const float4 row0 = lerp(
		gSmokeFroxelMedium[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, z)],
		gSmokeFroxelMedium[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, z)], footprint.blend.x);
	const float4 row1 = lerp(
		gSmokeFroxelMedium[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, z)],
		gSmokeFroxelMedium[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, z)], footprint.blend.x);
	return lerp(row0, row1, footprint.blend.y);
}

float3 SmokeBilinearSource(SmokeBilinearFootprint footprint, uint z)
{
	const float3 row0 = lerp(
		gSmokeFroxelSource[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, z)].rgb,
		gSmokeFroxelSource[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, z)].rgb, footprint.blend.x);
	const float3 row1 = lerp(
		gSmokeFroxelSource[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, z)].rgb,
		gSmokeFroxelSource[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, z)].rgb, footprint.blend.x);
	return lerp(row0, row1, footprint.blend.y);
}

float4 SmokeBilinearIntegrated(SmokeBilinearFootprint footprint, uint z)
{
	const float4 row0 = lerp(
		gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, z)],
		gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, z)], footprint.blend.x);
	const float4 row1 = lerp(
		gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, z)],
		gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, z)], footprint.blend.x);
	return lerp(row0, row1, footprint.blend.y);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.OutputWidth == 0u || gSmokeConstants.OutputHeight == 0u ||
		gSmokeConstants.RenderWidth == 0u || gSmokeConstants.RenderHeight == 0u ||
		gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;
	uint sceneWidth, sceneHeight, viewZWidth, viewZHeight, outputWidth, outputHeight;
	uint mediumFroxelCount, sourceFroxelCount, integratedFroxelCount, ignoredStride;
	gSmokeSceneInput.GetDimensions(sceneWidth, sceneHeight);
	gSmokeViewZInput.GetDimensions(viewZWidth, viewZHeight);
	gSmokeOutput.GetDimensions(outputWidth, outputHeight);
	gSmokeFroxelMedium.GetDimensions(mediumFroxelCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceFroxelCount, ignoredStride);
	gSmokeFroxelIntegrated.GetDimensions(integratedFroxelCount, ignoredStride);
	const uint2 renderDimensions = uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	if (sceneWidth != renderDimensions.x || sceneHeight != renderDimensions.y ||
		viewZWidth != renderDimensions.x || viewZHeight != renderDimensions.y ||
		outputWidth != renderDimensions.x || outputHeight != renderDimensions.y ||
		gSmokeConstants.OutputWidth != renderDimensions.x || gSmokeConstants.OutputHeight != renderDimensions.y)
		return;
	if (dispatchThreadId.x >= renderDimensions.x || dispatchThreadId.y >= renderDimensions.y)
		return;

	const uint2 outputPixel = dispatchThreadId.xy;
	const float4 scene = gSmokeSceneInput.Load(int3(outputPixel, 0));
	float viewDepth = 0.0;
	if (!SmokeDecodeViewDepth(gSmokeViewZInput.Load(int3(outputPixel, 0)).x, viewDepth))
	{
		gSmokeOutput[outputPixel] = scene;
		return;
	}

	const uint expectedFroxelCount = SmokeFroxelCount();
	if (mediumFroxelCount < expectedFroxelCount || sourceFroxelCount < expectedFroxelCount || integratedFroxelCount < expectedFroxelCount)
	{
		gSmokeOutput[outputPixel] = scene;
		return;
	}

	const float2 primarySampleUv = SmokePrimarySampleUv(outputPixel);
	const SmokeBilinearFootprint footprint = SmokeMakeBilinearFootprint(primarySampleUv);
	const uint depthSlice = SmokeDepthSlice(viewDepth);
	const float sliceNearDepth = SmokeSliceNearDepth(depthSlice);
	const float4 prefix = depthSlice == 0u ? float4(0.0, 0.0, 0.0, 1.0) :
		SmokeBilinearIntegrated(footprint, depthSlice - 1u);
	const float4 localMedium = SmokeBilinearMedium(footprint, depthSlice);
	const float3 localSource = SmokeBilinearSource(footprint, depthSlice);
	const float3 primaryRay = SmokeCameraRay(primarySampleUv);
	const float sliceFarDepth = SmokeSliceFarDepth(depthSlice);
	const float partialDepth = clamp(viewDepth - sliceNearDepth, 0.0, sliceFarDepth - sliceNearDepth);
	const float partialLength = SmokeWorldSegmentLength(primaryRay, sliceNearDepth, sliceNearDepth + partialDepth);
	const float extinction = max(localMedium.a, 0.0);
	const float segmentTransmittance = exp(-extinction * partialLength);
	const float scatterIntegral = extinction > 0.000001 ? (1.0 - segmentTransmittance) / extinction : partialLength;
	const float3 finalRadiance = prefix.rgb + saturate(prefix.a) * max(localSource, 0.0) * scatterIntegral;
	const float finalTransmittance = saturate(prefix.a) * segmentTransmittance;
	const float4 medium = float4(finalRadiance, saturate(finalTransmittance));

	float3 color = scene.rgb * medium.a + max(medium.rgb, 0.0);
	if (gSmokeConstants.DebugMode == 1u)
		color = (1.0 - saturate(medium.a)).xxx;
	else if (gSmokeConstants.DebugMode == 2u)
		color = saturate(medium.a).xxx;
	else if (gSmokeConstants.DebugMode == 3u)
	{
		const float2 cell = frac(primarySampleUv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight));
		const float edge = step(min(min(cell.x, 1.0 - cell.x), min(cell.y, 1.0 - cell.y)), 0.04);
		color = lerp(float3(cell, (float)depthSlice / max((float)gSmokeConstants.FroxelDepth - 1.0, 1.0)), 1.0.xxx, edge);
	}
	else if (gSmokeConstants.DebugMode == 4u)
	{
		const float terminalFraction = saturate((viewDepth - sliceNearDepth) / max(sliceFarDepth - sliceNearDepth, 0.000001));
		color = float3(terminalFraction, 1.0 - terminalFraction, 0.0);
	}
	gSmokeOutput[outputPixel] = float4(color, scene.a);
}
