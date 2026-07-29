#pragma once

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#endif

#include <cstdint>
#include <cstring>

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;

enum NriFfxApiReturnCode : uint32_t
{
	NRI_FFX_API_RETURN_OK = 0,
	NRI_FFX_API_RETURN_ERROR = 1,
	NRI_FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
	NRI_FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
	NRI_FFX_API_RETURN_NO_PROVIDER = 4,
	NRI_FFX_API_RETURN_ERROR_MEMORY = 5,
	NRI_FFX_API_RETURN_ERROR_PARAMETER = 6,
};

typedef uint64_t ffxStructType_t;
typedef struct ffxApiHeader
{
	ffxStructType_t type;
	struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxQueryDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks
{
	void* pUserData;
	ffxAlloc alloc;
	ffxDealloc dealloc;
} ffxAllocationCallbacks;

typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* context, const ffxConfigureDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext* context, ffxQueryDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext* context, const ffxDispatchDescHeader* desc);

enum
{
	NRI_FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 = 0x0000001u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x0000002u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK = 0x0000003u,
	NRI_FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION = 6u,
	NRI_FFX_API_EFFECT_ID_FRAMEGENERATION = 0x00020000u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION = 0x00020001u,
	NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION = 0x00020002u,
	NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION = 0x00020003u,
	NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE = 0x00020004u,
	NRI_FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT = 0x00020005u,
	NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE = 0x00020006u,
	NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE = 0x00020007u,
	NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE = 0x00020008u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS = 0x00020009u,
	NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO = 0x0002000au,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 = 0x00030001u,
	NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 = 0x00030002u,
	NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12 = 0x00030003u,
	NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12 = 0x00030004u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 = 0x00030005u,
	NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12 = 0x00030006u,
	NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 = 0x00030007u,
	NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 = 0x00030008u,
	NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12 = 0x00030009u,
};

enum
{
	NRI_FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE = 0x0000000u,
	NRI_FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS = 0x0000001u,
	NRI_FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS = 0x0000002u,
	NRI_FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE = 0x0fffffffu,
};

enum
{
	NRI_FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT = (1u << 0),
	NRI_FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1u << 1),
	NRI_FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1u << 2),
	NRI_FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED = (1u << 3),
	NRI_FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE = (1u << 4),
	NRI_FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE = (1u << 5),
	NRI_FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING = (1u << 6),
};

enum
{
	NRI_FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES = (1u << 0),
	NRI_FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS = (1u << 1),
	NRI_FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW = (1u << 2),
	NRI_FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY = (1u << 3),
	NRI_FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES = (1u << 4),
};

enum
{
	NRI_FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA = (1u << 0),
	NRI_FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING = (1u << 1),
};

enum
{
	NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB = 0,
	NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ = 1,
	NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB = 2,
};

enum NriFfxApiSurfaceFormat : uint32_t
{
	NRI_FFX_API_SURFACE_FORMAT_UNKNOWN,
	NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R32G32_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R8_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R32_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM,
	NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB,
	NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB,
	NRI_FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R16G16_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R16G16_SINT,
	NRI_FFX_API_SURFACE_FORMAT_R16_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R16_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R16_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_R16_SNORM,
	NRI_FFX_API_SURFACE_FORMAT_R8_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_R8G8_UNORM,
	NRI_FFX_API_SURFACE_FORMAT_R8G8_UINT,
	NRI_FFX_API_SURFACE_FORMAT_R32_FLOAT,
	NRI_FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP,
	NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R32G32_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R16G16_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R16_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R8_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R8G8_TYPELESS,
	NRI_FFX_API_SURFACE_FORMAT_R32_TYPELESS,
};

enum
{
	NRI_FFX_API_RESOURCE_USAGE_READ_ONLY = 0,
	NRI_FFX_API_RESOURCE_USAGE_RENDERTARGET = (1u << 0),
	NRI_FFX_API_RESOURCE_USAGE_UAV = (1u << 1),
	NRI_FFX_API_RESOURCE_USAGE_DEPTHTARGET = (1u << 2),
	NRI_FFX_API_RESOURCE_USAGE_INDIRECT = (1u << 3),
	NRI_FFX_API_RESOURCE_USAGE_ARRAYVIEW = (1u << 4),
	NRI_FFX_API_RESOURCE_USAGE_STENCILTARGET = (1u << 5),
};

enum
{
	NRI_FFX_API_RESOURCE_STATE_COMMON = (1u << 0),
	NRI_FFX_API_RESOURCE_STATE_UNORDERED_ACCESS = (1u << 1),
	NRI_FFX_API_RESOURCE_STATE_COMPUTE_READ = (1u << 2),
	NRI_FFX_API_RESOURCE_STATE_PIXEL_READ = (1u << 3),
	NRI_FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ = (NRI_FFX_API_RESOURCE_STATE_PIXEL_READ | NRI_FFX_API_RESOURCE_STATE_COMPUTE_READ),
	NRI_FFX_API_RESOURCE_STATE_COPY_SRC = (1u << 4),
	NRI_FFX_API_RESOURCE_STATE_COPY_DEST = (1u << 5),
	NRI_FFX_API_RESOURCE_STATE_GENERIC_READ = (NRI_FFX_API_RESOURCE_STATE_COPY_SRC | NRI_FFX_API_RESOURCE_STATE_COMPUTE_READ),
	NRI_FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT = (1u << 6),
	NRI_FFX_API_RESOURCE_STATE_PRESENT = (1u << 7),
	NRI_FFX_API_RESOURCE_STATE_RENDER_TARGET = (1u << 8),
};

enum
{
	NRI_FFX_API_RESOURCE_FLAGS_NONE = 0,
	NRI_FFX_API_RESOURCE_FLAGS_ALIASABLE = (1u << 0),
	NRI_FFX_API_RESOURCE_FLAGS_UNDEFINED = (1u << 1),
};

enum
{
	NRI_FFX_API_RESOURCE_TYPE_BUFFER = 0,
	NRI_FFX_API_RESOURCE_TYPE_TEXTURE1D = 1,
	NRI_FFX_API_RESOURCE_TYPE_TEXTURE2D = 2,
	NRI_FFX_API_RESOURCE_TYPE_TEXTURE_CUBE = 3,
	NRI_FFX_API_RESOURCE_TYPE_TEXTURE3D = 4,
};

struct ffxConfigureDescGlobalDebug1
{
	ffxConfigureDescHeader header;
	ffxApiMessage fpMessage;
	uint32_t debugLevel;
};

#ifdef _WIN32
struct ffxCreateBackendDX12Desc
{
	ffxCreateContextDescHeader header;
	ID3D12Device* device;
};
#endif  // _WIN32

// Vulkan backend. VkDevice/VkPhysicalDevice are dispatchable handles (opaque
// pointers) and vkGetDeviceProcAddr is a plain function pointer, so this stays
// free of any Vulkan headers - matching how the DX12 side is declared here.
struct ffxCreateBackendVKDesc
{
	ffxCreateContextDescHeader header;
	void* vkDevice;
	void* vkPhysicalDevice;
	void* vkDeviceProcAddr;
};

struct FfxApiDimensions2D
{
	uint32_t width;
	uint32_t height;
};

struct FfxApiFloatCoords2D
{
	float x;
	float y;
};

struct FfxApiRect2D
{
	int32_t left;
	int32_t top;
	int32_t width;
	int32_t height;
};

struct FfxApiResourceDescription
{
	uint32_t type;
	uint32_t format;
	union { uint32_t width; uint32_t size; };
	union { uint32_t height; uint32_t stride; };
	union { uint32_t depth; uint32_t alignment; };
	uint32_t mipCount;
	uint32_t flags;
	uint32_t usage;
};

struct FfxApiResource
{
	void* resource;
	FfxApiResourceDescription description;
	uint32_t state;
};

struct FfxApiEffectMemoryUsage
{
	uint64_t totalUsageInBytes;
	uint64_t aliasableUsageInBytes;
};

struct ffxQueryGetProviderVersion
{
	ffxQueryDescHeader header;
	uint64_t versionId;
	const char* versionName;
};

struct ffxQueryDescFrameGenerationGetGPUMemoryUsage
{
	ffxQueryDescHeader header;
	FfxApiEffectMemoryUsage* gpuMemoryUsageFrameGeneration;
};

struct ffxCreateContextDescFrameGeneration
{
	ffxCreateContextDescHeader header;
	uint32_t flags;
	FfxApiDimensions2D displaySize;
	FfxApiDimensions2D maxRenderSize;
	uint32_t backBufferFormat;
};

struct ffxCreateContextDescFrameGenerationHudless
{
	ffxCreateContextDescHeader header;
	uint32_t hudlessBackBufferFormat;
};

#ifdef _WIN32
struct ffxCreateContextDescFrameGenerationSwapChainWrapDX12
{
	ffxCreateContextDescHeader header;
	IDXGISwapChain4** swapchain;
	ID3D12CommandQueue* gameQueue;
};

struct ffxCreateContextDescFrameGenerationSwapChainNewDX12
{
	ffxCreateContextDescHeader header;
	IDXGISwapChain4** swapchain;
	DXGI_SWAP_CHAIN_DESC* desc;
	IDXGIFactory* dxgiFactory;
	ID3D12CommandQueue* gameQueue;
};

struct ffxCreateContextDescFrameGenerationSwapChainForHwndDX12
{
	ffxCreateContextDescHeader header;
	IDXGISwapChain4** swapchain;
	HWND hwnd;
	DXGI_SWAP_CHAIN_DESC1* desc;
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc;
	IDXGIFactory* dxgiFactory;
	ID3D12CommandQueue* gameQueue;
};
#endif  // _WIN32

struct ffxCallbackDescFrameGenerationPresent
{
	ffxDispatchDescHeader header;
	void* device;
	void* commandList;
	FfxApiResource currentBackBuffer;
	FfxApiResource currentUI;
	FfxApiResource outputSwapChainBuffer;
	bool isGeneratedFrame;
	uint64_t frameID;
};

struct ffxDispatchDescFrameGeneration
{
	ffxDispatchDescHeader header;
	void* commandList;
	FfxApiResource presentColor;
	FfxApiResource outputs[4];
	uint32_t numGeneratedFrames;
	bool reset;
	uint32_t backbufferTransferFunction;
	float minMaxLuminance[2];
	FfxApiRect2D generationRect;
	uint64_t frameID;
};

typedef ffxReturnCode_t(*PfnFfxPresentCallback)(ffxCallbackDescFrameGenerationPresent* params, void* userContext);
typedef ffxReturnCode_t(*PfnFfxFrameGenerationDispatchCallback)(ffxDispatchDescFrameGeneration* params, void* userContext);

struct ffxConfigureDescFrameGeneration
{
	ffxConfigureDescHeader header;
	void* swapChain;
	PfnFfxPresentCallback presentCallback;
	void* presentCallbackUserContext;
	PfnFfxFrameGenerationDispatchCallback frameGenerationCallback;
	void* frameGenerationCallbackUserContext;
	bool frameGenerationEnabled;
	bool allowAsyncWorkloads;
	FfxApiResource HUDLessColor;
	uint32_t flags;
	bool onlyPresentGenerated;
	FfxApiRect2D generationRect;
	uint64_t frameID;
};

struct ffxDispatchDescFrameGenerationPrepare
{
	ffxDispatchDescHeader header;
	uint64_t frameID;
	uint32_t flags;
	void* commandList;
	FfxApiDimensions2D renderSize;
	FfxApiFloatCoords2D jitterOffset;
	FfxApiFloatCoords2D motionVectorScale;
	float frameTimeDelta;
	bool unused_reset;
	float cameraNear;
	float cameraFar;
	float cameraFovAngleVertical;
	float viewSpaceToMetersFactor;
	FfxApiResource depth;
	FfxApiResource motionVectors;
};

struct ffxDispatchDescFrameGenerationPrepareCameraInfo
{
	ffxConfigureDescHeader header;
	float cameraPosition[3];
	float cameraUp[3];
	float cameraRight[3];
	float cameraForward[3];
};

struct ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12
{
	ffxConfigureDescHeader header;
	FfxApiResource uiResource;
	uint32_t flags;
};

struct ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12
{
	ffxQueryDescHeader header;
	void** pOutCommandList;
};

struct ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12
{
	ffxQueryDescHeader header;
	FfxApiResource* pOutTexture;
};

struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12
{
	ffxDispatchDescHeader header;
};

struct ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12
{
	ffxQueryDescHeader header;
	FfxApiEffectMemoryUsage* gpuMemoryUsageFrameGenerationSwapchain;
};

static inline void NriFfxInitHeader(ffxApiHeader& header, uint64_t type)
{
	header.type = type;
	header.pNext = nullptr;
}

#ifdef _WIN32
static inline uint32_t NriFfxGetSurfaceFormatDX12(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS;
	case DXGI_FORMAT_R32G32B32A32_UINT: return NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R32G32_TYPELESS;
	case DXGI_FORMAT_R32G32_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_R32_UINT:
		return NRI_FFX_API_SURFACE_FORMAT_R32_UINT;
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_R8_UINT:
		return NRI_FFX_API_SURFACE_FORMAT_R8_UINT;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_UNORM: return NRI_FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R11G11B10_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_R8G8B8A8_UNORM: return NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
	case DXGI_FORMAT_R8G8B8A8_SNORM: return NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
	case DXGI_FORMAT_R16G16_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R16G16_TYPELESS;
	case DXGI_FORMAT_R16G16_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
	case DXGI_FORMAT_R16G16_UINT: return NRI_FFX_API_SURFACE_FORMAT_R16G16_UINT;
	case DXGI_FORMAT_R32_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
		return NRI_FFX_API_SURFACE_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R8G8_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R8G8_TYPELESS;
	case DXGI_FORMAT_R8G8_UNORM: return NRI_FFX_API_SURFACE_FORMAT_R8G8_UNORM;
	case DXGI_FORMAT_R8G8_UINT: return NRI_FFX_API_SURFACE_FORMAT_R8G8_UINT;
	case DXGI_FORMAT_R16_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R16_TYPELESS;
	case DXGI_FORMAT_R16_FLOAT: return NRI_FFX_API_SURFACE_FORMAT_R16_FLOAT;
	case DXGI_FORMAT_R16_UINT: return NRI_FFX_API_SURFACE_FORMAT_R16_UINT;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
		return NRI_FFX_API_SURFACE_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R16_SNORM: return NRI_FFX_API_SURFACE_FORMAT_R16_SNORM;
	case DXGI_FORMAT_R8_TYPELESS: return NRI_FFX_API_SURFACE_FORMAT_R8_TYPELESS;
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_A8_UNORM:
		return NRI_FFX_API_SURFACE_FORMAT_R8_UNORM;
	case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return NRI_FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
	default: return NRI_FFX_API_SURFACE_FORMAT_UNKNOWN;
	}
}
#endif  // _WIN32

#ifdef _WIN32
static inline uint32_t NriFfxGetResourceStateFromDx12State(D3D12_RESOURCE_STATES state)
{
	if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
		return NRI_FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
	if ((state & D3D12_RESOURCE_STATE_COPY_DEST) != 0)
		return NRI_FFX_API_RESOURCE_STATE_COPY_DEST;
	if ((state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0)
		return NRI_FFX_API_RESOURCE_STATE_COPY_SRC;
	if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0)
		return NRI_FFX_API_RESOURCE_STATE_RENDER_TARGET;
	if ((state & D3D12_RESOURCE_STATE_PRESENT) != 0)
		return NRI_FFX_API_RESOURCE_STATE_PRESENT;
	if ((state & D3D12_RESOURCE_STATE_INDEX_BUFFER) != 0 || (state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) != 0)
		return NRI_FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT;
	if ((state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0 && (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0)
		return NRI_FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
	if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0)
		return NRI_FFX_API_RESOURCE_STATE_COMPUTE_READ;
	if ((state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0)
		return NRI_FFX_API_RESOURCE_STATE_PIXEL_READ;
	return NRI_FFX_API_RESOURCE_STATE_COMMON;
}

static inline FfxApiResource NriFfxGetResourceDX12(ID3D12Resource* resource, uint32_t state, uint32_t additionalUsages = 0u)
{
	FfxApiResource result = {};
	result.resource = resource;
	result.state = state;
	if (resource == nullptr)
		return result;

	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		result.description.flags = NRI_FFX_API_RESOURCE_FLAGS_NONE;
		result.description.usage = NRI_FFX_API_RESOURCE_USAGE_UAV;
		result.description.size = (uint32_t)desc.Width;
		result.description.stride = (uint32_t)desc.Height;
		result.description.type = NRI_FFX_API_RESOURCE_TYPE_BUFFER;
	}
	else
	{
		result.description.flags = NRI_FFX_API_RESOURCE_FLAGS_NONE;
		if (desc.Format == DXGI_FORMAT_D16_UNORM || desc.Format == DXGI_FORMAT_D32_FLOAT)
			result.description.usage = NRI_FFX_API_RESOURCE_USAGE_DEPTHTARGET;
		else if (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
			result.description.usage = NRI_FFX_API_RESOURCE_USAGE_DEPTHTARGET | NRI_FFX_API_RESOURCE_USAGE_STENCILTARGET;
		else
			result.description.usage = NRI_FFX_API_RESOURCE_USAGE_READ_ONLY;

		if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0)
			result.description.usage |= NRI_FFX_API_RESOURCE_USAGE_UAV;

		result.description.width = (uint32_t)desc.Width;
		result.description.height = (uint32_t)desc.Height;
		result.description.depth = (uint32_t)desc.DepthOrArraySize;
		result.description.mipCount = (uint32_t)desc.MipLevels;

		switch (desc.Dimension)
		{
		case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
			result.description.type = NRI_FFX_API_RESOURCE_TYPE_TEXTURE1D;
			break;
		case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
			result.description.type = desc.DepthOrArraySize == 6 ? NRI_FFX_API_RESOURCE_TYPE_TEXTURE_CUBE : NRI_FFX_API_RESOURCE_TYPE_TEXTURE2D;
			break;
		case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
			result.description.type = NRI_FFX_API_RESOURCE_TYPE_TEXTURE3D;
			break;
		default:
			result.description.type = NRI_FFX_API_RESOURCE_TYPE_TEXTURE2D;
			break;
		}
	}

	result.description.format = NriFfxGetSurfaceFormatDX12(desc.Format);
	result.description.usage |= additionalUsages;
	return result;
}
#endif  // _WIN32
