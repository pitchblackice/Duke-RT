#ifndef RAZE_NRI_PT_PRESENT_CONSTANTS_HLSLI
#define RAZE_NRI_PT_PRESENT_CONSTANTS_HLSLI

#define NRI_PRESENT_FLAG_ADD_SECONDARY 0x10u
#define NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE 0x1u
#define NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED 0x2u
#define NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE 0x4u
#define NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET 0x8u

struct NRIPresentConstants
{
	uint InputWidth;
	uint InputHeight;
	uint DisplayWidth;
	uint DisplayHeight;
	uint PackedSceneOrigin;
	uint FrameIndex;
	uint DebugMode;
	uint Flags;
	uint DenoiserMode;
	uint OutputMode;
	uint TonemapMode;
	uint OutputFlags;
	float Exposure;
	float PaperWhiteNits;
	float DisplayMaxLuminance;
	float DisplaySdrLuminance;
};

#endif
