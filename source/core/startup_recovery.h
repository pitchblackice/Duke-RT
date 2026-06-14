#pragma once

void StartupRecovery_Begin();
void StartupRecovery_UpdateStage(const char* stage);
const char* StartupRecovery_GetStartupNriAPI(const char* configuredApi);
void StartupRecovery_MarkNriCreateResult(const char* api, bool success, const char* reason, bool unsupported, const char* adapterName);
void StartupRecovery_MarkNriStartupFailure(const char* stage, const char* reason);
void StartupRecovery_MarkNriHealthy();
void StartupRecovery_MarkNriDeviceLost(const char* stage);
void StartupRecovery_MarkCleanExit();
