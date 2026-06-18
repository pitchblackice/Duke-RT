#pragma once

#include <chrono>
#include <cstdint>

class NRIRenderer;

struct NRIPreloadLevelSceneInputs
{
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	bool frameTargetUsed = false;
	bool standaloneContextUsed = false;
};

class NRIPreloadCoordinator
{
public:
	static bool Run(NRIRenderer& renderer, const NRIPreloadLevelSceneInputs& inputs);

private:
	struct Context
	{
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		uint32_t targetWidth = 0;
		uint32_t targetHeight = 0;
		std::chrono::steady_clock::time_point start = {};
		bool staticLightRefreshReady = true;
		bool frameTargetUsed = false;
		bool standaloneContextUsed = false;
	};

	enum class StepResult
	{
		Continue,
		Ready,
		Wait
	};

	static bool HasFrameTarget(NRIRenderer& renderer, const Context& context);
	static bool ShouldSkipForUnsupportedPathTracing(NRIRenderer& renderer, const Context& context);
	static void TraceBegin(NRIRenderer& renderer, const Context& context);
	static bool EnsureFrameResources(NRIRenderer& renderer, const Context& context);
	static void ResetSceneStats(NRIRenderer& renderer);
	static StepResult PreloadStaticSceneAndStartupCorrection(NRIRenderer& renderer, const Context& context);
	static void RefreshStaticLighting(NRIRenderer& renderer, Context& context);
	static StepResult PreloadResidentSceneResources(NRIRenderer& renderer, const Context& context);
	static bool Finish(NRIRenderer& renderer, const Context& context);
};
