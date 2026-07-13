#include "nri_renderer_settings.h"
#include "nri_cvars.h"

#include "c_cvars.h"
#include "printf.h"

#include <algorithm>



namespace
{
	uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	uint32_t ClampNrdHistoryFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	}

	uint32_t ClampNrdFastFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	uint32_t ClampNrdStabilizationFrameCount(int value, uint32_t maxAccumulatedFrameNum)
	{
		return (uint32_t)std::clamp(value, 0, (int)maxAccumulatedFrameNum);
	}

	uint32_t ClampSigmaStabilizationFrameCount(int value)
	{
		return (uint32_t)std::clamp(value, 0, (int)nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
	}

	float ClampNrdFastHistorySigmaScale(float value)
	{
		return std::clamp(value, 1.0f, 3.0f);
	}

	float ClampNrdPrepassBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 75.0f);
	}

	float ClampNrdBlurRadius(float value)
	{
		return std::clamp(value, 0.0f, 60.0f);
	}

	float ClampSigmaPlaneDistanceSensitivity(float value)
	{
		return std::clamp(value, 0.001f, 0.1f);
	}

	void MigratePreloadSettingsCVars()
	{
		constexpr int CurrentLoadingSettingsVersion = 3;
		const int loadingSettingsVersion = (int)nri_ptloadingsettingsversion;
		if (loadingSettingsVersion >= CurrentLoadingSettingsVersion)
		{
			return;
		}

		bool migrated = false;
		if (loadingSettingsVersion < 1 && (int)nri_ptloadingvoxelvariants == 128)
		{
			nri_ptloadingvoxelvariants = 256;
			migrated = true;
		}
		if (loadingSettingsVersion < 1 && (int)nri_ptloadingvoxelcpumaxvariants == 64)
		{
			nri_ptloadingvoxelcpumaxvariants = 256;
			migrated = true;
		}
		if (loadingSettingsVersion < 2 && (int)nri_ptpreloadmaxsubmitspertick == 8)
		{
			nri_ptpreloadmaxsubmitspertick = 2;
			migrated = true;
		}
		if (loadingSettingsVersion < 3)
		{
			if ((bool)nri_ptloadingvoxelcpu) nri_ptloadingvoxelcpu = false;
			if ((bool)nri_ptloadingvoxelgpuwhitelistonly) nri_ptloadingvoxelgpuwhitelistonly = false;
			if ((int)nri_ptloadingvoxelvariants == 256) nri_ptloadingvoxelvariants = 0;
			if ((int)nri_ptpersistentvoxelbuildactors == 0) nri_ptpersistentvoxelbuildactors = 1;
			if ((int)nri_ptvoxeladmissionloadvariants == 4) nri_ptvoxeladmissionloadvariants = 8;
			if ((int)nri_ptvoxeladmissionruntimevariants == 1) nri_ptvoxeladmissionruntimevariants = 4;
			if ((int)nri_ptvoxeladmissionruntimebytes == 16 * 1024 * 1024) nri_ptvoxeladmissionruntimebytes = 32 * 1024 * 1024;
			if ((int)nri_ptvoxelpreloadreadygraceframes == 16) nri_ptvoxelpreloadreadygraceframes = 0;
			if ((int)nri_ptvoxeladmitmaxbytesruntime == 16 * 1024 * 1024) nri_ptvoxeladmitmaxbytesruntime = 32 * 1024 * 1024;
			if ((int)nri_ptvoxeladmitmaxblasloading == 4) nri_ptvoxeladmitmaxblasloading = 8;
			if ((int)nri_ptvoxeladmitmaxblasruntime == 1) nri_ptvoxeladmitmaxblasruntime = 4;
			migrated = true;
		}

		nri_ptloadingsettingsversion = CurrentLoadingSettingsVersion;
		if (migrated)
		{
			Printf("NRI preload config: migrated archived preload defaults through version %d\n", CurrentLoadingSettingsVersion);
		}
	}
}

NRITraceSettings BuildNRITraceSettingsFromCVars()
{
	NRITraceSettings settings = {};
	settings.lightBounceCount = ClampTraceBounceCount((int)nri_ptlightbounces, 4u);
	settings.mirrorBounceCount = ClampTraceBounceCount((int)nri_ptmirrorbounces, 8u);
	settings.portalDepth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	settings.emissiveSampleCount = std::max<uint32_t>(ClampTraceBounceCount((int)nri_ptemissivesamples, 4u), 1u);
	return settings;
}

NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars()
{
	NRIDenoiserSettings settings = {};
	settings.denoiserMode = (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	settings.maxAccumulatedFrameNum = ClampNrdHistoryFrameCount((int)nri_nrdmaxframes);
	settings.maxFastAccumulatedFrameNum = ClampNrdFastFrameCount((int)nri_nrdfastframes, settings.maxAccumulatedFrameNum);
	settings.maxStabilizedFrameNum = ClampNrdStabilizationFrameCount((int)nri_nrdstabilizationframes, settings.maxAccumulatedFrameNum);
	settings.sigmaMaxStabilizedFrameNum = ClampSigmaStabilizationFrameCount((int)nri_nrdsigmastabilization);
	settings.hitDistanceReconstructionMode = (uint32_t)std::clamp((int)nri_nrdhitdistrecon, 0, 2);
	settings.inputSplitMode = (uint32_t)std::clamp((int)nri_nrdsplit, 0, 2);
	settings.fastHistoryClampingSigmaScale = ClampNrdFastHistorySigmaScale((float)nri_nrdfasthistorysigma);
	settings.diffusePrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassdiffuse);
	settings.specularPrepassBlurRadius = ClampNrdPrepassBlurRadius((float)nri_nrdprepassspecular);
	settings.minBlurRadius = ClampNrdBlurRadius((float)nri_nrdblurmin);
	settings.maxBlurRadius = std::max(settings.minBlurRadius, ClampNrdBlurRadius((float)nri_nrdblurmax));
	settings.sigmaPlaneDistanceSensitivity = ClampSigmaPlaneDistanceSensitivity((float)nri_nrdsigmaplanedistance);
	settings.enableAntiFirefly = nri_nrdantifirefly;
	settings.enableValidation = nri_validation;
	return settings;
}

NRIPersistentVoxelSettings BuildNRIPersistentVoxelSettingsFromCVars()
{
	MigratePreloadSettingsCVars();

	NRIPersistentVoxelSettings settings = {};
	settings.buildActors = (uint32_t)std::max(1, (int)nri_ptpersistentvoxelbuildactors);
	settings.buildPrimitives = (int)nri_ptpersistentvoxelbuildprims <= 0 ? 0u : (uint32_t)(int)nri_ptpersistentvoxelbuildprims;
	settings.buildBytes = (int)nri_ptpersistentvoxelbuildbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptpersistentvoxelbuildbytes;
	settings.texturePrewarms = (int)nri_ptpersistentvoxeltextureprewarms <= 0 ? 0u : (uint32_t)(int)nri_ptpersistentvoxeltextureprewarms;
	settings.textureBytes = (int)nri_ptpersistentvoxeltexturebytes <= 0 ? 0ull : (uint64_t)(int)nri_ptpersistentvoxeltexturebytes;
	settings.runtimeBudgetMode = (uint32_t)std::clamp((int)nri_ptvoxelruntimebudget, 0, 4);
	settings.admissionLoadVariants = (int)nri_ptvoxeladmissionloadvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmissionloadvariants;
	settings.admissionLoadBytes = (int)nri_ptvoxeladmissionloadbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmissionloadbytes;
	settings.admissionRuntimeVariants = (int)nri_ptvoxeladmissionruntimevariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmissionruntimevariants;
	settings.admissionRuntimeBytes = (int)nri_ptvoxeladmissionruntimebytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmissionruntimebytes;
	settings.admissionGraceFrames = (uint32_t)std::max(0, (int)nri_ptvoxeladmissiongraceframes);
	settings.admissionGraceVariants = (int)nri_ptvoxeladmissiongracevariants <= 0 ? 0u : (uint32_t)(int)nri_ptvoxeladmissiongracevariants;
	settings.preloadReadyGraceFrames = (uint32_t)std::max(0, (int)nri_ptvoxelpreloadreadygraceframes);
	settings.admitMaxBytesLoading = (int)nri_ptvoxeladmitmaxbytesloading <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmitmaxbytesloading;
	settings.admitMaxBytesRuntime = (int)nri_ptvoxeladmitmaxbytesruntime <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxeladmitmaxbytesruntime;
	settings.admitMaxMsLoading = (uint32_t)std::max(0, (int)nri_ptvoxeladmitmaxmsloading);
	settings.admitMaxMsRuntime = (uint32_t)std::max(0, (int)nri_ptvoxeladmitmaxmsruntime);
	settings.admitMaxBlasLoading = (int)nri_ptvoxeladmitmaxblasloading <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasloading;
	settings.admitMaxBlasRuntime = (int)nri_ptvoxeladmitmaxblasruntime <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasruntime;
	settings.admitMaxBlasPrimitives = (int)nri_ptvoxeladmitmaxblasprims <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxeladmitmaxblasprims;
	settings.admitIsolateBlasPrimitives = (int)nri_ptvoxeladmitisolateblasprims;
	settings.computeMaxJobs = (uint32_t)std::max(1, (int)nri_ptvoxelcomputemaxjobs);
	settings.residentMaxBytes = (int)nri_ptvoxelresidentmaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelresidentmaxbytes;
	settings.residentMinHeadroomBytes = (int)nri_ptvoxelresidentminheadroombytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelresidentminheadroombytes;
	settings.residentMaxColdMaps = (int)nri_ptvoxelresidentmaxcoldmaps < 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxelresidentmaxcoldmaps;
	settings.trimColdOnLoading = (bool)nri_ptvoxeltrimcoldloading;
	settings.sharedBlasBuildEnabled = (bool)nri_ptvoxelsharedblasbuild;
	settings.sharedBlasBuildsPerFrame = (int)nri_ptvoxelsharedblasbuilds <= 0 ? 0u : (uint32_t)(int)nri_ptvoxelsharedblasbuilds;
	settings.sharedBlasLoadingWarmupEnabled = (bool)nri_ptvoxelsharedblasloading;
	settings.sharedBlasRouteEnabled = (bool)nri_ptvoxelsharedblasroute;
	settings.transformKeyed = (bool)nri_ptvoxeltransformkeyed;
	settings.diagnosticsEnabled =
		(bool)nri_voxelstats ||
		(int)nri_pttraceframes > 0 ||
		(int)perf_looptraceframes > 0 ||
		(bool)nri_ptslowdowntrace;
	settings.excludeIndices = {
		(int32_t)(int)nri_ptvoxelexcludeindex,
		(int32_t)(int)nri_ptvoxelexcludeindex2,
		(int32_t)(int)nri_ptvoxelexcludeindex3
	};
	settings.excludeMinPrimitives = (uint32_t)std::max(0, (int)nri_ptvoxelexcludeminprims);
	return settings;
}

NRIRuntimeMutationSettings BuildNRIRuntimeMutationSettingsFromCVars()
{
	NRIRuntimeMutationSettings settings = {};
	settings.worklistEnabled = (bool)nri_ptruntimeworklist;
	settings.worklistSweepBudget = (uint32_t)std::max(0, (int)nri_ptruntimeworklistsweepbudget);
	settings.deferFarMaterialRefreshes = (bool)nri_ptruntimedeferfarmaterial;
	settings.deferNearInvisibleMaterialRefreshes = (bool)nri_ptruntimedefernearinvisiblematerial;
	settings.nearInvisibleMaterialBudget = (uint32_t)std::max(0, (int)nri_ptruntimenearinvisiblematerialbudget);
	settings.deferFarStructuralRebuilds = (bool)nri_ptruntimedeferfarstructural;
	settings.farStructuralBudget = (uint32_t)std::max(0, (int)nri_ptruntimefarstructuralbudget);
	settings.deferNearInvisibleStructuralRebuilds = (bool)nri_ptruntimedefernearinvisiblestructural;
	settings.nearInvisibleStructuralBudget = (uint32_t)std::max(0, (int)nri_ptruntimenearinvisiblestructuralbudget);
	settings.nearDistance = std::max(0.0f, (float)nri_ptruntimemutationneardistance);
	return settings;
}

NRISmokeSettings BuildNRISmokeSettingsFromCVars()
{
	NRISmokeSettings settings = {};
	settings.enabled = (bool)nri_ptsmoke;
	settings.readback = (bool)nri_ptsmokereadback;
	settings.quality = (uint32_t)std::clamp((int)nri_ptsmokequality, 0, 2);
	settings.particleCapacity = (uint32_t)std::clamp((int)nri_ptsmokeparticles, 256, 65536);
	settings.froxelPixelSize = (uint32_t)std::clamp((int)nri_ptsmokefroxelpixels, 4, 64);
	settings.froxelDepth = (uint32_t)std::clamp((int)nri_ptsmokefroxelz, 8, 128);
	settings.columnCapacity = (uint32_t)std::clamp((int)nri_ptsmokecolumncapacity, 8, 256);
	settings.simulationRate = (uint32_t)std::clamp((int)nri_ptsmokesimrate, 15, 240);
	settings.maxSubsteps = (uint32_t)std::clamp((int)nri_ptsmokemaxsubsteps, 1, 8);
	settings.pointLights = (bool)nri_ptsmokepointlights;
	settings.directionalLight = (bool)nri_ptsmokedirectionallight;
	settings.emissiveLights = (bool)nri_ptsmokeemissivelights;
	settings.indirect = (bool)nri_ptsmokeindirect;
	settings.indirectCacheMode = (uint32_t)std::clamp((int)nri_ptsmokeindirectcache, 0, 3);
	settings.lightMode = (uint32_t)std::clamp((int)nri_ptsmokelightmode, 0, 3);
	settings.lightSamples = (uint32_t)std::clamp((int)nri_ptsmokelightsamples, 1, 4);
	settings.maxLightCandidates = (uint32_t)std::clamp((int)nri_ptsmokemaxlightcandidates, 1, 32);
	settings.filteredVisibility = (bool)nri_ptsmokefilteredvisibility;
	settings.debugMode = (uint32_t)std::clamp((int)nri_ptsmokedebug, 0, 7);
	settings.traceMode = (uint32_t)std::clamp((int)nri_ptsmoketrace, 0, 2);
	settings.froxelMaxDistance = std::clamp((float)nri_ptsmokefroxelmaxdistance, 64.0f, 32768.0f);
	settings.timeScale = std::clamp((float)nri_ptsmoketimescale, 0.0f, 4.0f);
	settings.wind[0] = std::clamp((float)nri_ptsmokewindx, -256.0f, 256.0f);
	settings.wind[1] = std::clamp((float)nri_ptsmokewindy, -256.0f, 256.0f);
	settings.wind[2] = std::clamp((float)nri_ptsmokewindz, -256.0f, 256.0f);
	settings.densityScale = std::clamp((float)nri_ptsmokedensityscale, 0.0f, 16.0f);
	settings.radianceScale = std::clamp((float)nri_ptsmokeradiancescale, 0.0f, 16.0f);
	settings.indirectScale = std::clamp((float)nri_ptsmokeindirectscale, 0.0f, 16.0f);
	return settings;
}
