#ifndef NRI_SMOKE_DORMANT_SUMMARY_HLSLI
#define NRI_SMOKE_DORMANT_SUMMARY_HLSLI

// Experimental only. The production grid deliberately has no archive/release
// pass until fine-brick provenance and transactional publication are available.
struct SmokeDormantSummary
{
	float3 BoundsMin;
	float OpticalMass;
	float3 BoundsMax;
	float ThermalMass;
	float3 Centroid;
	float DensityMass;
	float3 Velocity;
	float VelocityWeight;
	uint SourceId;
	uint SourceClass;
	uint LastSimulationTick;
	uint Epoch;
	uint BrickGeneration;
	uint SummaryGeneration;
	uint Flags;
	uint Padding;
};

float SmokeDormantDecayFactor(uint elapsedTicks, float secondsPerTick, float halfLifeSeconds)
{
	if (elapsedTicks == 0u)
		return 1.0f;
	if (!(secondsPerTick > 0.0f) || !(halfLifeSeconds > 0.0f))
		return 0.0f;
	return exp2(-((float)elapsedTicks * secondsPerTick) / halfLifeSeconds);
}

#endif
