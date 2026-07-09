#include "nri_voxel_compute_preload.h"

#include "nri_cvars.h"
#include "nri_persistent_voxels.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace
{
	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	bool IsRequiredComputePreloadVariant(const nri_scene::PrecachedVoxelVariantView& variant)
	{
		return
			variant.priority <= 0 &&
			(variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
			(variant.gpuForce ||
				(variant.gpuPrefer && (variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
	}

	uint64_t EstimateVariantGeometryBytes(const nri_scene::PrecachedVoxelVariantView& variant)
	{
		if (variant.surface != nullptr)
		{
			return
				(uint64_t)variant.surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
				(uint64_t)variant.surface->indices.size() * (uint64_t)sizeof(uint32_t) +
				(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
		}

		return
			(uint64_t)variant.primitiveCount * 2ull * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)variant.primitiveCount * 3ull * (uint64_t)sizeof(uint32_t) +
			(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	}

	bool ShouldEmitPreloadTrace(const NRIVoxelComputePreloadSettings& settings)
	{
		return settings.traceLevel >= 1 || (int)nri_ptloadingtrace >= 1 || (int)nri_ptvoxelcomputetrace >= 1;
	}
}

NRIVoxelComputePreloadSettings BuildNRIVoxelComputePreloadSettingsFromCVars()
{
	NRIVoxelComputePreloadSettings settings = {};
	settings.enabled = (bool)nri_ptvoxelcomputepreload;
	settings.dryRun = (bool)nri_ptvoxelcomputepreloaddryrun;
	settings.traceLevel = std::max(0, (int)nri_ptvoxelcomputepreloadtrace);
	settings.includeRequired = (bool)nri_ptvoxelcomputepreloadrequired;
	settings.includeOptional = (bool)nri_ptvoxelcomputepreloadoptional;
	settings.preloadMaterials = (bool)nri_ptvoxelcomputepreloadmaterials;
	settings.maxMilliseconds = std::max(0, (int)nri_ptvoxelcomputepreloadmaxms);
	settings.maxJobs = std::max(0, (int)nri_ptvoxelcomputepreloadmaxjobs);
	settings.maxBlasBuilds = std::max(0, (int)nri_ptvoxelcomputepreloadmaxblas);
	settings.maxBytes = (int)nri_ptvoxelcomputepreloadmaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelcomputepreloadmaxbytes;
	settings.maxMaterialRows = std::max(0, (int)nri_ptvoxelcomputepreloadmaxmaterialrows);
	return settings;
}

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex)
{
	const auto start = std::chrono::steady_clock::now();
	NRIVoxelComputePreloadStats stats = {};
	stats.enabled = settings.enabled;
	stats.dryRun = settings.dryRun;
	stats.variants = (uint32_t)variants.size();

	std::unordered_set<uint64_t> uniqueMeshes;
	std::unordered_set<uint64_t> uniqueMaterials;
	std::unordered_set<uint64_t> selectedMaterials;
	uniqueMeshes.reserve(variants.size());
	uniqueMaterials.reserve(variants.size());
	selectedMaterials.reserve(variants.size());

	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		const bool required = IsRequiredComputePreloadVariant(variant);
		if (required)
		{
			stats.required++;
		}
		else
		{
			stats.optional++;
		}

		if (variant.meshKeyHash != 0)
		{
			uniqueMeshes.insert(variant.meshKeyHash);
		}
		if (variant.materialKeyHash != 0)
		{
			uniqueMaterials.insert(variant.materialKeyHash);
		}
		if (variant.directOnlyAdmission)
		{
			stats.directOnly++;
		}
		if (variant.surface != nullptr)
		{
			stats.surfaceReady++;
		}
		if (variant.model != nullptr)
		{
			stats.sourceReady++;
		}
		if (variant.materialKeyHash != 0 && variant.materialVariantHash != 0)
		{
			stats.materialContextReady++;
		}

		const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(variant);
		const PersistentVoxelReadinessStatus readiness = residency.GetSharedVariantReadiness(meshResourceKey, variant.materialKeyHash);
		if (readiness.meshPresent)
		{
			stats.meshResident++;
		}
		if (readiness.materialPresent)
		{
			stats.materialResident++;
		}
		if (readiness.blasReady)
		{
			stats.blasReady++;
		}
		if (readiness.ready)
		{
			stats.ready++;
		}
		else
		{
			stats.notReady++;
		}

		const uint64_t estimatedBytes = EstimateVariantGeometryBytes(variant);
		stats.estimatedGeometryBytes += estimatedBytes;
		if (!settings.enabled)
		{
			stats.skippedDisabled++;
			continue;
		}
		if (required && !settings.includeRequired)
		{
			stats.skippedRequiredOff++;
			continue;
		}
		if (!required && !settings.includeOptional)
		{
			stats.skippedOptionalOff++;
			continue;
		}
		if (settings.maxJobs != 0 && stats.selected >= settings.maxJobs)
		{
			stats.skippedJobBudget++;
			continue;
		}
		if (settings.maxBytes != 0 && stats.selectedGeometryBytes + estimatedBytes > settings.maxBytes)
		{
			stats.skippedByteBudget++;
			continue;
		}
		if (settings.preloadMaterials &&
			variant.materialKeyHash != 0 &&
			selectedMaterials.find(variant.materialKeyHash) == selectedMaterials.end() &&
			settings.maxMaterialRows != 0 &&
			stats.materialRowsPlanned + 1u > settings.maxMaterialRows)
		{
			stats.skippedMaterialBudget++;
			continue;
		}

		stats.selected++;
		stats.selectedGeometryBytes += estimatedBytes;
		if (required)
		{
			stats.selectedRequired++;
		}
		else
		{
			stats.selectedOptional++;
		}
		if (settings.preloadMaterials && variant.materialKeyHash != 0 && selectedMaterials.insert(variant.materialKeyHash).second)
		{
			stats.materialRowsPlanned++;
		}
	}

	stats.uniqueMeshes = (uint32_t)uniqueMeshes.size();
	stats.uniqueMaterials = (uint32_t)uniqueMaterials.size();
	const NRIPersistentVoxelMemoryUsage memoryUsage = residency.GetMemoryUsage();
	stats.residentSceneBytes = memoryUsage.sceneBufferBytes;
	stats.residentAsBytes = memoryUsage.accelerationStructureBytes;
	stats.actionReady = settings.enabled && settings.dryRun;
	stats.planMs = DurationMs(start, std::chrono::steady_clock::now());

	if (ShouldEmitPreloadTrace(settings))
	{
		stats.emitted = true;
		Printf("NRI PT voxel compute preload: event=plan level=%s build_serial=%llu frame=%u enabled=%u dry_run=%u action=%s variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u ms=%.3f\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			settings.enabled ? 1u : 0u,
			settings.dryRun ? 1u : 0u,
			stats.actionReady ? "dry-run" : (settings.enabled ? "future-work" : "disabled"),
			stats.variants,
			stats.required,
			stats.optional,
			stats.selected,
			stats.selectedRequired,
			stats.selectedOptional,
			stats.uniqueMeshes,
			stats.uniqueMaterials,
			stats.surfaceReady,
			stats.directOnly,
			stats.sourceReady,
			stats.materialContextReady,
			stats.meshResident,
			stats.materialResident,
			stats.blasReady,
			stats.ready,
			stats.notReady,
			stats.materialRowsPlanned,
			(unsigned long long)stats.estimatedGeometryBytes,
			(unsigned long long)stats.selectedGeometryBytes,
			(unsigned long long)stats.residentSceneBytes,
			(unsigned long long)stats.residentAsBytes,
			stats.skippedDisabled,
			stats.skippedRequiredOff,
			stats.skippedOptionalOff,
			stats.skippedByteBudget,
			stats.skippedJobBudget,
			stats.skippedMaterialBudget,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
		Printf("PERF pt voxel preload summary NRI: frame=%u enabled=%u dry_run=%u variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u plan_ms=%.3f\n",
			frameIndex,
			settings.enabled ? 1u : 0u,
			settings.dryRun ? 1u : 0u,
			stats.variants,
			stats.required,
			stats.optional,
			stats.selected,
			stats.selectedRequired,
			stats.selectedOptional,
			stats.uniqueMeshes,
			stats.uniqueMaterials,
			stats.surfaceReady,
			stats.directOnly,
			stats.sourceReady,
			stats.materialContextReady,
			stats.meshResident,
			stats.materialResident,
			stats.blasReady,
			stats.ready,
			stats.notReady,
			stats.materialRowsPlanned,
			(unsigned long long)stats.estimatedGeometryBytes,
			(unsigned long long)stats.selectedGeometryBytes,
			(unsigned long long)stats.residentSceneBytes,
			(unsigned long long)stats.residentAsBytes,
			stats.skippedDisabled,
			stats.skippedRequiredOff,
			stats.skippedOptionalOff,
			stats.skippedByteBudget,
			stats.skippedJobBudget,
			stats.skippedMaterialBudget,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
	}

	return stats;
}
