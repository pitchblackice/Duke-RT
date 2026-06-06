#include "Include/ExposureConstants.hlsli"

float ResolvePreviousExposure(float fallbackExposure)
{
	const float previous = gPreviousExposureState.Load(int3(0, 0, 0)).x;
	if (isnan(previous) || isinf(previous) || previous <= 0.0)
	{
		return fallbackExposure;
	}

	return previous;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x != 0u || dispatchThreadId.y != 0u || dispatchThreadId.z != 0u)
	{
		return;
	}

	const uint binCount = max(gExposureConstants.HistogramBinCount, 1u);
	uint totalSamples = 0u;
	for (uint bin = 0u; bin < binCount; ++bin)
	{
		totalSamples += gExposureHistogram[bin];
	}

	const float fallbackExposure = max(gExposureConstants.FallbackManualExposure, 0.0);
	float lowLogLuminance = gExposureConstants.LogLuminanceMin;
	float highLogLuminance = gExposureConstants.LogLuminanceMin;
	float meteredLogLuminance = gExposureConstants.LogLuminanceMin;
	uint lowBin = 0u;
	uint highBin = 0u;
	float targetExposure = clamp(fallbackExposure, gExposureConstants.MinExposure, gExposureConstants.MaxExposure);

	if (totalSamples != 0u)
	{
		const uint lowTarget = min((uint)floor((float)(totalSamples - 1u) * saturate(gExposureConstants.LowPercentile * 0.01)), totalSamples - 1u);
		const uint highTarget = min((uint)floor((float)(totalSamples - 1u) * saturate(gExposureConstants.HighPercentile * 0.01)), totalSamples - 1u);

		uint cumulative = 0u;
		for (uint lowScan = 0u; lowScan < binCount; ++lowScan)
		{
			cumulative += gExposureHistogram[lowScan];
			if (cumulative > lowTarget)
			{
				lowBin = lowScan;
				break;
			}
		}

		cumulative = 0u;
		for (uint highScan = 0u; highScan < binCount; ++highScan)
		{
			cumulative += gExposureHistogram[highScan];
			if (cumulative > highTarget)
			{
				highBin = highScan;
				break;
			}
		}

		if (highBin < lowBin)
		{
			highBin = lowBin;
		}

		uint clippedSamples = 0u;
		float weightedLogSum = 0.0;
		for (uint weightedBin = lowBin; weightedBin <= highBin && weightedBin < binCount; ++weightedBin)
		{
			const uint count = gExposureHistogram[weightedBin];
			clippedSamples += count;
			weightedLogSum += (float)count * ExposureBinToLogLuminance(weightedBin);
		}

		if (clippedSamples != 0u)
		{
			meteredLogLuminance = weightedLogSum / (float)clippedSamples;
		}
		else
		{
			meteredLogLuminance = ExposureBinToLogLuminance(lowBin);
		}

		lowLogLuminance = ExposureBinToLogLuminance(lowBin);
		highLogLuminance = ExposureBinToLogLuminance(highBin);
		const float meteredLuminance = max(exp2(meteredLogLuminance), 1.0e-6);
		targetExposure = gExposureConstants.TargetLuminance * gExposureConstants.ExposureBias / meteredLuminance;
		targetExposure = clamp(targetExposure, gExposureConstants.MinExposure, gExposureConstants.MaxExposure);
	}

	const float previousExposure = ResolvePreviousExposure(targetExposure);
	float adaptedExposure = targetExposure;
	if ((gExposureConstants.Flags & NRI_EXPOSURE_FLAG_FREEZE) != 0u)
	{
		adaptedExposure = previousExposure;
	}
	else
	{
		const float previousStops = log2(max(previousExposure, 1.0e-6));
		const float targetStops = log2(max(targetExposure, 1.0e-6));
		const float speed = targetStops > previousStops ? gExposureConstants.AdaptUpSpeed : gExposureConstants.AdaptDownSpeed;
		const float alpha = 1.0 - exp(-max(speed, 0.0) * (1.0 / 60.0));
		adaptedExposure = exp2(lerp(previousStops, targetStops, saturate(alpha)));
	}
	adaptedExposure = clamp(SanitizeExposureFloat(adaptedExposure, targetExposure), gExposureConstants.MinExposure, gExposureConstants.MaxExposure);

	gCurrentExposureState[uint2(0, 0)] = float4(adaptedExposure, targetExposure, meteredLogLuminance, (float)totalSamples);

	gExposureDebug[0] = NRI_EXPOSURE_DEBUG_MAGIC;
	gExposureDebug[1] = gExposureConstants.FrameIndex;
	gExposureDebug[2] = totalSamples;
	gExposureDebug[3] = (uint)floor((float)totalSamples * saturate(gExposureConstants.LowPercentile * 0.01));
	gExposureDebug[4] = (uint)floor((float)totalSamples * saturate(gExposureConstants.HighPercentile * 0.01));
	gExposureDebug[5] = lowBin;
	gExposureDebug[6] = highBin;
	gExposureDebug[7] = asuint(lowLogLuminance);
	gExposureDebug[8] = asuint(highLogLuminance);
	gExposureDebug[9] = asuint(meteredLogLuminance);
	gExposureDebug[10] = asuint(targetExposure);
	gExposureDebug[11] = asuint(adaptedExposure);
	gExposureDebug[12] = asuint(previousExposure);
	gExposureDebug[13] = asuint(fallbackExposure);
	gExposureDebug[14] = gExposureConstants.Flags;
	gExposureDebug[15] = binCount;
}
