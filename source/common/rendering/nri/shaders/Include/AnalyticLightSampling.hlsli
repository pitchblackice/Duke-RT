#ifndef NRI_ANALYTIC_LIGHT_SAMPLING_HLSLI
#define NRI_ANALYTIC_LIGHT_SAMPLING_HLSLI

float EvaluateAnalyticPointLightAttenuation(float distanceToLight, float radius, float intensity)
{
	if (radius <= 0.0 || distanceToLight >= radius)
		return 0.0;

	const float normalizedDistance = saturate(distanceToLight / radius);
	const float softRange = 1.0 - normalizedDistance;
	const float smoothRange = softRange * softRange * (3.0 - 2.0 * softRange);
	const float shapedFalloff = rcp(1.0 + 4.0 * normalizedDistance * normalizedDistance);
	return intensity * smoothRange * smoothRange * shapedFalloff;
}

float3 BuildAnalyticEmitterTangent(float3 direction)
{
	const float3 referenceAxis = abs(direction.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	return normalize(cross(referenceAxis, direction));
}

float3 SampleAnalyticReceiverFacingDisk(float3 position, float emitterRadius, float3 centerDirection, float2 unitDiskSample)
{
	if (emitterRadius <= 0.0)
		return position;

	const float3 tangent = BuildAnalyticEmitterTangent(centerDirection);
	const float3 bitangent = normalize(cross(centerDirection, tangent));
	const float2 disk = unitDiskSample * emitterRadius;
	return position + tangent * disk.x + bitangent * disk.y;
}

#endif
