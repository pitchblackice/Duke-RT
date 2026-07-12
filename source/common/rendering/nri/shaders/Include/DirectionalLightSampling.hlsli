#ifndef RAZE_NRI_DIRECTIONAL_LIGHT_SAMPLING_HLSLI
#define RAZE_NRI_DIRECTIONAL_LIGHT_SAMPLING_HLSLI

float3 BuildDirectionalLightTangent(float3 direction)
{
	const float3 referenceAxis = abs(direction.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	return normalize(cross(referenceAxis, direction));
}

float3 SampleUniformDirectionalCone(float3 centerDirection, float angularRadius, float2 uniformSample)
{
	const float cosTheta = lerp(cos(max(angularRadius, 1e-4)), 1.0, uniformSample.x);
	const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
	const float phi = 6.28318530718 * uniformSample.y;
	const float3 tangent = BuildDirectionalLightTangent(centerDirection);
	const float3 bitangent = normalize(cross(centerDirection, tangent));
	return normalize(centerDirection * cosTheta + tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta));
}

#endif
