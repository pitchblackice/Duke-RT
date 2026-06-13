#include "nri_debug_overlays.h"

#include "nri_render_geometry_helpers.h"
#include "nri_renderer.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

EXTERN_CVAR(Int, nri_ptspherelongs)
EXTERN_CVAR(Int, nri_ptspherelats)

bool NRIRenderer::AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId)
{
	if (!mDebugOverlays.AddRuntimeDebugSphere(center, diameter, metalness, roughness, outId))
	{
		return false;
	}

	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

bool NRIRenderer::RemoveRuntimeDebugSphere(uint32_t id)
{
	if (!mDebugOverlays.RemoveRuntimeDebugSphere(id))
	{
		return false;
	}

	RequestHistoryReset("runtime-debug-sphere-change");
	return true;
}

void NRIRenderer::ClearRuntimeDebugSpheres()
{
	if (!mDebugOverlays.ClearRuntimeDebugSpheres())
	{
		return;
	}

	RequestHistoryReset("runtime-debug-sphere-change");
}

void NRIRenderer::PrintRuntimeDebugSpheres() const
{
	mDebugOverlays.PrintRuntimeDebugSpheres();
}

uint32_t NRIRenderer::GetRuntimeDebugSphereCount() const
{
	return mDebugOverlays.GetRuntimeDebugSphereCount();
}

void NRIRenderer::NotifyDebugSphereTessellationChange()
{
	mDebugOverlays.InvalidateRuntimeDebugSphereTessellation();
	RequestHistoryReset("debug-sphere-tessellation-change");
}

namespace
{
	constexpr uint32_t NriPtDebugSphereLimit = 64u;

	uint32_t GetRuntimeDebugSphereLongitudeSegments()
	{
		return (uint32_t)std::clamp<int>(nri_ptspherelongs, 8, 256);
	}

	uint32_t GetRuntimeDebugSphereLatitudeSegments()
	{
		return (uint32_t)std::clamp<int>(nri_ptspherelats, 4, 128);
	}

	uint32_t GetRuntimeDebugSphereTriangleCount()
	{
		return GetRuntimeDebugSphereLongitudeSegments() * 2u * (GetRuntimeDebugSphereLatitudeSegments() - 1u);
	}

	void PathTracingToWorldPosition(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	class ScopedDebugOverlayTimer
	{
	public:
		ScopedDebugOverlayTimer(double& targetMs, bool collectTiming)
			: mTarget(collectTiming ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedDebugOverlayTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};
}

bool NRIDebugOverlaySystem::AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId)
{
	if (center == nullptr || diameter <= 0.0f)
	{
		return false;
	}

	if (mRuntimeDebugSpheres.size() >= NriPtDebugSphereLimit)
	{
		return false;
	}

	RuntimeDebugSphere sphere = {};
	sphere.id = mNextRuntimeDebugSphereId++;
	nri_scene::Copy3(center, sphere.center);
	sphere.diameter = diameter;
	sphere.metalness = std::clamp(metalness, 0.0f, 1.0f);
	sphere.roughness = std::clamp(roughness, 0.0f, 1.0f);

	NRIDebugOverlayBuildTelemetry telemetry = {};
	if (!EnsureRuntimeDebugSphereCache(sphere, false, telemetry))
	{
		return false;
	}

	mRuntimeDebugSpheres.push_back(std::move(sphere));
	outId = mRuntimeDebugSpheres.back().id;
	return true;
}

bool NRIDebugOverlaySystem::RemoveRuntimeDebugSphere(uint32_t id)
{
	const auto it = std::find_if(mRuntimeDebugSpheres.begin(), mRuntimeDebugSpheres.end(),
		[id](const RuntimeDebugSphere& sphere)
		{
			return sphere.id == id;
		});
	if (it == mRuntimeDebugSpheres.end())
	{
		return false;
	}

	mRuntimeDebugSpheres.erase(it);
	return true;
}

bool NRIDebugOverlaySystem::ClearRuntimeDebugSpheres()
{
	if (mRuntimeDebugSpheres.empty())
	{
		return false;
	}

	mRuntimeDebugSpheres.clear();
	return true;
}

void NRIDebugOverlaySystem::PrintRuntimeDebugSpheres() const
{
	Printf("NRI PT debug spheres: active=%u limit=%u tessellation=%ux%u triangles_per_sphere=%u\n",
		(uint32_t)mRuntimeDebugSpheres.size(),
		NriPtDebugSphereLimit,
		GetRuntimeDebugSphereLongitudeSegments(),
		GetRuntimeDebugSphereLatitudeSegments(),
		GetRuntimeDebugSphereTriangleCount());
	for (const RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		float worldPosition[3] = {};
		PathTracingToWorldPosition(sphere.center, worldPosition);
		Printf("NRI PT debug sphere %u: id=%u render_pos=(%.3f, %.3f, %.3f) world_pos=(%.3f, %.3f, %.3f) diameter=%.3f metalness=%.3f roughness=%.3f\n",
			sphere.id,
			sphere.id,
			sphere.center[0],
			sphere.center[1],
			sphere.center[2],
			worldPosition[0],
			worldPosition[1],
			worldPosition[2],
			sphere.diameter,
			sphere.metalness,
			sphere.roughness);
	}
}

void NRIDebugOverlaySystem::InvalidateRuntimeDebugSphereTessellation()
{
	for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
	{
		sphere.cacheValid = false;
		sphere.cachedLongitudeSegments = 0;
		sphere.cachedLatitudeSegments = 0;
	}
}

bool NRIDebugOverlaySystem::BuildRuntimeDebugSphereOverlay(
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials,
	NRIDebugOverlayBuildTelemetry& outTelemetry,
	bool collectTiming)
{
	outTelemetry.runtimeDebugSphereCount = (uint32_t)mRuntimeDebugSpheres.size();
	outTelemetry.runtimeDebugSphereLongitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	outTelemetry.runtimeDebugSphereLatitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	outTelemetry.runtimeDebugSpherePrimitiveCount = 0;
	outTelemetry.runtimeDebugSphereMaterialCount = 0;

	if (mRuntimeDebugSpheres.empty())
	{
		return false;
	}

	size_t totalVertexCount = 0;
	size_t totalIndexCount = 0;
	size_t totalPrimitiveCount = 0;
	size_t totalProvenanceCount = 0;
	size_t totalMaterialCount = 0;
	size_t totalLightMetadataCount = 0;
	{
		ScopedDebugOverlayTimer perfTimer(outTelemetry.runtimeDebugSphereGeoMs, collectTiming);
		for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
		{
			if (!EnsureRuntimeDebugSphereCache(sphere, collectTiming, outTelemetry))
			{
				continue;
			}

			totalVertexCount += sphere.geometry.vertices.size();
			totalIndexCount += sphere.geometry.indices.size();
			totalPrimitiveCount += sphere.geometry.primitives.size();
			totalProvenanceCount += sphere.geometry.primitiveProvenance.size();
			totalMaterialCount += sphere.materialBridge.materials.size();
			totalLightMetadataCount += sphere.materialBridge.lightMetadata.size();
		}
	}

	outGeometry.vertices.reserve(totalVertexCount);
	outGeometry.indices.reserve(totalIndexCount);
	outGeometry.primitives.reserve(totalPrimitiveCount);
	outGeometry.primitiveProvenance.reserve(totalProvenanceCount);
	outMaterials.materials.reserve(totalMaterialCount);
	outMaterials.lightMetadata.reserve(totalLightMetadataCount);

	{
		ScopedDebugOverlayTimer perfTimer(outTelemetry.runtimeDebugSphereMaterialMs, collectTiming);
		for (RuntimeDebugSphere& sphere : mRuntimeDebugSpheres)
		{
			if (!sphere.cacheValid)
			{
				continue;
			}

			AppendGeometry(sphere.geometry, (uint32_t)outMaterials.materials.size(), outGeometry);
			nri_scene::AppendMaterialBridge(sphere.materialBridge, outMaterials);
		}
	}

	outTelemetry.runtimeDebugSpherePrimitiveCount = (uint32_t)outGeometry.primitives.size();
	outTelemetry.runtimeDebugSphereMaterialCount = (uint32_t)outMaterials.materials.size();
	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}

bool NRIDebugOverlaySystem::EnsureRuntimeDebugSphereCache(RuntimeDebugSphere& sphere, bool collectTiming, NRIDebugOverlayBuildTelemetry& telemetry)
{
	const uint32_t longitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	const uint32_t latitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	if (sphere.cacheValid &&
		sphere.cachedLongitudeSegments == longitudeSegments &&
		sphere.cachedLatitudeSegments == latitudeSegments &&
		!sphere.geometry.primitives.empty() &&
		!sphere.materialBridge.materials.empty())
	{
		return true;
	}

	sphere.geometry = {};
	sphere.materialBridge = {};

	nri_scene::SceneView sphereView;
	sphereView.opaqueFlats.reserve(1u);
	sphereView.stats.totalDrawItems = 1;
	sphereView.stats.flatDrawItems = 1;
	sphereView.stats.materialRefs = 1;
	sphereView.stats.triangleEstimate = (unsigned int)GetRuntimeDebugSphereTriangleCount();
	AppendRuntimeDebugSphereToSceneView(sphere, sphereView);

	{
		ScopedDebugOverlayTimer perfTimer(telemetry.geometryBuildDebugSphereMs, collectTiming);
		nri_scene::BuildGeometry(sphereView, sphere.geometry);
	}
	nri_scene::BuildMaterials(sphereView, sphere.materialBridge);

	const size_t materialCount = sphere.materialBridge.materials.size();
	if (sphere.geometry.primitives.empty() || materialCount == 0)
	{
		sphere.cacheValid = false;
		sphere.cachedLongitudeSegments = 0;
		sphere.cachedLatitudeSegments = 0;
		return false;
	}

	for (size_t i = 0; i < materialCount; ++i)
	{
		nri_scene::MaterialData& material = sphere.materialBridge.materials[i];
		material.lightLevel = 1.0f;
		material.alpha = 1.0f;
		material.metalnessHint = sphere.metalness;
		material.roughnessHint = sphere.roughness;
		material.materialClass = 0;

		if (i < sphere.materialBridge.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = sphere.materialBridge.lightMetadata[i];
			metadata.texture = nullptr;
			metadata.textureId = 0;
			metadata.materialFlags = material.flags;
			metadata.materialClass = material.materialClass;
			metadata.alpha = material.alpha;
			metadata.lightLevel = material.lightLevel;
			metadata.averageColor[0] = 1.0f;
			metadata.averageColor[1] = 1.0f;
			metadata.averageColor[2] = 1.0f;

			uint32_t diameterBits = 0;
			uint32_t metalnessBits = 0;
			uint32_t roughnessBits = 0;
			std::memcpy(&diameterBits, &sphere.diameter, sizeof(diameterBits));
			std::memcpy(&metalnessBits, &sphere.metalness, sizeof(metalnessBits));
			std::memcpy(&roughnessBits, &sphere.roughness, sizeof(roughnessBits));
			metadata.materialKey = HashCombine64(metadata.materialKey, sphere.id);
			metadata.materialKey = HashCombine64(metadata.materialKey, ((uint64_t)diameterBits << 32u) | (uint64_t)metalnessBits);
			metadata.materialKey = HashCombine64(metadata.materialKey, (uint64_t)roughnessBits);
		}
	}

	sphere.cachedLongitudeSegments = longitudeSegments;
	sphere.cachedLatitudeSegments = latitudeSegments;
	sphere.cacheValid = true;
	return true;
}

void NRIDebugOverlaySystem::AppendRuntimeDebugSphereToSceneView(const RuntimeDebugSphere& sphere, nri_scene::SceneView& sceneView) const
{
	const uint32_t longitudeSegments = GetRuntimeDebugSphereLongitudeSegments();
	const uint32_t latitudeSegments = GetRuntimeDebugSphereLatitudeSegments();
	constexpr float Pi = 3.14159265358979323846f;
	auto makeVertex = [Pi](const RuntimeDebugSphere& sphere, float u, float v) -> nri_scene::CapturedVertex
	{
		const float theta = u * 2.0f * Pi;
		const float phi = v * Pi;
		const float radius = sphere.diameter * 0.5f;
		const float sinPhi = sinf(phi);
		nri_scene::CapturedVertex vertex = {};
		vertex.position[0] = sphere.center[0] + radius * sinPhi * cosf(theta);
		vertex.position[1] = sphere.center[1] + radius * cosf(phi);
		vertex.position[2] = sphere.center[2] + radius * sinPhi * sinf(theta);
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = u;
		vertex.uv[1] = v;
		return vertex;
	};
	auto appendTriangle = [](nri_scene::SurfaceRef& surface, const RuntimeDebugSphere& sphere, const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		nri_scene::CapturedVertex v0 = a;
		nri_scene::CapturedVertex v1 = b;
		nri_scene::CapturedVertex v2 = c;

		const float abx = v1.position[0] - v0.position[0];
		const float aby = v1.position[1] - v0.position[1];
		const float abz = v1.position[2] - v0.position[2];
		const float acx = v2.position[0] - v0.position[0];
		const float acy = v2.position[1] - v0.position[1];
		const float acz = v2.position[2] - v0.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float centroidX = (v0.position[0] + v1.position[0] + v2.position[0]) / 3.0f;
		const float centroidY = (v0.position[1] + v1.position[1] + v2.position[1]) / 3.0f;
		const float centroidZ = (v0.position[2] + v1.position[2] + v2.position[2]) / 3.0f;
		const float radialX = centroidX - sphere.center[0];
		const float radialY = centroidY - sphere.center[1];
		const float radialZ = centroidZ - sphere.center[2];
		if (nx * radialX + ny * radialY + nz * radialZ < 0.0f)
		{
			std::swap(v1, v2);
		}

		surface.vertices.push_back(v0);
		surface.vertices.push_back(v1);
		surface.vertices.push_back(v2);
	};

	nri_scene::SurfaceRef surface = {};
	surface.material.texture = nullptr;
	surface.material.palette = 0;
	surface.material.shade = 0;
	surface.material.alpha = 1.0f;
	surface.material.flags = nri_scene::MaterialFlag_None;
	surface.provenance.sourceType = nri_scene::SurfaceSourceType::DebugSphere;

	for (uint32_t lat = 0; lat < latitudeSegments; ++lat)
	{
		const float v0 = (float)lat / (float)latitudeSegments;
		const float v1 = (float)(lat + 1u) / (float)latitudeSegments;
		for (uint32_t lon = 0; lon < longitudeSegments; ++lon)
		{
			const float u0 = (float)lon / (float)longitudeSegments;
			const float u1 = (float)(lon + 1u) / (float)longitudeSegments;
			const auto p00 = makeVertex(sphere, u0, v0);
			const auto p01 = makeVertex(sphere, u1, v0);
			const auto p10 = makeVertex(sphere, u0, v1);
			const auto p11 = makeVertex(sphere, u1, v1);

			if (lat == 0u)
			{
				appendTriangle(surface, sphere, p00, p10, p11);
			}
			else if (lat + 1u == latitudeSegments)
			{
				appendTriangle(surface, sphere, p00, p10, p01);
			}
			else
			{
				appendTriangle(surface, sphere, p00, p10, p11);
				appendTriangle(surface, sphere, p00, p11, p01);
			}
		}
	}

	sceneView.opaqueFlats.push_back(std::move(surface));
}
