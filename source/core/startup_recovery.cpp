#include "startup_recovery.h"

#include "cmdlib.h"
#include "c_cvars.h"
#include "gameconfigfile.h"
#include "i_specialpaths.h"
#include "m_argv.h"
#include "printf.h"
#include "version.h"

#ifdef HAVE_NRI
#include "nri/renderer/nri_settings_profiles.h"
#endif

#include <cstdlib>
#include <cstring>

EXTERN_CVAR(String, nri_api)

#ifdef HAVE_NRI
EXTERN_CVAR(Int, nri_settingsprofile)
#endif

namespace
{
constexpr const char* kRunStateStarting = "starting";
constexpr const char* kRunStateHealthy = "healthy";
constexpr const char* kRunStateCleanExit = "clean_exit";
constexpr const char* kRunStateStartupFailed = "startup_failed";
constexpr const char* kRunStateDeviceLost = "device_lost";

struct RecoveryState
{
	FString path;
	FString selectedApi = "d3d12";
	FString build;
	bool loaded = false;
	bool begun = false;
	bool recoveryActive = false;
	bool deviceLostThisRun = false;
	bool healthyThisRun = false;
	int runId = 0;
};

RecoveryState gRecovery;

bool IsApiName(const char* api, const char* expected)
{
	return api != nullptr && stricmp(api, expected) == 0;
}

const char* NormalizeApi(const char* api)
{
	return IsApiName(api, "vulkan") ? "vulkan" : "d3d12";
}

const char* ApiSection(const char* api)
{
	return IsApiName(api, "vulkan") ? "Api.vulkan" : "Api.d3d12";
}

bool SameBuild(const char* storedBuild)
{
	return storedBuild == nullptr || storedBuild[0] == '\0' || gRecovery.build.Compare(storedBuild) == 0;
}

bool IsBadApiState(const char* state)
{
	return state != nullptr &&
		(stricmp(state, "unsupported") == 0 ||
		 stricmp(state, "startup_failed") == 0 ||
		 stricmp(state, "device_lost") == 0);
}

bool IsBadRunState(const char* state)
{
	return state != nullptr &&
		(stricmp(state, kRunStateStarting) == 0 ||
		 stricmp(state, kRunStateStartupFailed) == 0 ||
		 stricmp(state, kRunStateDeviceLost) == 0);
}

bool IsGoodRunState(const char* state)
{
	return state != nullptr &&
		(stricmp(state, kRunStateHealthy) == 0 ||
		 stricmp(state, kRunStateCleanExit) == 0);
}

FString GetBuildSignature()
{
	return FStringf("%s:%s", GetVersionString(), GetGitHash());
}

FString GetRecoveryPath()
{
	FString path = GameConfig != nullptr ? GameConfig->GetPathName() : M_GetConfigPath(false);
	const char* chars = path.GetChars();
	const char* slash = strrchr(chars, '/');
	const char* backslash = strrchr(chars, '\\');
	const char* sep = slash > backslash ? slash : backslash;
	const char* dot = strrchr(chars, '.');
	if (dot != nullptr && (sep == nullptr || dot > sep))
	{
		path.Truncate((size_t)(dot - chars));
	}
	path << "-recovery.ini";
	return path;
}

const char* FindStartupSetOverride(const char* name)
{
	if (Args == nullptr || name == nullptr || name[0] == '\0')
	{
		return nullptr;
	}

	const char* value = nullptr;
	for (int i = 1; i + 2 < Args->NumArgs(); ++i)
	{
		if (stricmp(Args->GetArg(i), "+set") != 0)
		{
			continue;
		}

		const char* key = Args->GetArg(i + 1);
		const char* candidate = Args->GetArg(i + 2);
		if (key != nullptr && candidate != nullptr && stricmp(key, name) == 0)
		{
			value = candidate;
		}
	}
	return value;
}

int ReadInt(FConfigFile& file, const char* section, const char* key, int fallback)
{
	if (!file.SetSection(section))
	{
		return fallback;
	}
	const char* value = file.GetValueForKey(key);
	return value != nullptr ? (int)strtol(value, nullptr, 10) : fallback;
}

const char* ReadString(FConfigFile& file, const char* section, const char* key)
{
	if (!file.SetSection(section))
	{
		return nullptr;
	}
	return file.GetValueForKey(key);
}

void SetInt(FConfigFile& file, const char* key, int value)
{
	file.SetValueForKey(key, FStringf("%d", value));
}

int GetCurrentSettingsProfile()
{
#ifdef HAVE_NRI
	return (int)nri_settingsprofile;
#else
	return 0;
#endif
}

void EnsureLoaded()
{
	if (gRecovery.loaded)
	{
		return;
	}

	gRecovery.loaded = true;
	gRecovery.build = GetBuildSignature();
	gRecovery.path = GetRecoveryPath();
	gRecovery.selectedApi = "d3d12";
}

void WriteRunState(const char* state, const char* stage, const char* reason)
{
	EnsureLoaded();
	FConfigFile file(gRecovery.path.GetChars());

	file.SetSection("Run", true);
	SetInt(file, "id", gRecovery.runId);
	file.SetValueForKey("state", state != nullptr ? state : "");
	file.SetValueForKey("stage", stage != nullptr ? stage : "");
	file.SetValueForKey("reason", reason != nullptr ? reason : "");
	file.SetValueForKey("build", gRecovery.build);
	file.SetValueForKey("nri_api", gRecovery.selectedApi);
	SetInt(file, "nri_settingsprofile", GetCurrentSettingsProfile());
	file.SetValueForKey("recovery_active", gRecovery.recoveryActive ? "true" : "false");

	file.WriteConfigFile();
}

void WriteApiState(const char* api, const char* state, const char* reason, const char* adapterName, bool success)
{
	EnsureLoaded();
	const char* normalizedApi = NormalizeApi(api);
	const char* section = ApiSection(normalizedApi);
	FConfigFile file(gRecovery.path.GetChars());

	const int successCount = ReadInt(file, section, "success_count", 0);
	const int failureCount = ReadInt(file, section, "failure_count", 0);

	file.SetSection(section, true);
	file.SetValueForKey("state", state != nullptr ? state : "");
	file.SetValueForKey("build", gRecovery.build);
	file.SetValueForKey("last_reason", reason != nullptr ? reason : "");
	file.SetValueForKey("last_adapter", adapterName != nullptr ? adapterName : "");
	SetInt(file, "success_count", successCount + (success ? 1 : 0));
	SetInt(file, "failure_count", failureCount + (success ? 0 : 1));
	SetInt(file, success ? "last_good_run" : "last_bad_run", gRecovery.runId);
	file.WriteConfigFile();
}

bool IsApiBadForCurrentBuild(FConfigFile& file, const char* api)
{
	const char* section = ApiSection(api);
	const char* state = ReadString(file, section, "state");
	const char* build = ReadString(file, section, "build");
	return SameBuild(build) && IsBadApiState(state);
}

bool IsManualApiRetryAfterGoodRun(FConfigFile& file, const char* requestedApi)
{
	const char* previousState = ReadString(file, "Run", "state");
	const char* previousBuild = ReadString(file, "Run", "build");
	const char* previousApi = ReadString(file, "Run", "nri_api");
	return SameBuild(previousBuild) &&
		IsGoodRunState(previousState) &&
		previousApi != nullptr &&
		stricmp(NormalizeApi(previousApi), requestedApi) != 0;
}

FString ChooseApi(FConfigFile& file, const char* configuredApi)
{
	const char* explicitApi = FindStartupSetOverride("nri_api");
	if (explicitApi != nullptr && explicitApi[0] != '\0')
	{
		return NormalizeApi(explicitApi);
	}

	const char* requestedApi = NormalizeApi(configuredApi);
	const char* alternateApi = IsApiName(requestedApi, "d3d12") ? "vulkan" : "d3d12";
	if (IsManualApiRetryAfterGoodRun(file, requestedApi))
	{
		return requestedApi;
	}
	if (!IsApiBadForCurrentBuild(file, requestedApi))
	{
		return requestedApi;
	}
	if (!IsApiBadForCurrentBuild(file, alternateApi))
	{
		return alternateApi;
	}
	return requestedApi;
}

void ApplySafeProfile()
{
#ifdef HAVE_NRI
	NRIApplySafeSettingsProfileForRecovery();
#endif
}
}

void StartupRecovery_Begin()
{
	EnsureLoaded();
	FConfigFile file(gRecovery.path.GetChars());

	const char* previousState = ReadString(file, "Run", "state");
	const char* previousStage = ReadString(file, "Run", "stage");
	const char* previousReason = ReadString(file, "Run", "reason");
	const char* previousBuild = ReadString(file, "Run", "build");
	const bool previousRunFailed = SameBuild(previousBuild) && IsBadRunState(previousState);

	gRecovery.runId = ReadInt(file, "Run", "id", 0) + 1;
	gRecovery.selectedApi = ChooseApi(file, (const char*)nri_api);
	gRecovery.recoveryActive = previousRunFailed;
	gRecovery.begun = true;

	if (previousRunFailed)
	{
		ApplySafeProfile();
		Printf(PRINT_NOTIFY,
			"Startup recovery: previous run state=%s stage=%s reason=%s; applying Safe Mode profile and selecting NRI API '%s'.\n",
			previousState != nullptr ? previousState : "unknown",
			previousStage != nullptr ? previousStage : "unknown",
			previousReason != nullptr ? previousReason : "unknown",
			gRecovery.selectedApi.GetChars());
	}

	WriteRunState(kRunStateStarting, "startup", previousRunFailed ? "recovering-from-previous-failure" : "");
}

void StartupRecovery_UpdateStage(const char* stage)
{
	if (!gRecovery.begun || gRecovery.deviceLostThisRun || gRecovery.healthyThisRun)
	{
		return;
	}
	WriteRunState(kRunStateStarting, stage, "");
}

const char* StartupRecovery_GetStartupNriAPI(const char* configuredApi)
{
	EnsureLoaded();
	const char* explicitApi = FindStartupSetOverride("nri_api");
	if (explicitApi != nullptr && explicitApi[0] != '\0')
	{
		gRecovery.selectedApi = NormalizeApi(explicitApi);
		return gRecovery.selectedApi.GetChars();
	}

	if (!gRecovery.begun)
	{
		FConfigFile file(gRecovery.path.GetChars());
		gRecovery.selectedApi = ChooseApi(file, configuredApi);
	}
	return gRecovery.selectedApi.GetChars();
}

void StartupRecovery_MarkNriCreateResult(const char* api, bool success, const char* reason, bool unsupported, const char* adapterName)
{
	const char* state = success ? "startup_ok" : unsupported ? "unsupported" : "startup_failed";
	WriteApiState(api, state, reason, adapterName, success);
	if (!success)
	{
		WriteRunState(kRunStateStartupFailed, "nri_create_device", reason);
	}
}

void StartupRecovery_MarkNriStartupFailure(const char* stage, const char* reason)
{
	WriteApiState(gRecovery.selectedApi.GetChars(), "startup_failed", reason, nullptr, false);
	WriteRunState(kRunStateStartupFailed, stage, reason);
}

void StartupRecovery_MarkNriHealthy()
{
	if (gRecovery.deviceLostThisRun || gRecovery.healthyThisRun)
	{
		return;
	}
	gRecovery.healthyThisRun = true;
	WriteApiState(gRecovery.selectedApi.GetChars(), "healthy", "presented-frames", nullptr, true);
	WriteRunState(kRunStateHealthy, "runtime", "");
}

void StartupRecovery_MarkNriDeviceLost(const char* stage)
{
	gRecovery.deviceLostThisRun = true;
	gRecovery.healthyThisRun = false;
	WriteApiState(gRecovery.selectedApi.GetChars(), "device_lost", stage, nullptr, false);
	WriteRunState(kRunStateDeviceLost, stage, "device_lost");
}

void StartupRecovery_MarkCleanExit()
{
	if (!gRecovery.begun || gRecovery.deviceLostThisRun)
	{
		return;
	}
	WriteRunState(kRunStateCleanExit, "shutdown", "");
}
