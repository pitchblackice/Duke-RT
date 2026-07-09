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

	bool IsRequiredComputePreloadRawVariant(const nri_scene::PrecachedVoxelRawManifestView& variant)
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

	uint64_t EstimateRawVariantGeometryBytes(const nri_scene::PrecachedVoxelRawManifestView& variant)
	{
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

void BuildNRIVoxelComputePreloadDirectVariants(
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const NRIVoxelComputePreloadSettings& settings,
	std::vector<nri_scene::PrecachedVoxelVariantView>& outVariants)
{
	outVariants.clear();
	if (!settings.enabled || settings.dryRun)
	{
		return;
	}

	uint64_t selectedBytes = 0;
	for (const nri_scene::PrecachedVoxelRawManifestView& rawVariant : rawVariants)
	{
		const bool required = IsRequiredComputePreloadRawVariant(rawVariant);
		if (required && !settings.includeRequired)
		{
			continue;
		}
		if (!required && !settings.includeOptional)
		{
			continue;
		}
		if (!rawVariant.rawSourceResident ||
			!rawVariant.materialContextReady ||
			rawVariant.primitiveCount == 0 ||
			rawVariant.meshKeyHash == 0 ||
			rawVariant.materialKeyHash == 0 ||
			rawVariant.model == nullptr)
		{
			continue;
		}

		const uint64_t estimatedBytes = EstimateRawVariantGeometryBytes(rawVariant);
		if (settings.maxJobs != 0 && outVariants.size() >= settings.maxJobs)
		{
			continue;
		}
		if (settings.maxBytes != 0 && selectedBytes + estimatedBytes > settings.maxBytes)
		{
			continue;
		}

		nri_scene::PrecachedVoxelVariantView variant = {};
		variant.meshKeyHash = rawVariant.meshKeyHash;
		variant.materialKeyHash = rawVariant.materialKeyHash;
		variant.geometrySignature = rawVariant.meshVariantHash != 0 ? rawVariant.meshVariantHash : rawVariant.meshKeyHash;
		variant.meshVariantHash = rawVariant.meshVariantHash;
		variant.materialVariantHash = rawVariant.materialVariantHash;
		variant.sourceBits = rawVariant.sourceBits;
		variant.priority = rawVariant.priority;
		variant.admissionRank = rawVariant.admissionRank;
		variant.sourcePicnum = rawVariant.sourcePicnum;
		variant.resolvedVoxelIndex = rawVariant.resolvedVoxelIndex;
		variant.primitiveCount = rawVariant.primitiveCount;
		variant.gpuForce = rawVariant.gpuForce;
		variant.gpuPrefer = rawVariant.gpuPrefer;
		variant.model = rawVariant.model;
		variant.surface = nullptr;
		variant.material = rawVariant.material;
		variant.materialSurface.material = rawVariant.material;
		variant.materialSurface.provenance.sourceType = nri_scene::SurfaceSourceType::VoxelProxySprite;
		variant.materialSurface.provenance.materialFlags = rawVariant.material.flags;
		variant.directOnlyAdmission = true;
		outVariants.push_back(std::move(variant));
		selectedBytes += estimatedBytes;
	}
}

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const nri_scene::PrecachedVoxelRawManifestStats& rawManifestStats,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const char* timelineStage)
{
	const auto start = std::chrono::steady_clock::now();
	NRIVoxelComputePreloadStats stats = {};
	stats.enabled = settings.enabled;
	stats.dryRun = settings.dryRun;
	stats.variants = (uint32_t)variants.size();
	stats.rawVariants = (uint32_t)rawVariants.size();
	stats.manifestSources = rawManifestStats.manifestSources;
	stats.manifestLines = rawManifestStats.manifestLines;
	stats.manifestRequests = rawManifestStats.manifestRequests;
	stats.manifestSkippedInactive = rawManifestStats.manifestSkippedInactive;
	stats.manifestSkippedSyntax = rawManifestStats.manifestSkippedSyntax;
	stats.manifestSkippedActor = rawManifestStats.manifestSkippedActor;
	stats.manifestSkippedUnsupported = rawManifestStats.manifestSkippedUnsupported;
	stats.manifestDiscovered = rawManifestStats.discovered;
	stats.manifestUniqueRequests = rawManifestStats.uniqueRequests;
	stats.manifestSkippedInvalid = rawManifestStats.skippedInvalid;
	stats.manifestSkippedDuplicate = rawManifestStats.skippedDuplicate;

	std::unordered_set<uint64_t> uniqueMeshes;
	std::unordered_set<uint64_t> uniqueMaterials;
	std::unordered_set<uint64_t> selectedMaterials;
	std::unordered_set<uint64_t> rawUniqueMeshes;
	std::unordered_set<uint64_t> rawUniqueMaterials;
	std::unordered_set<uint64_t> rawRequiredMaterials;
	std::unordered_set<uint64_t> rawOptionalMaterials;
	std::unordered_set<uint64_t> rawSelectedMaterials;
	std::unordered_set<uint64_t> rawActorScopedMaterials;
	uniqueMeshes.reserve(variants.size());
	uniqueMaterials.reserve(variants.size());
	selectedMaterials.reserve(variants.size());
	rawUniqueMeshes.reserve(rawVariants.size());
	rawUniqueMaterials.reserve(rawVariants.size());
	rawRequiredMaterials.reserve(rawVariants.size());
	rawOptionalMaterials.reserve(rawVariants.size());
	rawSelectedMaterials.reserve(rawVariants.size());
	rawActorScopedMaterials.reserve(rawVariants.size());

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

	for (const nri_scene::PrecachedVoxelRawManifestView& variant : rawVariants)
	{
		const bool required = IsRequiredComputePreloadRawVariant(variant);
		if (required)
		{
			stats.rawRequired++;
		}
		else
		{
			stats.rawOptional++;
		}

		if (variant.meshKeyHash != 0)
		{
			rawUniqueMeshes.insert(variant.meshKeyHash);
		}
		if (variant.materialKeyHash != 0)
		{
			rawUniqueMaterials.insert(variant.materialKeyHash);
			if (required)
			{
				rawRequiredMaterials.insert(variant.materialKeyHash);
			}
			else
			{
				rawOptionalMaterials.insert(variant.materialKeyHash);
			}
			if (variant.materialVariantHash != 0 && variant.materialVariantHash != variant.materialKeyHash)
			{
				rawActorScopedMaterials.insert(variant.materialKeyHash);
			}
		}
		if (variant.material.texture != nullptr)
		{
			stats.rawMaterialTextureRefs++;
		}
		if (variant.rawSourceResident)
		{
			stats.rawSourceResident++;
		}
		else
		{
			stats.rawSourceMissing++;
		}
		if (variant.materialContextReady)
		{
			stats.rawMaterialContextReady++;
		}
		else
		{
			stats.rawMaterialContextMissing++;
		}
		if (variant.cpuSurfaceReady)
		{
			stats.rawCpuSurfaceReady++;
		}
		if (variant.legacyGpuCandidate)
		{
			stats.rawLegacyGpuCandidate++;
		}
		else
		{
			stats.rawLegacyGpuSourceSkipped++;
		}

		const uint64_t estimatedBytes = EstimateRawVariantGeometryBytes(variant);
		stats.rawEstimatedGeometryBytes += estimatedBytes;
		if (!settings.enabled)
		{
			stats.rawSkippedDisabled++;
			continue;
		}
		if (required && !settings.includeRequired)
		{
			stats.rawSkippedRequiredOff++;
			continue;
		}
		if (!required && !settings.includeOptional)
		{
			stats.rawSkippedOptionalOff++;
			continue;
		}
		if (!variant.rawSourceResident || variant.primitiveCount == 0)
		{
			stats.rawSkippedSourceMissing++;
			continue;
		}
		if (!variant.materialContextReady)
		{
			stats.rawSkippedMaterialMissing++;
			continue;
		}
		if (settings.maxJobs != 0 && stats.rawSelected >= settings.maxJobs)
		{
			stats.rawSkippedJobBudget++;
			continue;
		}
		if (settings.maxBytes != 0 && stats.rawSelectedGeometryBytes + estimatedBytes > settings.maxBytes)
		{
			stats.rawSkippedByteBudget++;
			continue;
		}

		stats.rawSelected++;
		stats.rawSelectedGeometryBytes += estimatedBytes;
		if (variant.materialKeyHash != 0)
		{
			rawSelectedMaterials.insert(variant.materialKeyHash);
		}
		if (required)
		{
			stats.rawSelectedRequired++;
		}
		else
		{
			stats.rawSelectedOptional++;
		}
	}

	stats.uniqueMeshes = (uint32_t)uniqueMeshes.size();
	stats.uniqueMaterials = (uint32_t)uniqueMaterials.size();
	stats.rawUniqueMeshes = (uint32_t)rawUniqueMeshes.size();
	stats.rawUniqueMaterials = (uint32_t)rawUniqueMaterials.size();
	stats.rawMaterialRequiredKeys = (uint32_t)rawRequiredMaterials.size();
	stats.rawMaterialOptionalKeys = (uint32_t)rawOptionalMaterials.size();
	stats.rawMaterialSelectedKeys = (uint32_t)rawSelectedMaterials.size();
	stats.rawMaterialActorScopedKeys = (uint32_t)rawActorScopedMaterials.size();
	const NRIPersistentVoxelMemoryUsage memoryUsage = residency.GetMemoryUsage();
	stats.residentSceneBytes = memoryUsage.sceneBufferBytes;
	stats.residentAsBytes = memoryUsage.accelerationStructureBytes;
	stats.actionReady = settings.enabled && settings.dryRun;
	stats.planMs = DurationMs(start, std::chrono::steady_clock::now());

	if (ShouldEmitPreloadTrace(settings))
	{
		stats.emitted = true;
		Printf("NRI PT voxel compute preload: event=plan stage=%s level=%s build_serial=%llu frame=%u enabled=%u dry_run=%u action=%s variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u raw_variants=%u raw_required=%u raw_optional=%u raw_selected=%u raw_selected_required=%u raw_selected_optional=%u raw_unique_meshes=%u raw_unique_materials=%u raw_material_required_keys=%u raw_material_optional_keys=%u raw_material_selected_keys=%u raw_material_actor_scoped_keys=%u raw_material_texture_refs=%u raw_source_resident=%u raw_source_missing=%u raw_material_context=%u raw_material_missing=%u raw_cpu_surface_ready=%u raw_legacy_gpu_candidate=%u raw_legacy_gpu_source_skipped=%u raw_estimated_bytes=%llu raw_selected_bytes=%llu raw_skipped_disabled=%u raw_skipped_required_off=%u raw_skipped_optional_off=%u raw_skipped_source_missing=%u raw_skipped_material_missing=%u raw_skipped_byte_budget=%u raw_skipped_job_budget=%u manifest_sources=%u manifest_lines=%u manifest_requests=%u manifest_discovered=%u manifest_unique=%u manifest_skipped_inactive=%u manifest_skipped_syntax=%u manifest_skipped_actor=%u manifest_skipped_unsupported=%u manifest_skipped_invalid=%u manifest_skipped_duplicate=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u ms=%.3f\n",
			timelineStage != nullptr ? timelineStage : "snapshot",
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
			stats.rawVariants,
			stats.rawRequired,
			stats.rawOptional,
			stats.rawSelected,
			stats.rawSelectedRequired,
			stats.rawSelectedOptional,
			stats.rawUniqueMeshes,
			stats.rawUniqueMaterials,
			stats.rawMaterialRequiredKeys,
			stats.rawMaterialOptionalKeys,
			stats.rawMaterialSelectedKeys,
			stats.rawMaterialActorScopedKeys,
			stats.rawMaterialTextureRefs,
			stats.rawSourceResident,
			stats.rawSourceMissing,
			stats.rawMaterialContextReady,
			stats.rawMaterialContextMissing,
			stats.rawCpuSurfaceReady,
			stats.rawLegacyGpuCandidate,
			stats.rawLegacyGpuSourceSkipped,
			(unsigned long long)stats.rawEstimatedGeometryBytes,
			(unsigned long long)stats.rawSelectedGeometryBytes,
			stats.rawSkippedDisabled,
			stats.rawSkippedRequiredOff,
			stats.rawSkippedOptionalOff,
			stats.rawSkippedSourceMissing,
			stats.rawSkippedMaterialMissing,
			stats.rawSkippedByteBudget,
			stats.rawSkippedJobBudget,
			stats.manifestSources,
			stats.manifestLines,
			stats.manifestRequests,
			stats.manifestDiscovered,
			stats.manifestUniqueRequests,
			stats.manifestSkippedInactive,
			stats.manifestSkippedSyntax,
			stats.manifestSkippedActor,
			stats.manifestSkippedUnsupported,
			stats.manifestSkippedInvalid,
			stats.manifestSkippedDuplicate,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
		Printf("PERF pt voxel preload summary NRI: frame=%u enabled=%u dry_run=%u variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u raw_variants=%u raw_required=%u raw_optional=%u raw_selected=%u raw_selected_required=%u raw_selected_optional=%u raw_unique_meshes=%u raw_unique_materials=%u raw_material_required_keys=%u raw_material_optional_keys=%u raw_material_selected_keys=%u raw_material_actor_scoped_keys=%u raw_material_texture_refs=%u raw_source_resident=%u raw_source_missing=%u raw_material_context=%u raw_material_missing=%u raw_cpu_surface_ready=%u raw_legacy_gpu_candidate=%u raw_legacy_gpu_source_skipped=%u raw_estimated_bytes=%llu raw_selected_bytes=%llu raw_skipped_disabled=%u raw_skipped_required_off=%u raw_skipped_optional_off=%u raw_skipped_source_missing=%u raw_skipped_material_missing=%u raw_skipped_byte_budget=%u raw_skipped_job_budget=%u manifest_sources=%u manifest_lines=%u manifest_requests=%u manifest_discovered=%u manifest_unique=%u manifest_skipped_inactive=%u manifest_skipped_syntax=%u manifest_skipped_actor=%u manifest_skipped_unsupported=%u manifest_skipped_invalid=%u manifest_skipped_duplicate=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u plan_ms=%.3f\n",
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
			stats.rawVariants,
			stats.rawRequired,
			stats.rawOptional,
			stats.rawSelected,
			stats.rawSelectedRequired,
			stats.rawSelectedOptional,
			stats.rawUniqueMeshes,
			stats.rawUniqueMaterials,
			stats.rawMaterialRequiredKeys,
			stats.rawMaterialOptionalKeys,
			stats.rawMaterialSelectedKeys,
			stats.rawMaterialActorScopedKeys,
			stats.rawMaterialTextureRefs,
			stats.rawSourceResident,
			stats.rawSourceMissing,
			stats.rawMaterialContextReady,
			stats.rawMaterialContextMissing,
			stats.rawCpuSurfaceReady,
			stats.rawLegacyGpuCandidate,
			stats.rawLegacyGpuSourceSkipped,
			(unsigned long long)stats.rawEstimatedGeometryBytes,
			(unsigned long long)stats.rawSelectedGeometryBytes,
			stats.rawSkippedDisabled,
			stats.rawSkippedRequiredOff,
			stats.rawSkippedOptionalOff,
			stats.rawSkippedSourceMissing,
			stats.rawSkippedMaterialMissing,
			stats.rawSkippedByteBudget,
			stats.rawSkippedJobBudget,
			stats.manifestSources,
			stats.manifestLines,
			stats.manifestRequests,
			stats.manifestDiscovered,
			stats.manifestUniqueRequests,
			stats.manifestSkippedInactive,
			stats.manifestSkippedSyntax,
			stats.manifestSkippedActor,
			stats.manifestSkippedUnsupported,
			stats.manifestSkippedInvalid,
			stats.manifestSkippedDuplicate,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
	}

	return stats;
}
