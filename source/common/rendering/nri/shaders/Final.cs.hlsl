#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"

float3 BootstrapPattern(float2 uv, float3 cameraForward, float3 skyColor, float3 groundColor, uint frameIndex)
{
	const float2 centered = uv * 2.0 - 1.0;
	const float aspect = (float)gTraceConstants.DisplayWidth / max((float)gTraceConstants.DisplayHeight, 1.0);
	float2 gridUv = float2(centered.x * aspect, centered.y);
	float3 color = lerp(groundColor, skyColor, saturate(uv.y));
	color = lerp(color, abs(normalize(cameraForward)) * 0.75 + 0.1, 0.35);

	const float borderMask = step(0.96, max(abs(centered.x), abs(centered.y)));
	const float crossMask = step(abs(gridUv.x), 0.01) + step(abs(gridUv.y), 0.01);
	const float gridMask = step(frac((gridUv.x + 8.0) * 8.0), 0.02) + step(frac((gridUv.y + 8.0) * 8.0), 0.02);
	const float framePulse = ((frameIndex & 31u) < 16u) ? 1.0 : 0.35;

	color = lerp(color, float3(0.02, 0.02, 0.02), saturate(gridMask * 0.35));
	color = lerp(color, float3(1.0, 1.0, 1.0), saturate(crossMask));
	color = lerp(color, float3(framePulse, 0.25, 1.0 - framePulse * 0.5), borderMask);
	return saturate(color);
}

float3 BootstrapPlane(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 planeNormal = normalize(gTraceConstants.CameraUp);
	const float3 planeRight = normalize(gTraceConstants.CameraRight);
	const float3 planeForward = normalize(cross(planeNormal, planeRight));
	const float3 planePoint = gTraceConstants.CameraPos - planeNormal * 96.0;
	float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);
	const float denom = dot(rayDir, planeNormal);
	if (abs(denom) > 0.0001)
	{
		const float t = dot(planePoint - gTraceConstants.CameraPos, planeNormal) / denom;
		if (t > 0.0)
		{
			const float3 hitPos = gTraceConstants.CameraPos + rayDir * t;
			const float localX = dot(hitPos - planePoint, planeRight);
			const float localZ = dot(hitPos - planePoint, planeForward);
			const float checker = fmod(floor(localX * 0.125) + floor(localZ * 0.125), 2.0);
			const float gridX = step(frac(abs(localX) * 0.125), 0.035);
			const float gridZ = step(frac(abs(localZ) * 0.125), 0.035);
			const float gridMask = saturate(gridX + gridZ);
			const float3 warm = float3(0.62, 0.43, 0.24);
			const float3 cool = float3(0.22, 0.24, 0.28);
			float3 base = lerp(cool, warm, checker);
			const float distanceFade = saturate(1.0 / (1.0 + t * 0.03));
			const float stripe = 0.5 + 0.5 * sin(localX * 0.05 + gTraceConstants.FrameIndex * 0.02);
			base = lerp(base, base.bgr, stripe * 0.15);
			base = lerp(base, float3(0.98, 0.96, 0.9), gridMask * 0.85);
			const float sun = saturate(dot(planeNormal, normalize(gTraceConstants.LightDirection))) * 0.35 + 0.65;
			color = base * distanceFade * sun;
		}
	}

	return saturate(color);
}

bool IntersectTriangle(float3 rayOrigin, float3 rayDir, float3 v0, float3 v1, float3 v2, out float outT, out float3 outBarycentrics)
{
	outT = 0.0;
	outBarycentrics = 0.0;
	const float3 edge1 = v1 - v0;
	const float3 edge2 = v2 - v0;
	const float3 p = cross(rayDir, edge2);
	const float det = dot(edge1, p);
	if (abs(det) < 1e-5)
	{
		return false;
	}

	const float invDet = 1.0 / det;
	const float3 t = rayOrigin - v0;
	const float u = dot(t, p) * invDet;
	if (u < 0.0 || u > 1.0)
	{
		return false;
	}

	const float3 q = cross(t, edge1);
	const float v = dot(rayDir, q) * invDet;
	if (v < 0.0 || (u + v) > 1.0)
	{
		return false;
	}

	const float hitT = dot(edge2, q) * invDet;
	if (hitT <= 0.0)
	{
		return false;
	}

	outT = hitT;
	outBarycentrics = float3(1.0 - u - v, u, v);
	return true;
}

float3 BootstrapTriangle(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);

	const float3 center = gTraceConstants.CameraPos + gTraceConstants.CameraForward * 256.0;
	const float3 v0 = center + gTraceConstants.CameraUp * 72.0;
	const float3 v1 = center - gTraceConstants.CameraRight * 80.0 - gTraceConstants.CameraUp * 56.0;
	const float3 v2 = center + gTraceConstants.CameraRight * 80.0 - gTraceConstants.CameraUp * 56.0;
	float hitT = 0.0;
	float3 bary = 0.0;
	if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v1, v2, hitT, bary))
	{
		color = bary;
		const float edge = min(bary.x, min(bary.y, bary.z));
		color = lerp(color, float3(1.0, 1.0, 1.0), step(edge, 0.04));
	}

	return saturate(color);
}

float3 BootstrapTexturedQuad(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 rayDir = normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);

	const float3 center = gTraceConstants.CameraPos + gTraceConstants.CameraForward * 320.0 + gTraceConstants.CameraUp * 12.0;
	const float3 quadRight = gTraceConstants.CameraRight * 128.0;
	const float3 quadUp = gTraceConstants.CameraUp * 96.0;
	const float3 v0 = center - quadRight + quadUp;
	const float3 v1 = center - quadRight - quadUp;
	const float3 v2 = center + quadRight - quadUp;
	const float3 v3 = center + quadRight + quadUp;

	float hitT = 0.0;
	float3 bary = 0.0;
	float2 surfaceUv = 0.0;
	if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v1, v2, hitT, bary))
	{
		surfaceUv = float2(bary.z, bary.y + bary.z);
	}
	else if (IntersectTriangle(gTraceConstants.CameraPos, rayDir, v0, v2, v3, hitT, bary))
	{
		surfaceUv = float2(bary.y + bary.z, bary.z);
	}
	else
	{
		return saturate(color);
	}

	const float checker = fmod(floor(surfaceUv.x * 8.0) + floor(surfaceUv.y * 8.0), 2.0);
	float3 texel = lerp(float3(0.15, 0.18, 0.72), float3(0.95, 0.78, 0.18), checker);
	const float gridMask = step(frac(surfaceUv.x * 8.0), 0.04) + step(frac(surfaceUv.y * 8.0), 0.04);
	texel = lerp(texel, float3(1.0, 1.0, 1.0), saturate(gridMask));
	texel *= 0.8 + 0.2 * sin((surfaceUv.x + surfaceUv.y + gTraceConstants.FrameIndex * 0.01) * 12.0);
	return saturate(texel);
}

float3 BootstrapGenerateRay(float2 uv)
{
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	return normalize(
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);
}

float3 BootstrapCapturedSceneFlat(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	if (gTraceConstants.PrimitiveCount == 0u)
	{
		return color;
	}

	const float3 rayDir = BootstrapGenerateRay(uv);
	const HitData hit = TraceBootstrapGeometry(gTraceConstants.CameraPos, rayDir);
	if (!hit.hit)
	{
		return color;
	}

	const float primitiveHash = (float)(hit.primitiveIndex % 31u) / 30.0;
	return float3(frac(primitiveHash * 1.7), frac(primitiveHash * 2.3), frac(primitiveHash * 3.1));
}

float3 BootstrapCapturedSceneBaseColor(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	if (gTraceConstants.PrimitiveCount == 0u)
	{
		return color;
	}

	const float3 rayDir = BootstrapGenerateRay(uv);
	const HitData hit = TraceBootstrapGeometry(gTraceConstants.CameraPos, rayDir);
	if (!hit.hit)
	{
		return color;
	}

	return saturate(SampleSurfaceColor(hit.materialIndex, hit.uv).rgb);
}

float3 BootstrapCapturedSceneLit(float2 uv)
{
	float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	if (gTraceConstants.PrimitiveCount == 0u)
	{
		return color;
	}

	const float3 rayDir = BootstrapGenerateRay(uv);
	const HitData hit = TraceBootstrapGeometry(gTraceConstants.CameraPos, rayDir);
	if (!hit.hit)
	{
		return GetMissColor(rayDir);
	}

	const float4 albedo = SampleSurfaceColor(hit.materialIndex, hit.uv);
	const float lambert = max(dot(hit.normal, gTraceConstants.LightDirection), 0.0);
	const float lighting = 0.20 + lambert * 0.80;
	float3 diffuse = albedo.rgb * lighting;
	const float3 halfVector = normalize(gTraceConstants.LightDirection - rayDir);
	const float specularTerm = pow(max(dot(hit.normal, halfVector), 0.0), 32.0);
	const float3 specular = albedo.rgb * specularTerm * 0.2;
	return saturate(diffuse + specular);
}

float3 BootstrapHashColor(uint index)
{
	const float seed = (float)(index + 1u);
	return saturate(float3(frac(seed * 0.173), frac(seed * 0.347), frac(seed * 0.613)));
}

float2 BootstrapNormalizeToBounds(float2 value, float2 minValue, float2 maxValue)
{
	const float2 span = max(maxValue - minValue, 1e-3.xx);
	return (value - minValue) / span;
}

bool BootstrapProjectPoint(float3 worldPos, out float2 screenPos, out float viewZ)
{
	const float3 relative = worldPos - gTraceConstants.CameraPos;
	viewZ = dot(relative, gTraceConstants.CameraForward);
	if (viewZ <= 0.001)
	{
		screenPos = 0.0;
		return false;
	}

	const float ndcX = dot(relative, gTraceConstants.CameraRight) / max(viewZ * gTraceConstants.TanHalfFovX, 1e-5);
	const float ndcY = dot(relative, gTraceConstants.CameraUp) / max(viewZ * gTraceConstants.TanHalfFovY, 1e-5);
	const float2 uv = float2(ndcX * 0.5 + 0.5, 0.5 - ndcY * 0.5);
	screenPos = uv * float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight);
	return all(uv >= float2(-0.2, -0.2)) && all(uv <= float2(1.2, 1.2));
}

float BootstrapDistanceToSegment(float2 p, float2 a, float2 b)
{
	const float2 ab = b - a;
	const float denom = max(dot(ab, ab), 1e-5);
	const float t = saturate(dot(p - a, ab) / denom);
	return length((a + ab * t) - p);
}

bool BootstrapProjectPrimitive(uint primitiveIndex, out float2 p0, out float2 p1, out float2 p2, out float3 worldCenter)
{
	const PrimitiveData primitive = gPrimitives[min(primitiveIndex, gTraceConstants.PrimitiveCount - 1u)];
	const SceneVertex v0 = gVertices[primitive.indices.x];
	const SceneVertex v1 = gVertices[primitive.indices.y];
	const SceneVertex v2 = gVertices[primitive.indices.z];
	float z0 = 0.0;
	float z1 = 0.0;
	float z2 = 0.0;
	const bool ok0 = BootstrapProjectPoint(v0.position, p0, z0);
	const bool ok1 = BootstrapProjectPoint(v1.position, p1, z1);
	const bool ok2 = BootstrapProjectPoint(v2.position, p2, z2);
	worldCenter = (v0.position + v1.position + v2.position) / 3.0;
	return ok0 && ok1 && ok2;
}

bool BootstrapBarycentrics2D(float2 p, float2 a, float2 b, float2 c, out float3 bary)
{
	const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (abs(area) < 1e-5)
	{
		bary = 0.0;
		return false;
	}

	bary.x = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
	bary.y = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
	bary.z = 1.0 - bary.x - bary.y;
	return all(bary >= -0.001) && all(bary <= 1.001);
}

float3 BootstrapCapturedPoints(uint2 pixelPos)
{
	float3 color = BootstrapPattern(((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight), gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 pixel = (float2)pixelPos + 0.5;
	const uint primitiveCount = min(gTraceConstants.PrimitiveCount, 96u);
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const PrimitiveData primitive = gPrimitives[primitiveIndex];
		const uint indices[3] = { primitive.indices.x, primitive.indices.y, primitive.indices.z };
		[unroll]
		for (uint i = 0; i < 3; ++i)
		{
			float2 projectedPoint = 0.0;
			float viewZ = 0.0;
			if (!BootstrapProjectPoint(gVertices[indices[i]].position, projectedPoint, viewZ))
			{
				continue;
			}

			if (length(projectedPoint - pixel) <= 2.0)
			{
				color = BootstrapHashColor(indices[i]);
			}
		}
	}

	return saturate(color);
}

float3 BootstrapSceneSignature(float2 uv)
{
	float3 color = float3(0.03, 0.04, 0.07);
	if (gTraceConstants.PrimitiveCount == 0u)
	{
		return float3(0.85, 0.1, 0.1);
	}

	const PrimitiveData primitive = gPrimitives[0];
	const SceneVertex v0 = gVertices[primitive.indices.x];
	const float primitiveNorm = saturate((float)gTraceConstants.PrimitiveCount / 1024.0);
	const float materialNorm = saturate((float)gTraceConstants.MaterialCount / 256.0);
	const float3 indexNorm = frac(float3((float)primitive.indices.x, (float)primitive.indices.y, (float)primitive.indices.z) * 0.013);
	const float3 positionNorm = abs(v0.position) / (abs(v0.position) + 256.0);
	const float3 normalNorm = primitive.normal * 0.5 + 0.5;
	const float flagsNorm = saturate((float)(primitive.flags & 255u) / 255.0);
	const float3 header = lerp(float3(0.08, 0.1, 0.14), float3(0.95, 0.85, 0.25), step(frac(uv.x * 24.0), primitiveNorm));

	if (uv.y < 0.18)
	{
		color = header;
	}
	else if (uv.y < 0.36)
	{
		const float lane = floor(uv.x * 3.0);
		if (lane < 1.0)
		{
			color = lerp(float3(0.06, 0.06, 0.08), float3(0.2, 0.95, 0.35), step(frac(uv.y * 22.0), primitiveNorm));
		}
		else if (lane < 2.0)
		{
			color = lerp(float3(0.06, 0.06, 0.08), float3(0.22, 0.55, 0.98), step(frac(uv.y * 22.0), materialNorm));
		}
		else
		{
			color = lerp(float3(0.06, 0.06, 0.08), float3(0.95, 0.3, 0.2), step(frac(uv.y * 22.0), flagsNorm));
		}
	}
	else if (uv.y < 0.68)
	{
		const float lane = floor(uv.x * 3.0);
		if (lane < 1.0)
		{
			color = positionNorm;
		}
		else if (lane < 2.0)
		{
			color = indexNorm;
		}
		else
		{
			color = normalNorm;
		}
	}
	else
	{
		const float2 tileUv = frac(uv * 12.0);
		const float tileMask = step(tileUv.x, primitiveNorm) * step(tileUv.y, materialNorm);
		color = lerp(float3(0.03, 0.03, 0.05), normalNorm.bgr * 0.85 + 0.15, tileMask);
	}

	return saturate(color);
}

float3 BootstrapRawVertexScatter(uint2 pixelPos)
{
	const float2 uv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight);
	float3 color = float3(0.02, 0.025, 0.04);
	const uint vertexCount = min(gTraceConstants.PrimitiveCount * 3u, 192u);
	if (vertexCount == 0u)
	{
		return float3(0.85, 0.1, 0.1);
	}

	float2 minPos = float2(1e20, 1e20);
	float2 maxPos = float2(-1e20, -1e20);
	[loop]
	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const float2 pos = gVertices[vertexIndex].position.xz;
		minPos = min(minPos, pos);
		maxPos = max(maxPos, pos);
	}

	const float2 gridSize = float2(24.0, 14.0);
	const float2 cell = floor(uv * gridSize);
	float occupancy = 0.0;
	float3 accum = 0.0;
	[loop]
	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const float2 normPos = BootstrapNormalizeToBounds(gVertices[vertexIndex].position.xz, minPos, maxPos);
		const float2 vertexCell = floor(float2(normPos.x, 1.0 - normPos.y) * gridSize);
		if (all(abs(vertexCell - cell) < 0.5))
		{
			occupancy += 1.0;
			accum += BootstrapHashColor(vertexIndex);
		}
	}

	const float2 cellUv = frac(uv * gridSize);
	const float gridLine = step(cellUv.x, 0.06) + step(cellUv.y, 0.06);
	if (occupancy > 0.0)
	{
		const float3 cellColor = accum / occupancy;
		const float intensity = saturate(occupancy / 4.0);
		color = lerp(cellColor * 0.4, cellColor, intensity);
	}
	color = lerp(color, float3(0.9, 0.92, 0.96), saturate(gridLine * 0.45));

	return saturate(color);
}

float3 BootstrapRawPrimitiveScatter(uint2 pixelPos)
{
	const float2 uv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight);
	float3 color = float3(0.025, 0.02, 0.045);
	const uint primitiveCount = min(gTraceConstants.PrimitiveCount, 128u);
	if (primitiveCount == 0u)
	{
		return float3(0.85, 0.1, 0.1);
	}

	float2 minPos = float2(1e20, 1e20);
	float2 maxPos = float2(-1e20, -1e20);
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const PrimitiveData primitive = gPrimitives[primitiveIndex];
		const float2 c = (gVertices[primitive.indices.x].position.xz + gVertices[primitive.indices.y].position.xz + gVertices[primitive.indices.z].position.xz) / 3.0;
		minPos = min(minPos, c);
		maxPos = max(maxPos, c);
	}

	const float2 gridSize = float2(20.0, 12.0);
	const float2 cell = floor(uv * gridSize);
	float occupancy = 0.0;
	float3 accum = 0.0;
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const PrimitiveData primitive = gPrimitives[primitiveIndex];
		const float2 c = (gVertices[primitive.indices.x].position.xz + gVertices[primitive.indices.y].position.xz + gVertices[primitive.indices.z].position.xz) / 3.0;
		const float2 normPos = BootstrapNormalizeToBounds(c, minPos, maxPos);
		const float2 primitiveCell = floor(float2(normPos.x, 1.0 - normPos.y) * gridSize);
		if (all(abs(primitiveCell - cell) < 0.5))
		{
			occupancy += 1.0;
			accum += BootstrapHashColor(primitiveIndex);
		}
	}

	const float2 cellUv = frac(uv * gridSize);
	const float gridLine = step(cellUv.x, 0.06) + step(cellUv.y, 0.06);
	if (occupancy > 0.0)
	{
		const float3 cellColor = accum / occupancy;
		const float intensity = saturate(occupancy / 3.0);
		color = lerp(cellColor * 0.35, cellColor * 1.1, intensity);
	}
	color = lerp(color, float3(0.95, 0.95, 0.98), saturate(gridLine * 0.45));

	return saturate(color);
}

float3 BootstrapPrimitiveCentroids(uint2 pixelPos)
{
	float3 color = BootstrapPattern(((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight), gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 pixel = (float2)pixelPos + 0.5;
	const uint primitiveCount = min(gTraceConstants.PrimitiveCount, 128u);
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		float2 p0 = 0.0;
		float2 p1 = 0.0;
		float2 p2 = 0.0;
		float3 center = 0.0;
		float2 centroidPos = 0.0;
		if (!BootstrapProjectPrimitive(primitiveIndex, p0, p1, p2, center))
		{
			continue;
		}

		centroidPos = (p0 + p1 + p2) / 3.0;
		if (length(centroidPos - pixel) <= 2.5)
		{
			color = BootstrapHashColor(primitiveIndex);
		}
	}

	return saturate(color);
}

float3 BootstrapWireframe(uint2 pixelPos)
{
	float3 color = BootstrapPattern(((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight), gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 pixel = (float2)pixelPos + 0.5;
	const uint primitiveCount = min(gTraceConstants.PrimitiveCount, 48u);
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		float2 p0 = 0.0;
		float2 p1 = 0.0;
		float2 p2 = 0.0;
		float3 center = 0.0;
		if (!BootstrapProjectPrimitive(primitiveIndex, p0, p1, p2, center))
		{
			continue;
		}

		const float edgeDist = min(BootstrapDistanceToSegment(pixel, p0, p1), min(BootstrapDistanceToSegment(pixel, p1, p2), BootstrapDistanceToSegment(pixel, p2, p0)));
		if (edgeDist <= 1.0)
		{
			color = BootstrapHashColor(primitiveIndex);
		}
	}

	return saturate(color);
}

float3 BootstrapFirstTriangle(uint2 pixelPos)
{
	float3 color = BootstrapPattern(((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight), gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
	const float2 pixel = (float2)pixelPos + 0.5;
	const uint primitiveCount = min(gTraceConstants.PrimitiveCount, 128u);
	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		float2 p0 = 0.0;
		float2 p1 = 0.0;
		float2 p2 = 0.0;
		float3 center = 0.0;
		if (!BootstrapProjectPrimitive(primitiveIndex, p0, p1, p2, center))
		{
			continue;
		}

		float3 bary = 0.0;
		if (BootstrapBarycentrics2D(pixel, p0, p1, p2, bary))
		{
			const float edge = min(bary.x, min(bary.y, bary.z));
			color = lerp(BootstrapHashColor(primitiveIndex), float3(1.0, 1.0, 1.0), step(edge, 0.03));
		}
		return saturate(color);
	}

	return saturate(color);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.DisplayWidth || dispatchThreadId.y >= gTraceConstants.DisplayHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float2 uv = ((float2)pixelPos + 0.5) / float2(gTraceConstants.DisplayWidth, gTraceConstants.DisplayHeight);
	float4 composed = 0.0;

	if ((gTraceConstants.Flags & 0x4u) != 0)
	{
		float3 color = BootstrapPattern(uv, gTraceConstants.CameraForward, gTraceConstants.SkyColor, gTraceConstants.GroundColor, gTraceConstants.FrameIndex);
		if (gTraceConstants.BootstrapMode == 1)
		{
			color = BootstrapPlane(uv);
		}
		else if (gTraceConstants.BootstrapMode == 2)
		{
			color = BootstrapTriangle(uv);
		}
		else if (gTraceConstants.BootstrapMode == 3)
		{
			color = BootstrapTexturedQuad(uv);
		}
		else if (gTraceConstants.BootstrapMode == 4)
		{
			color = BootstrapSceneSignature(uv);
		}
		else if (gTraceConstants.BootstrapMode == 5)
		{
			color = BootstrapRawVertexScatter(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 6)
		{
			color = BootstrapRawPrimitiveScatter(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 7)
		{
			color = BootstrapCapturedPoints(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 8)
		{
			color = BootstrapPrimitiveCentroids(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 9)
		{
			color = BootstrapWireframe(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 10)
		{
			color = BootstrapFirstTriangle(pixelPos);
		}
		else if (gTraceConstants.BootstrapMode == 11)
		{
			color = BootstrapCapturedSceneFlat(uv);
		}
		else if (gTraceConstants.BootstrapMode == 12)
		{
			color = BootstrapCapturedSceneBaseColor(uv);
		}
		gFinalOutput[pixelPos] = float4(saturate(color), 1.0);
		return;
	}

	if (gTraceConstants.DebugMode == 5)
	{
		const float2 motion = gMotionInput[pixelPos].xy / max(float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight), 1.0);
		composed = float4(motion * 0.5 + 0.5, 0.5, 1.0);
	}
	else if (gTraceConstants.DebugMode == 6)
	{
		const float viewZ = abs(gViewZInput[pixelPos].x);
		const float normalized = saturate(viewZ / 4096.0);
		composed = float4(normalized.xxx, 1.0);
	}
	else if (gTraceConstants.DebugMode == 7)
	{
		composed = float4(saturate(gBaseColorInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 8)
	{
		composed = float4(NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos]).xyz * 0.5 + 0.5, 1.0);
	}
	else if (gTraceConstants.DebugMode == 9)
	{
		composed = float4(saturate(gValidationInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 10)
	{
		composed = float4(saturate(gGuideDiffuseInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 11)
	{
		composed = float4(saturate(gGuideSpecularInput[pixelPos].rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 12)
	{
		const float hitMetric = saturate(gGuideSpecHitInput[pixelPos].w);
		composed = float4(hitMetric.xxx, 1.0);
	}
	else if (gTraceConstants.DebugMode == 13)
	{
		composed = float4(saturate(gHistoryInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 14)
	{
		composed = float4(saturate(gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
	}
	else if (gTraceConstants.DebugMode == 15)
	{
		if ((gTraceConstants.Flags & 0x2u) != 0)
		{
			composed = float4(saturate(gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
		}
		else
		{
			composed = float4(saturate(gComposedInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
		}
	}
	else if ((gTraceConstants.Flags & 0x8u) != 0)
	{
		composed = float4(saturate(gComposedInput.SampleLevel(gLinearClamp, uv, 0.0).rgb), 1.0);
	}
	else if ((gTraceConstants.Flags & 0x2u) != 0)
	{
		composed = gUpscaledInput.SampleLevel(gLinearClamp, uv, 0.0);
	}
	else
	{
		composed = gComposedInput[pixelPos];
	}

	gFinalOutput[pixelPos] = float4(saturate(composed.rgb), 1.0);
}
