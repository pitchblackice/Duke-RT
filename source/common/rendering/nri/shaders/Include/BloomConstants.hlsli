#ifndef RAZE_NRI_PT_BLOOM_CONSTANTS_HLSLI
#define RAZE_NRI_PT_BLOOM_CONSTANTS_HLSLI

#define NRI_BLOOM_FLAG_THRESHOLD 0x1u
#define NRI_BLOOM_FLAG_ENERGY_CONSTRAINED 0x2u
#define NRI_BLOOM_FLAG_DEBUG 0x4u

struct NRIBloomConstants
{
	uint InputWidth;
	uint InputHeight;
	uint OutputWidth;
	uint OutputHeight;
	float Intensity;
	float Sigma;
	float Cutoff;
	float Fuzziness;
	uint FrameIndex;
	uint LevelIndex;
	uint LevelCount;
	uint Flags;
	float InputTexelSizeX;
	float InputTexelSizeY;
	float Reserved0;
	float Reserved1;
};

#endif
