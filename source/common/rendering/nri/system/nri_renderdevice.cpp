#include "nri_renderdevice.h"

#include "../framegen/nri_framegen.h"
#include "../renderer/nri_renderer.h"
#include "../renderer/nri_renderstate.h"
#include "nri_hwbuffer.h"
#include "nri_hwtexture.h"
#include "c_cvars.h"
#include "cmdlib.h"
#include "d_eventbase.h"
#include "i_mainwindow.h"
#include "i_time.h"
#include "printf.h"
#include "textures.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "version.h"
#include "hw_drawinfo.h"
#include "hwrenderer/data/hw_clock.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_skydome.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "coreplayer.h"
#include "coreactor.h"
#include "gamecontrol.h"
#include "lightoverlay.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <fstream>
#include <string>

EXTERN_CVAR(String, nri_api)
EXTERN_CVAR(Int, nri_ptportaldepth)
EXTERN_CVAR(Int, nri_ptdebug)
EXTERN_CVAR(Bool, nri_validation)
EXTERN_CVAR(Bool, nri_apivalidation)
EXTERN_CVAR(Bool, nri_dred)
EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Bool, nri_framegen)
EXTERN_CVAR(Bool, nri_framegenlatency)
EXTERN_CVAR(Int, nri_framegenprovider)
EXTERN_CVAR(Bool, nri_ptsectorlighting)
EXTERN_CVAR(Float, nri_ptsectorambientscale)
EXTERN_CVAR(Float, nri_ptsectorhemiscale)
EXTERN_CVAR(Float, nri_ptsectorfogscale)
EXTERN_CVAR(Float, nri_ptsectorclamp)
EXTERN_CVAR(Int, nri_ptsectorfilterpal)
EXTERN_CVAR(Int, nri_ptsectorfilterminshade)
EXTERN_CVAR(Int, nri_ptsectorfiltermaxshade)
EXTERN_CVAR(Int, nri_ptsectorfilterlotag)
EXTERN_CVAR(Int, nri_ptsectorpulseframes)
EXTERN_CVAR(Float, nri_ptsectorpulseamount)
CVAR(Bool, nri_ptsanity, false, 0)
CVAR(Bool, nri_ptwaitpresent, true, 0)

CUSTOM_CVAR(Int, nri_pttraceframes, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 600)
	{
		self = 600;
	}
}

CUSTOM_CVAR(Int, nri_ptnudgetrace, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}
}

namespace
{
	static bool IsFullscreenPaletteBlendCommand(const F2DDrawer& drawer, const F2DDrawer::RenderCommand& cmd)
	{
		if (cmd.isSpecial != SpecialDrawCommand::NotSpecial ||
			cmd.mTexture != nullptr ||
			cmd.shape2DBufInfo != nullptr ||
			cmd.mType != F2DDrawer::DrawTypeTriangles ||
			(cmd.mFlags & F2DDrawer::DTF_Scissor) != 0 ||
			cmd.mVertCount != 4 ||
			cmd.mIndexCount != 6 ||
			cmd.mVertIndex < 0 ||
			cmd.mVertIndex + 3 >= drawer.mVertices.SSize())
		{
			return false;
		}

		const auto& v0 = drawer.mVertices[cmd.mVertIndex + 0];
		const auto& v1 = drawer.mVertices[cmd.mVertIndex + 1];
		const auto& v2 = drawer.mVertices[cmd.mVertIndex + 2];
		const auto& v3 = drawer.mVertices[cmd.mVertIndex + 3];
		const float width = (float)drawer.GetWidth();
		const float height = (float)drawer.GetHeight();

		return
			v0.x == 0.0f && v0.y == 0.0f &&
			v1.x == 0.0f && v1.y == height &&
			v2.x == width && v2.y == 0.0f &&
			v3.x == width && v3.y == height;
	}

	static constexpr int DefaultSwapChainTextureCount = 3;
	static constexpr float DefaultPtTestLightRadius = 256.0f;
	static constexpr float DefaultPtTestLightOffset = 64.0f;
	static constexpr float DefaultPtTestSphereMetalness = 1.0f;
	static constexpr float DefaultPtTestSphereRoughness = 0.05f;
	static constexpr double BuildTickSeconds = 1.0 / 120.0;
	static NRIRenderDevice* GetActiveNRIRenderDevice();

	static void WorldToPathTracingPosition(const DVector3& worldPos, float out[3])
	{
		out[0] = (float)worldPos.X;
		out[1] = (float)-worldPos.Z;
		out[2] = (float)-worldPos.Y;
	}

	static FString DescribeResolvedMuzzleFlashRuleIds(const ResolvedLightOverlaySet& resolvedSet, uint32_t limit = 16u)
	{
		if (resolvedSet.muzzleFlashRules.Size() == 0)
		{
			return "none";
		}

		TArray<FString> ids;
		ids.Reserve(resolvedSet.muzzleFlashRules.Size());
		for (const auto& rule : resolvedSet.muzzleFlashRules)
		{
			ids.Push(rule.id);
		}

		std::sort(ids.begin(), ids.end(), [](const FString& a, const FString& b)
		{
			return a.CompareNoCase(b) < 0;
		});

		FString result;
		const uint32_t printCount = std::min<uint32_t>((uint32_t)ids.Size(), limit);
		for (uint32_t i = 0; i < printCount; ++i)
		{
			if (!result.IsEmpty())
			{
				result << ",";
			}
			result << ids[i];
		}

		if (printCount < (uint32_t)ids.Size())
		{
			result.AppendFormat(",...(+%u)", (uint32_t)ids.Size() - printCount);
		}

		return result;
	}

	static const ResolvedLightOverlayMuzzleFlashRule* FindResolvedMuzzleFlashRule(const ResolvedLightOverlaySet& resolvedSet, const char* eventId)
	{
		if (eventId == nullptr || *eventId == '\0')
		{
			return nullptr;
		}

		for (const auto& rule : resolvedSet.muzzleFlashRules)
		{
			if (rule.id.CompareNoCase(eventId) == 0)
			{
				return &rule;
			}
		}

		return nullptr;
	}

	static bool BuildLocalPlayerWeaponLightEvent(const char* eventId, float forwardOffset, PathTracingWeaponLightEvent& outEvent, FString& outError)
	{
		if (eventId == nullptr || *eventId == '\0')
		{
			outError = "missing muzzle-flash event id";
			return false;
		}

		if (netgame)
		{
			outError = "cannot be used in multiplayer";
			return false;
		}

		outEvent = {};
		outEvent.eventId = eventId;
		outEvent.absoluteTimeSeconds = PlayClock > 0 ? (double)PlayClock * BuildTickSeconds : 0.0;

		DCorePlayer* player = PlayerArray[myconnectindex];
		if (player == nullptr)
		{
			outEvent.worldPosition = DVector3(forwardOffset, 0.0, 0.0);
			outEvent.basisRight = DVector3(0.0, 1.0, 0.0);
			outEvent.basisForward = DVector3(1.0, 0.0, 0.0);
			outEvent.basisUp = DVector3(0.0, 0.0, 1.0);
			outEvent.hasBasis = true;
			return true;
		}

		DCoreActor* actor = player->GetActor();
		if (actor == nullptr)
		{
			outEvent.worldPosition = DVector3(forwardOffset, 0.0, 0.0);
			outEvent.basisRight = DVector3(0.0, 1.0, 0.0);
			outEvent.basisForward = DVector3(1.0, 0.0, 0.0);
			outEvent.basisUp = DVector3(0.0, 0.0, 1.0);
			outEvent.hasBasis = true;
			return true;
		}

		const DRotator viewRotation(
			player->getPitchWithView(),
			actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
			actor->spr.Angles.Roll + player->ViewAngles.Roll);
		const DVector3 forward = DVector3(viewRotation).Unit();
		const DVector3 right = DVector3(DRotator(nullAngle, viewRotation.Yaw + DAngle90, nullAngle)).Unit();
		DVector3 up = (forward ^ right).Unit();
		if (up.isZero())
		{
			up = DVector3(0.0, 0.0, 1.0);
		}

		outEvent.hasEmitterActorIndex = true;
		outEvent.emitterActorIndex = actor->GetIndex();
		outEvent.worldPosition = actor->getPosWithOffsetZ() + forward * forwardOffset;
		outEvent.basisRight = right;
		outEvent.basisForward = forward;
		outEvent.basisUp = up;
		outEvent.hasBasis = true;
		return true;
	}
}

CUSTOM_CVAR(Int, nri_ptswaptextures, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self == 1)
	{
		self = 2;
	}
	else if (self > 8)
	{
		self = 8;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}

CUSTOM_CVAR(Int, nri_ptswapflags, -1, 0)
{
	if (self < -1)
	{
		self = -1;
	}
	else if (self > 3)
	{
		self = 3;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->SetVSync(vid_vsync);
	}
}

namespace
{
	static nri::Result(NRI_CALL* gNriGetInterfaceForwarder)(const nri::Device&, const char*, size_t, void*) = nullptr;
	static void (NRI_CALL* gNriDestroyDeviceForwarder)(nri::Device*) = nullptr;
	using PFN_D3D12_GET_DEBUG_INTERFACE = HRESULT(WINAPI*)(REFIID, void**);

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIShaderStages()
	{
		return (nri::StageBits)((uint32_t)nri::StageBits::VERTEX_SHADER | (uint32_t)nri::StageBits::FRAGMENT_SHADER);
	}

	static nri::StageBits NRISwapChainAcquireWaitStages()
	{
		// Raze first touches the acquired swapchain image as a copy destination in
		// PostProcessScene() before HUD color-attachment work begins.
		return (nri::StageBits)((uint32_t)nri::StageBits::COPY | (uint32_t)nri::StageBits::COLOR_ATTACHMENT);
	}

	static uint32_t AlignUp(uint32_t value, uint32_t alignment)
	{
		if (alignment <= 1)
		{
			return value;
		}

		const uint32_t remainder = value % alignment;
		return remainder == 0 ? value : value + alignment - remainder;
	}

	static const char* GetDxgiErrorName(HRESULT hr)
	{
		switch (hr)
		{
		case S_OK: return "S_OK";
		case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
		case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
		case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
		case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
		default: return "unknown";
		}
	}

	static PFN_D3D12_GET_DEBUG_INTERFACE GetD3D12GetDebugInterfaceFn()
	{
		static PFN_D3D12_GET_DEBUG_INTERFACE sFn = nullptr;
		static bool sLoaded = false;
		if (!sLoaded)
		{
			HMODULE module = GetModuleHandleW(L"d3d12.dll");
			if (module == nullptr)
			{
				module = LoadLibraryW(L"d3d12.dll");
			}

			if (module != nullptr)
			{
				sFn = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(module, "D3D12GetDebugInterface"));
			}

			sLoaded = true;
		}

		return sFn;
	}

	static std::string NarrowWideString(const wchar_t* text)
	{
		if (text == nullptr || *text == L'\0')
		{
			return {};
		}

		const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (required <= 1)
		{
			return {};
		}

		std::string result((size_t)required, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
		result.pop_back();
		return result;
	}

	static std::string GetDredDebugName(const char* ansiName, const wchar_t* wideName)
	{
		if (ansiName != nullptr && *ansiName != '\0')
		{
			return ansiName;
		}

		std::string wide = NarrowWideString(wideName);
		return wide.empty() ? std::string("(unnamed)") : wide;
	}

	static const char* GetD3D12AutoBreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
	{
		switch (op)
		{
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRayTracingAS";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRayTracingAS";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
		case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
		case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
		case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "BeginCommandList";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHGRAPH: return "DispatchGraph";
		case D3D12_AUTO_BREADCRUMB_OP_SETPROGRAM: return "SetProgram";
		default: return "Other";
		}
	}

	static const char* GetD3D12DredAllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
	{
		switch (type)
		{
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "CommandQueue";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PipelineState";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "CommandList";
		case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "Fence";
		case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DescriptorHeap";
		case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "Heap";
		case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QueryHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PipelineLibrary";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER: return "VideoDecoder";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "VideoProcessor";
		case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "Resource";
		case D3D12_DRED_ALLOCATION_TYPE_PASS: return "Pass";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION: return "CryptoSession";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "CryptoSessionPolicy";
		case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "ProtectedResourceSession";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "VideoDecoderHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "CommandPool";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "CommandRecorder";
		case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "StateObject";
		case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "MetaCommand";
		case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "SchedulingGroup";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_ESTIMATOR: return "VideoMotionEstimator";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_VECTOR_HEAP: return "VideoMotionVectorHeap";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_EXTENSION_COMMAND: return "VideoExtensionCommand";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_ENCODER: return "VideoEncoder";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_ENCODER_HEAP: return "VideoEncoderHeap";
		default: return "Other";
		}
	}

	static void LogD3D12DredBreadcrumbWindow(const D3D12_AUTO_BREADCRUMB_OP* history, uint32_t breadcrumbCount, uint32_t completedValue, uint32_t nodeIndex)
	{
		if (history == nullptr || breadcrumbCount == 0)
		{
			return;
		}

		const uint32_t clampedCompleted = (std::min)(completedValue, breadcrumbCount - 1);
		const uint32_t start = clampedCompleted > 2 ? clampedCompleted - 2 : 0;
		const uint32_t end = (std::min)(breadcrumbCount, start + 5);
		for (uint32_t i = start; i < end; ++i)
		{
			const char* marker = i == clampedCompleted ? " <last_completed>" : "";
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u].op[%u]: %s%s\n",
				nodeIndex,
				i,
				GetD3D12AutoBreadcrumbOpName(history[i]),
				marker);
		}
	}

	static void LogD3D12DredBreadcrumbNodes(const D3D12_AUTO_BREADCRUMB_NODE* head, const char* context)
	{
		uint32_t nodeIndex = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE* node = head; node != nullptr && nodeIndex < 6; node = node->pNext, ++nodeIndex)
		{
			const std::string commandListName = GetDredDebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW);
			const std::string queueName = GetDredDebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
			const uint32_t breadcrumbCount = node->BreadcrumbCount;
			const uint32_t completedValue = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u] after %s: cmdlist=%s queue=%s completed=%u/%u\n",
				nodeIndex,
				context != nullptr ? context : "unknown",
				commandListName.c_str(),
				queueName.c_str(),
				completedValue,
				breadcrumbCount);

			LogD3D12DredBreadcrumbWindow(node->pCommandHistory, breadcrumbCount, completedValue, nodeIndex);
		}
	}

	static void LogD3D12DredBreadcrumbNodes1(const D3D12_AUTO_BREADCRUMB_NODE1* head, const char* context)
	{
		uint32_t nodeIndex = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* node = head; node != nullptr && nodeIndex < 6; node = node->pNext, ++nodeIndex)
		{
			const std::string commandListName = GetDredDebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW);
			const std::string queueName = GetDredDebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
			const uint32_t breadcrumbCount = node->BreadcrumbCount;
			const uint32_t completedValue = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u] after %s: cmdlist=%s queue=%s completed=%u/%u contexts=%u\n",
				nodeIndex,
				context != nullptr ? context : "unknown",
				commandListName.c_str(),
				queueName.c_str(),
				completedValue,
				breadcrumbCount,
				node->BreadcrumbContextsCount);

			LogD3D12DredBreadcrumbWindow(node->pCommandHistory, breadcrumbCount, completedValue, nodeIndex);

			if (node->pBreadcrumbContexts != nullptr && node->BreadcrumbContextsCount != 0)
			{
				const uint32_t contextCount = (std::min)(node->BreadcrumbContextsCount, 6u);
				for (uint32_t i = 0; i < contextCount; ++i)
				{
					const D3D12_DRED_BREADCRUMB_CONTEXT& breadcrumbContext = node->pBreadcrumbContexts[i];
					const std::string breadcrumbText = NarrowWideString(breadcrumbContext.pContextString);
					Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u].context[%u]: index=%u text=%s\n",
						nodeIndex,
						i,
						breadcrumbContext.BreadcrumbIndex,
						breadcrumbText.empty() ? "(empty)" : breadcrumbText.c_str());
				}
			}
		}
	}

	static void LogD3D12DredAllocationNodes(const D3D12_DRED_ALLOCATION_NODE* head, const char* label)
	{
		uint32_t allocationIndex = 0;
		for (const D3D12_DRED_ALLOCATION_NODE* allocation = head; allocation != nullptr && allocationIndex < 8; allocation = allocation->pNext, ++allocationIndex)
		{
			const std::string objectName = GetDredDebugName(allocation->ObjectNameA, allocation->ObjectNameW);
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED %s[%u]: type=%s name=%s\n",
				label,
				allocationIndex,
				GetD3D12DredAllocationTypeName(allocation->AllocationType),
				objectName.c_str());
		}
	}

	static void LogD3D12DredAllocationNodes1(const D3D12_DRED_ALLOCATION_NODE1* head, const char* label)
	{
		uint32_t allocationIndex = 0;
		for (const D3D12_DRED_ALLOCATION_NODE1* allocation = head; allocation != nullptr && allocationIndex < 8; allocation = allocation->pNext, ++allocationIndex)
		{
			const std::string objectName = GetDredDebugName(allocation->ObjectNameA, allocation->ObjectNameW);
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED %s[%u]: type=%s name=%s object=%p\n",
				label,
				allocationIndex,
				GetD3D12DredAllocationTypeName(allocation->AllocationType),
				objectName.c_str(),
				allocation->pObject);
		}
	}

	static void ConfigureD3D12Dred()
	{
		if (!nri_dred)
		{
			return;
		}

		const auto getDebugInterface = GetD3D12GetDebugInterfaceFn();
		if (getDebugInterface == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED setup failed: D3D12GetDebugInterface is unavailable.\n");
			return;
		}

		ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
		if (SUCCEEDED(getDebugInterface(IID_PPV_ARGS(&settings1))) && settings1 != nullptr)
		{
			settings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->Release();
			return;
		}

		ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
		if (SUCCEEDED(getDebugInterface(IID_PPV_ARGS(&settings))) && settings != nullptr)
		{
			settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->Release();
			return;
		}

		Printf(TEXTCOLOR_RED "NRI D3D12 DRED setup failed: DRED settings interfaces are unavailable.\n");
	}

	static void ConfigureD3D12DebugLayer()
	{
		if (!nri_apivalidation)
		{
			return;
		}

		const auto getDebugInterface = GetD3D12GetDebugInterfaceFn();
		if (getDebugInterface == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 API validation requested, but D3D12GetDebugInterface is unavailable.\n");
			return;
		}

		ID3D12Debug* debug = nullptr;
		if (FAILED(getDebugInterface(IID_PPV_ARGS(&debug))) || debug == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 API validation requested, but ID3D12Debug is unavailable.\n");
			return;
		}

		debug->EnableDebugLayer();

		ID3D12Debug1* debug1 = nullptr;
		if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))) && debug1 != nullptr)
		{
			debug1->SetEnableGPUBasedValidation(FALSE);
			debug1->SetEnableSynchronizedCommandQueueValidation(FALSE);
			debug1->Release();
		}

		debug->Release();
		Printf("NRI D3D12 debug layer enabled for API validation.\n");
	}

	static void ConfigureD3D12InfoQueue(const nri::CoreInterface& core, nri::Device* device)
	{
		if (!nri_apivalidation || device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		ID3D12InfoQueue* infoQueue = nullptr;
		if (FAILED(d3d12Device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 debug layer is enabled, but ID3D12InfoQueue is unavailable.\n");
			return;
		}

		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_MESSAGE, FALSE);
		Printf("NRI D3D12 info queue configured: debugger breaks disabled while API validation is on.\n");
		infoQueue->Release();
	}

	template<typename T>
	static void SetNriDebugName(const nri::CoreInterface& core, T* object, const char* name)
	{
		if (object == nullptr || name == nullptr || *name == '\0' || core.SetDebugName == nullptr)
		{
			return;
		}

		core.SetDebugName(reinterpret_cast<nri::Object*>(object), name);
	}

	static void LogD3D12DeviceRemovedReason(const nri::CoreInterface& core, nri::Device* device, const char* context)
	{
		if (device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		const HRESULT hr = d3d12Device->GetDeviceRemovedReason();
		if (hr == S_OK)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 removed reason after %s: %s (0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(hr),
				(unsigned)hr);
			return;
		}

		Printf(TEXTCOLOR_RED "NRI D3D12 removed reason after %s: %s (0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(hr),
			(unsigned)hr);
	}

	static void LogD3D12InfoQueueMessages(const nri::CoreInterface& core, nri::Device* device, const char* context)
	{
		if (device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		ID3D12InfoQueue* infoQueue = nullptr;
		if (FAILED(d3d12Device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
		{
			return;
		}

		const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		if (messageCount == 0)
		{
			infoQueue->Release();
			return;
		}

		const UINT64 start = messageCount > 16 ? messageCount - 16 : 0;
		for (UINT64 i = start; i < messageCount; ++i)
		{
			SIZE_T messageSize = 0;
			if (FAILED(infoQueue->GetMessage(i, nullptr, &messageSize)) || messageSize == 0)
			{
				continue;
			}

			std::vector<uint8_t> storage(messageSize);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if (FAILED(infoQueue->GetMessage(i, message, &messageSize)))
			{
				continue;
			}

			Printf(TEXTCOLOR_RED "NRI D3D12 info queue after %s [%u]: %s\n",
				context != nullptr ? context : "unknown",
				(unsigned)message->ID,
				message->pDescription != nullptr ? message->pDescription : "(no description)");
		}

		infoQueue->ClearStoredMessages();
		infoQueue->Release();
	}

	static NRIRenderDevice* GetActiveNRIRenderDevice()
	{
		if (screen == nullptr || screen->Backend() != 4)
		{
			Printf("The NRI backend is not active.\n");
			return nullptr;
		}

		return static_cast<NRIRenderDevice*>(screen);
	}

	class ScopedNriTiming
	{
	public:
		ScopedNriTiming(glcycle_t& aggregate, double& outputMs)
			: mAggregate(aggregate), mOutputMs(outputMs)
		{
			mOutputMs = 0.0;
			mAggregate.Clock();
			mTimer.ResetAndClock();
		}

		~ScopedNriTiming()
		{
			mTimer.Unclock();
			mOutputMs = mTimer.TimeMS();
			mAggregate.Unclock();
		}

		ScopedNriTiming(const ScopedNriTiming&) = delete;
		ScopedNriTiming& operator=(const ScopedNriTiming&) = delete;

	private:
		glcycle_t& mAggregate;
		double& mOutputMs;
		cycle_t mTimer;
	};

	static const char* GetNriResultName(nri::Result result)
	{
		switch (result)
		{
		case nri::Result::INVALID_SDK:
			return "invalid_sdk";
		case nri::Result::SUCCESS:
			return "success";
		case nri::Result::FAILURE:
			return "failure";
		case nri::Result::INVALID_ARGUMENT:
			return "invalid_argument";
		case nri::Result::OUT_OF_MEMORY:
			return "out_of_memory";
		case nri::Result::UNSUPPORTED:
			return "unsupported";
		case nri::Result::DEVICE_LOST:
			return "device_lost";
		case nri::Result::OUT_OF_DATE:
			return "out_of_date";
		default:
			return "other";
		}
	}

	static const char* GetNriMessageTypeName(nri::Message messageType)
	{
		switch (messageType)
		{
		case nri::Message::INFO:
			return "info";
		case nri::Message::WARNING:
			return "warning";
		case nri::Message::ERROR:
			return "error";
		default:
			return "other";
		}
	}

	static const char* GetNriVendorName(nri::Vendor vendor)
	{
		switch (vendor)
		{
		case nri::Vendor::NVIDIA:
			return "NVIDIA";
		case nri::Vendor::AMD:
			return "AMD";
		case nri::Vendor::INTEL:
			return "Intel";
		default:
			return "Unknown";
		}
	}

	static void NRI_CALL NriMessageCallback(nri::Message messageType, const char* file, uint32_t line, const char* message, void*)
	{
		if (file != nullptr && *file != '\0')
		{
			Printf("NRI %s: %s (%s:%u)\n", GetNriMessageTypeName(messageType), message, file, line);
		}
		else
		{
			Printf("NRI %s: %s\n", GetNriMessageTypeName(messageType), message);
		}
	}

	static uint32_t CountSetBits(uint64_t mask)
	{
		uint32_t count = 0;
		while (mask != 0)
		{
			count += (uint32_t)(mask & 1ull);
			mask >>= 1;
		}
		return count;
	}

	static FString DescribeSwapChainFlags(nri::SwapChainBits flags)
	{
		if (flags == nri::SwapChainBits::NONE)
		{
			return "NONE";
		}

		FString description;
		const auto appendFlag = [&description](const char* text)
		{
			if (!description.IsEmpty())
			{
				description << "|";
			}
			description << text;
		};

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::VSYNC) != 0)
		{
			appendFlag("VSYNC");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_TEARING) != 0)
		{
			appendFlag("ALLOW_TEARING");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::WAITABLE) != 0)
		{
			appendFlag("WAITABLE");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0)
		{
			appendFlag("ALLOW_LOW_LATENCY");
		}

		return description;
	}

	static FString DescribeSwapChainImageMask(uint64_t mask, uint32_t textureCount)
	{
		if (textureCount == 0)
		{
			return "none";
		}

		FString description;
		for (uint32_t i = 0; i < textureCount; ++i)
		{
			if ((mask & (1ull << i)) == 0)
			{
				continue;
			}

			if (!description.IsEmpty())
			{
				description << ",";
			}
			description.AppendFormat("%u", i);
		}

		if (description.IsEmpty())
		{
			description = "none";
		}

		return description;
	}

	static FString DescribeSwapChainImageCounts(const std::vector<uint64_t>& counts)
	{
		if (counts.empty())
		{
			return "none";
		}

		FString description;
		for (size_t i = 0; i < counts.size(); ++i)
		{
			if (i != 0)
			{
				description << " ";
			}
			description.AppendFormat("%u:%llu", (uint32_t)i, (unsigned long long)counts[i]);
		}

		return description;
	}

	static uint8_t GetRequestedSwapChainTextureCount()
	{
		if (nri_ptswaptextures > 0)
		{
			return (uint8_t)nri_ptswaptextures;
		}

		return DefaultSwapChainTextureCount;
	}

	static nri::SwapChainBits GetRequestedSwapChainFlags()
	{
		switch ((int)nri_ptswapflags)
		{
		case 0:
			return nri::SwapChainBits::NONE;
		case 1:
			return nri::SwapChainBits::ALLOW_TEARING;
		case 2:
			return nri::SwapChainBits::VSYNC;
		case 3:
			return NRIFlags(nri::SwapChainBits::VSYNC, nri::SwapChainBits::ALLOW_TEARING);
		default:
			return vid_vsync ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::ALLOW_TEARING;
		}
	}

	static bool HasRequestedFrameGenerationProvider()
	{
		return (int)nri_framegenprovider != 0;
	}

	static const char* DescribeSwapChainFlagOverride()
	{
		switch ((int)nri_ptswapflags)
		{
		case -1: return "default";
		case 0: return "NONE";
		case 1: return "ALLOW_TEARING";
		case 2: return "VSYNC";
		case 3: return "VSYNC|ALLOW_TEARING";
		default: return "invalid";
		}
	}

}

extern "C" nri::Result NRI_CALL nriGetInterface(const nri::Device& device, const char* interfaceName, size_t interfaceSize, void* interfacePtr)
{
	return gNriGetInterfaceForwarder != nullptr ? gNriGetInterfaceForwarder(device, interfaceName, interfaceSize, interfacePtr) : nri::Result::FAILURE;
}

extern "C" void NRI_CALL nriDestroyDevice(nri::Device* device)
{
	if (gNriDestroyDeviceForwarder != nullptr)
	{
		gNriDestroyDeviceForwarder(device);
	}
}

CCMD(nri_ptcaps)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingCaps();
	}
}

CCMD(nri_ptstatus)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingStatus();
	}
}

CCMD(nri_ptchunkdump)
{
	const int32_t chunkIndex = argv.argc() > 1 ? atoi(argv[1]) : -1;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingMapChunkDump(chunkIndex);
	}
	else
	{
		Printf("nri_ptchunkdump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptchunkcompare)
{
	const int32_t chunkIndex = argv.argc() > 1 ? atoi(argv[1]) : -1;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingMapChunkCompare(chunkIndex);
	}
	else
	{
		Printf("nri_ptchunkcompare is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptbuffers)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingBuffers();
	}
}

CCMD(nri_ptreset)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ResetPathTracingHistory();
	}
}

CCMD(nri_ptlightspawn)
{
	if (argv.argc() < 5)
	{
		Printf("nri_ptlightspawn <r> <g> <b> <intensity> [radius] [offset]: spawns a PT test point light in front of the local player.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptlightspawn is only available while using the NRI renderer.\n");
		return;
	}

	const float radius = argv.argc() > 5 ? (float)atof(argv[5]) : DefaultPtTestLightRadius;
	const float offset = argv.argc() > 6 ? (float)atof(argv[6]) : DefaultPtTestLightOffset;
	uint32_t lightId = 0;
	frameBuffer->SpawnPathTracingPointLight(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		(float)atof(argv[3]),
		(float)atof(argv[4]),
		radius,
		offset,
		lightId);
}

CCMD(nri_ptlightlist)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingPointLights();
	}
	else
	{
		Printf("nri_ptlightlist is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptmuzzleflash_test)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptmuzzleflash_test <rule_id>: emits one synthetic PT muzzle-flash event from the local player.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptmuzzleflash_test is only available while using the NRI renderer.\n");
		return;
	}

	const ResolvedLightOverlaySet& resolvedSet = GetResolvedLightOverlaySet();
	const char* ruleId = argv[1];
	const ResolvedLightOverlayMuzzleFlashRule* rule = FindResolvedMuzzleFlashRule(resolvedSet, ruleId);
	if (rule == nullptr)
	{
		Printf("nri_ptmuzzleflash_test: no resolved muzzle-flash rule '%s'. available=%s\n",
			ruleId,
			DescribeResolvedMuzzleFlashRuleIds(resolvedSet).GetChars());
		return;
	}

	PathTracingWeaponLightEvent event;
	FString error;
	if (!BuildLocalPlayerWeaponLightEvent(rule->id.GetChars(), DefaultPtTestLightOffset, event, error))
	{
		Printf("nri_ptmuzzleflash_test: %s.\n", error.GetChars());
		return;
	}

	frameBuffer->EmitPathTracingWeaponLightEvent(event);
	Printf("NRI PT muzzle-flash test queued: event=%s actor=%d world_pos=(%.3f, %.3f, %.3f) time=%.4f pending=%u source=%s\n",
		event.eventId.GetChars(),
		event.emitterActorIndex,
		event.worldPosition.X,
		event.worldPosition.Y,
		event.worldPosition.Z,
		event.absoluteTimeSeconds,
		frameBuffer->GetPendingPathTracingWeaponLightEventCount(),
		rule->source.sourceName.GetChars());
}

CCMD(nri_ptsphere)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptsphere <diameter> <distance> [metalness] [roughness]: spawns a PT debug sphere along camera forward.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptsphere is only available while using the NRI renderer.\n");
		return;
	}

	uint32_t sphereId = 0;
	frameBuffer->SpawnPathTracingDebugSphere(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		argv.argc() > 3 ? (float)atof(argv[3]) : DefaultPtTestSphereMetalness,
		argv.argc() > 4 ? (float)atof(argv[4]) : DefaultPtTestSphereRoughness,
		sphereId);
}

CCMD(nri_ptspherespawn)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptspherespawn <diameter> <distance> [metalness] [roughness]: spawns a PT debug sphere along camera forward.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptspherespawn is only available while using the NRI renderer.\n");
		return;
	}

	uint32_t sphereId = 0;
	frameBuffer->SpawnPathTracingDebugSphere(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		argv.argc() > 3 ? (float)atof(argv[3]) : DefaultPtTestSphereMetalness,
		argv.argc() > 4 ? (float)atof(argv[4]) : DefaultPtTestSphereRoughness,
		sphereId);
}

CCMD(nri_ptspherelist)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingDebugSpheres();
	}
	else
	{
		Printf("nri_ptspherelist is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsphereclear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingDebugSpheres();
	}
	else
	{
		Printf("nri_ptsphereclear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsphereremove)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptsphereremove <id>: removes a PT debug sphere by id.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->RemovePathTracingDebugSphere((uint32_t)atoi(argv[1]));
	}
	else
	{
		Printf("nri_ptsphereremove is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_addsprite)
{
	if (argv.argc() < 7)
	{
		Printf("nri_ptlightheuristic_addsprite <tile> <r> <g> <b> <intensity> <radius> [flicker_frames]: adds a PT analytic sprite-tile light heuristic.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingSpriteTileLightHeuristic(
			(uint32_t)atoi(argv[1]),
			(float)atof(argv[2]),
			(float)atof(argv[3]),
			(float)atof(argv[4]),
			(float)atof(argv[5]),
			(float)atof(argv[6]),
			argv.argc() > 7 ? (uint32_t)atoi(argv[7]) : 0u,
			ruleId);
	}
	else
	{
		Printf("nri_ptlightheuristic_addsprite is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_clear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingLightHeuristics();
	}
	else
	{
		Printf("nri_ptlightheuristic_clear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_list)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingLightHeuristics();
	}
	else
	{
		Printf("nri_ptlightheuristic_list is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightclear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingPointLights();
	}
	else
	{
		Printf("nri_ptlightclear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightremove)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptlightremove <id>: removes a PT test point light by id.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->RemovePathTracingPointLight((uint32_t)atoi(argv[1]));
	}
	else
	{
		Printf("nri_ptlightremove is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightdump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSceneLightDump(radius, limit);
	}
	else
	{
		Printf("nri_ptlightdump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightdebug_nearplayer)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 1024.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 16u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSceneLightDump(radius, limit);
	}
	else
	{
		Printf("nri_ptlightdebug_nearplayer is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightclusterdebug)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingLightClusters();
	}
	else
	{
		Printf("nri_ptlightclusterdebug is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_addtile)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptemissiveheuristic_addtile <tile> [intensity_scale]: adds a PT emissive tile rule using base-texture emission.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingTextureEmissiveHeuristic(
			(uint32_t)atoi(argv[1]),
			nri_scene::MaterialEmissiveMode_UseBaseTexture,
			argv.argc() > 2 ? (float)atof(argv[2]) : 1.0f,
			nullptr,
			false,
			ruleId);
	}
	else
	{
		Printf("nri_ptemissiveheuristic_addtile is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_addtilemode)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptemissiveheuristic_addtilemode <tile> <base|glowmap|constant> [intensity_scale] [r g b]: adds a PT emissive tile rule with an explicit source mode.\n");
		return;
	}

	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	const char* modeName = argv[2];
	if (!stricmp(modeName, "base") || !stricmp(modeName, "albedo") || !stricmp(modeName, "texture"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
	}
	else if (!stricmp(modeName, "glowmap") || !stricmp(modeName, "glow"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseGlowmapTexture;
	}
	else if (!stricmp(modeName, "constant") || !stricmp(modeName, "const"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
	}
	else
	{
		Printf("Unknown emissive mode '%s'. Expected one of: base, glowmap, constant.\n", modeName);
		return;
	}

	const float intensityScale = argv.argc() > 3 ? (float)atof(argv[3]) : 1.0f;
	bool hasExplicitColor = false;
	float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
	if (emissiveMode == nri_scene::MaterialEmissiveMode_UseConstantColor && argv.argc() >= 7)
	{
		emissiveColor[0] = (float)atof(argv[4]);
		emissiveColor[1] = (float)atof(argv[5]);
		emissiveColor[2] = (float)atof(argv[6]);
		hasExplicitColor = true;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingTextureEmissiveHeuristic(
			(uint32_t)atoi(argv[1]),
			emissiveMode,
			intensityScale,
			hasExplicitColor ? emissiveColor : nullptr,
			hasExplicitColor,
			ruleId);
	}
	else
	{
		Printf("nri_ptemissiveheuristic_addtilemode is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_clear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingEmissiveHeuristics();
	}
	else
	{
		Printf("nri_ptemissiveheuristic_clear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_list)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingEmissiveHeuristics();
	}
	else
	{
		Printf("nri_ptemissiveheuristic_list is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissivedump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingEmissiveSurfaces(radius, limit);
	}
	else
	{
		Printf("nri_ptemissivedump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsectorlight_set)
{
	if (argv.argc() < 4)
	{
		Printf("nri_ptsectorlight_set <ambientScale> <hemiScale> <fogScale>: updates PT sector-light heuristic scales.\n");
		return;
	}

	nri_ptsectorambientscale = (float)atof(argv[1]);
	nri_ptsectorhemiscale = (float)atof(argv[2]);
	nri_ptsectorfogscale = (float)atof(argv[3]);
	Printf("NRI PT sector-light scales set: ambient=%.3f hemi=%.3f fog=%.3f\n",
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale);
}

CCMD(nri_ptsectorlight_filter)
{
	if (argv.argc() < 4)
	{
		Printf("nri_ptsectorlight_filter <pal|-1> <minShade> <maxShade> [lotag]: updates PT sector-light heuristic filters.\n");
		return;
	}

	nri_ptsectorfilterpal = atoi(argv[1]);
	nri_ptsectorfilterminshade = atoi(argv[2]);
	nri_ptsectorfiltermaxshade = atoi(argv[3]);
	nri_ptsectorfilterlotag = argv.argc() > 4 ? atoi(argv[4]) : -1;
	Printf("NRI PT sector-light filter set: pal=%d shade=[%d,%d] lotag=%d\n",
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag);
}

CCMD(nri_ptsectorlight_clear)
{
	nri_ptsectorlighting = true;
	nri_ptsectorambientscale = 0.20f;
	nri_ptsectorhemiscale = 0.12f;
	nri_ptsectorfogscale = 0.20f;
	nri_ptsectorclamp = 1.0f;
	nri_ptsectorfilterpal = -1;
	nri_ptsectorfilterminshade = -128;
	nri_ptsectorfiltermaxshade = 127;
	nri_ptsectorfilterlotag = -1;
	nri_ptsectorpulseframes = 0;
	nri_ptsectorpulseamount = 0.0f;
	Printf("NRI PT sector-light heuristics cleared.\n");
}

CCMD(nri_ptsectorlightdump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSectorLights(radius, limit);
	}
	else
	{
		Printf("nri_ptsectorlightdump is only available while using the NRI renderer.\n");
	}
}

NRIRenderDevice::NRIRenderDevice(void* hMonitor, bool fullscreen)
	: SystemBaseFrameBuffer(hMonitor, fullscreen), mRenderState(std::make_unique<NRIRenderState>(this))
{
	vendorstring = "NRI";
	glslversion = 6.6f;
	mRenderer = std::make_unique<NRIRenderer>(this);
}

NRIRenderDevice::~NRIRenderDevice()
{
	WaitForCommands(true);

	delete mVertexData;
	mVertexData = nullptr;
	delete mSkyData;
	mSkyData = nullptr;
	delete mViewpoints;
	mViewpoints = nullptr;
	delete mLights;
	mLights = nullptr;
	delete mBones;
	mBones = nullptr;

	DestroySwapChain();
	mFrameGeneration.Shutdown();
	if (mRenderer != nullptr)
	{
		mRenderer->Shutdown();
	}
	DestroyRenderResources();

	DestroyQueuedFrames();

	if (mFrameFence != nullptr)
	{
		mCore.DestroyFence(mFrameFence);
		mFrameFence = nullptr;
	}

	if (mStreamerInstance != nullptr)
	{
		mStreamer.DestroyStreamer(mStreamerInstance);
		mStreamerInstance = nullptr;
	}

	if (mDevice != nullptr && mDestroyDeviceFn != nullptr)
	{
		mDestroyDeviceFn(mDevice);
		mDevice = nullptr;
	}

	if (mNriModule != nullptr)
	{
		FreeLibrary((HMODULE)mNriModule);
		mNriModule = nullptr;
	}

	gNriDestroyDeviceForwarder = nullptr;
	gNriGetInterfaceForwarder = nullptr;
}

void NRIRenderDevice::Update()
{
	double draw2DMs = 0.0;
	double endFrameMs = 0.0;
	double presentShellMs = 0.0;
	double baseUpdateMs = 0.0;
	const double updateStartMs = I_msTimeF();

	if (mInitialized && mFrameBegun)
	{
		double stageStartMs = I_msTimeF();
		if (mFrameGenerationUiTargetActive)
		{
			const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
			if (sceneBlendPrefixCount > 0u)
			{
				SetActiveRenderTarget();
				DrawFrameGenerationSceneBlendPrefix();
				if (NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource(); uiTarget != nullptr)
				{
					mActiveTarget = uiTarget;
					mFrameGenerationUiTargetActive = true;
				}
			}

			Draw2D();
			twod->Clear();
			FinalizeFrameGenerationUiTarget();
			if (!IsFrameGenerationPresentPathActive())
			{
				CompositeFrameGenerationUiTexture();
				Draw2D();
				twod->Clear();
			}
		}
		else
		{
			SetActiveRenderTarget();
			Draw2D();
			twod->Clear();
		}
		draw2DMs = I_msTimeF() - stageStartMs;
		stageStartMs = I_msTimeF();
		mRenderState->EndFrame();
		endFrameMs = I_msTimeF() - stageStartMs;
		stageStartMs = I_msTimeF();
		EndFrameAndPresent();
		presentShellMs = I_msTimeF() - stageStartMs;
	}

	double stageStartMs = I_msTimeF();
	Super::Update();
	baseUpdateMs = I_msTimeF() - stageStartMs;

	if (PerfLoopTraceActive())
	{
		Printf(
			"PERF update trace NRI: frame=%llu draw2d=%.3f endframe=%.3f present_shell=%.3f base=%.3f total=%.3f frame_begun=%d framegen_ui=%d acquired=%d presented=%d\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			draw2DMs,
			endFrameMs,
			presentShellMs,
			baseUpdateMs,
			I_msTimeF() - updateStartMs,
			mFrameBegun ? 1 : 0,
			mFrameGenerationUiTargetActive ? 1 : 0,
			mHasAcquiredSwapChainImage ? 1 : 0,
			mHasPresentedSwapChainFrame ? 1 : 0);
	}
}

void NRIRenderDevice::InitializeState()
{
	SetViewportRects(nullptr);

	if (!LoadNRI() || !CreateDevice() || !CreateRenderResources() || !CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		mInitialized = false;
		return;
	}
	mFrameGeneration.Initialize(*this);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(mPipelineNbr);
	mLights = new FLightBuffer(mPipelineNbr);
	mBones = new BoneBuffer(mPipelineNbr);

	LogStartup();
	if (mRenderer != nullptr && !mRenderer->Initialize())
	{
		Printf(TEXTCOLOR_RED "NRI path tracing renderer initialization failed.\n");
	}
	mInitialized = true;
}

bool NRIRenderDevice::CompileNextShader()
{
	return true;
}

int NRIRenderDevice::GetShaderCount()
{
	return 0;
}

const char* NRIRenderDevice::DeviceName() const
{
	return mDeviceName.GetChars();
}

void NRIRenderDevice::BeginFrame()
{
	if (!mInitialized)
	{
		return;
	}

	if (mFrameBegun)
	{
		return;
	}

	mFrameGeneration.BeginFrame(*this);

	mTraceThisFrame = false;
	if (nri_pttraceframes > 0)
	{
		mTraceThisFrame = true;
	}

	Reset2DTextureFrameStats();
	mLastFrameBoundaryStats.frameNumber++;
	mLastFrameBoundaryStats.frameIndex = mFrameIndex;
	mLastFrameBoundaryStats.waitMs = 0.0;
	mLastFrameBoundaryStats.waitForPresentMs = 0.0;
	mLastFrameBoundaryStats.acquireMs = 0.0;
	mLastFrameBoundaryStats.submitMs = 0.0;
	mLastFrameBoundaryStats.presentMs = 0.0;
	mLastFrameBoundaryStats.submittedFenceValue = 0;
	mLastFrameBoundaryStats.waitForPresentResult = nri::Result::SUCCESS;
	mLastFrameBoundaryStats.acquireResult = nri::Result::FAILURE;
	mLastFrameBoundaryStats.presentResult = nri::Result::FAILURE;
	mCurrentQueuedFrameIndex = GetQueuedFrameIndex(mFrameIndex);
	mLastFrameBoundaryStats.queuedFrameIndex = mCurrentQueuedFrameIndex;
	mLastFrameBoundaryStats.swapChainImageIndex = 0;
	mLastFrameBoundaryStats.acquireSemaphoreIndex = 0;
	mLastFrameBoundaryStats.sanityModeEnabled = !!nri_ptsanity;
	mLastFrameBoundaryStats.sanityFrameUsed = false;
	mLastFrameBoundaryStats.sceneTargetSelected = false;
	mLastFrameBoundaryStats.pathTracedSceneRendered = false;
	mLastFrameBoundaryStats.sceneCopiedToPresent = false;
	mLastFrameBoundaryStats.postProcessInvoked = false;
	SelectQueuedFrame(mCurrentQueuedFrameIndex);

	{
		ScopedNriTiming waitTiming(NriPTFrameWait, mLastFrameBoundaryStats.waitMs);
		WaitForCommands(false);
	}
	SetViewportRects(nullptr);

	if (!EnsureSwapChainSize())
	{
		return;
	}

	mAcquireSemaphoreIndex = mSwapChainImages.empty() ? 0 : (uint32_t)(mFrameIndex % mSwapChainImages.size());
	mLastFrameBoundaryStats.acquireSemaphoreIndex = mAcquireSemaphoreIndex;

	if (IsFrameGenerationPresentPathActive())
	{
		IDXGISwapChain4* frameGenSwapChain = mFrameGeneration.GetPresentSwapChain();
		if (frameGenSwapChain == nullptr)
		{
			mFrameGeneration.RequestNativeFallback("present-bridge-invalidated");
			WaitForCommands(true);
			CreateSwapChain();
			return;
		}

		mCurrentSwapChainImage = frameGenSwapChain->GetCurrentBackBufferIndex();
		mLastFrameBoundaryStats.acquireResult = nri::Result::SUCCESS;
		mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
		mHasAcquiredSwapChainImage = false;
		if (mCurrentSwapChainImage >= mFrameGenerationPresentImages.size())
		{
			Printf(TEXTCOLOR_RED "NRI framegen present bridge returned backbuffer index %u outside wrapped image range %u.\n",
				mCurrentSwapChainImage,
				(unsigned)mFrameGenerationPresentImages.size());
			mFrameGeneration.RequestNativeFallback("present-bridge-invalidated");
			WaitForCommands(true);
			CreateSwapChain();
			return;
		}
		mCurrentPresentTarget = &mFrameGenerationPresentImages[mCurrentSwapChainImage];
	}
	else
	{
		const bool waitableSwapChain = ((uint32_t)mSwapChainFlags & (uint32_t)nri::SwapChainBits::WAITABLE) != 0;
		const bool allowWaitForPresent = waitableSwapChain;
		if (nri_ptwaitpresent && allowWaitForPresent && mHasPresentedSwapChainFrame && mSwapChain != nullptr)
		{
			nri::Result waitForPresentResult = nri::Result::FAILURE;
			{
				ScopedNriTiming waitPresentTiming(NriPTWaitPresent, mLastFrameBoundaryStats.waitForPresentMs);
				waitForPresentResult = mSwapChainInterface.WaitForPresent(*mSwapChain);
			}
			mLastFrameBoundaryStats.waitForPresentResult = waitForPresentResult;
		}

		nri::Result acquireResult = nri::Result::FAILURE;
		{
			ScopedNriTiming acquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
			acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
		}
		mLastFrameBoundaryStats.acquireResult = acquireResult;
		mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
		if (acquireResult == nri::Result::SUCCESS)
		{
			NoteSwapChainAcquire(mCurrentSwapChainImage);
		}
		if (acquireResult == nri::Result::OUT_OF_DATE)
		{
			if (!EnsureSwapChainSize())
			{
				return;
			}

			{
				ScopedNriTiming reacquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
				acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
			}
			mLastFrameBoundaryStats.acquireResult = acquireResult;
			mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
			if (acquireResult == nri::Result::SUCCESS)
			{
				NoteSwapChainAcquire(mCurrentSwapChainImage);
			}
		}

		if (acquireResult != nri::Result::SUCCESS)
		{
			if (acquireResult == nri::Result::DEVICE_LOST)
			{
				mFrameGeneration.NoteReset("device-lost");
			}
			Printf(TEXTCOLOR_RED "NRI failed to acquire swapchain image.\n");
			LogD3D12FailureDiagnostics("AcquireNextTexture");
			return;
		}

		mHasAcquiredSwapChainImage = true;
		mCurrentPresentTarget = &mSwapChainImages[mCurrentSwapChainImage].target;
	}

	// Match NRD-Sample's swapchain handling: each acquired image re-enters command recording
	// with unknown local state and must be explicitly transitioned before first use.
	mCurrentPresentTarget->state = {};
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
	if (!BeginCommandList("BeginFrame", false))
	{
		ResetFrameTracking(false);
		return;
	}
	mRenderState->BeginFrame();

	if (mViewpoints != nullptr)
	{
		mViewpoints->Clear();
	}

	mFrameBegun = true;
}

FRenderState* NRIRenderDevice::RenderState()
{
	return mRenderState.get();
}

void NRIRenderDevice::Draw2D()
{
	if (!mInitialized || twod == nullptr)
	{
		return;
	}

	struct Draw2DTraceStats
	{
		uint32_t commands = 0;
		uint32_t specialCommands = 0;
		uint32_t texturedCommands = 0;
		uint32_t canvasTextureCommands = 0;
		uint32_t scissorCommands = 0;
		uint32_t transformedCommands = 0;
		uint32_t shapeCommands = 0;
		uint32_t triangleCommands = 0;
		uint32_t lineCommands = 0;
		uint32_t pointCommands = 0;
		int32_t vertices = 0;
		int32_t indices = 0;
	};

	auto collectTraceStats = [](F2DDrawer* drawer)
	{
		Draw2DTraceStats stats;
		if (drawer == nullptr)
		{
			return stats;
		}

		stats.vertices = drawer->mVertices.Size();
		stats.indices = drawer->mIndices.Size();
		stats.commands = (uint32_t)drawer->mData.Size();
		for (const auto& cmd : drawer->mData)
		{
			if (cmd.isSpecial != SpecialDrawCommand::NotSpecial)
			{
				stats.specialCommands++;
				continue;
			}

			if (cmd.mTexture != nullptr && cmd.mTexture->isValid())
			{
				stats.texturedCommands++;
				if (cmd.mTexture->isHardwareCanvas())
				{
					stats.canvasTextureCommands++;
				}
			}
			if ((cmd.mFlags & F2DDrawer::DTF_Scissor) != 0)
			{
				stats.scissorCommands++;
			}
			if (cmd.useTransform)
			{
				stats.transformedCommands++;
			}
			if (cmd.shape2DBufInfo != nullptr)
			{
				stats.shapeCommands++;
			}

			switch (cmd.mType)
			{
			case F2DDrawer::DrawTypeLines:
				stats.lineCommands++;
				break;
			case F2DDrawer::DrawTypePoints:
				stats.pointCommands++;
				break;
			case F2DDrawer::DrawTypeTriangles:
			default:
				stats.triangleCommands++;
				break;
			}
		}
		return stats;
	};

	const char* drawMode = "full";
	F2DDrawer* activeDrawer = twod;
	F2DDrawer uiDrawer;
	const double drawStartMs = I_msTimeF();

	if (mFrameGenerationUiTargetActive)
	{
		const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
		if (sceneBlendPrefixCount >= twod->mData.Size())
		{
			return;
		}

		if (sceneBlendPrefixCount > 0)
		{
			uiDrawer = *twod;
			uiDrawer.mData.Delete(0, (int)sceneBlendPrefixCount);
			activeDrawer = &uiDrawer;
			drawMode = "ui-suffix";
		}
	}

	const auto traceStats = collectTraceStats(activeDrawer);
	::Draw2D(activeDrawer, *mRenderState);

	if (PerfLoopTraceActive())
	{
		const auto rsTrace = mRenderState->GetPerfTraceStats();
		const auto& texStats = mTexture2DDebugStats;
		Printf(
			"PERF draw2d trace NRI: frame=%llu mode=%s draw_ms=%.3f cmds=%u specials=%u textured=%u canvas=%u scissor=%u xform=%u shapes=%u tris=%u lines=%u points=%u verts=%d indices=%d ensure=%u hits=%u misses=%u uploads=%u recreate=%u bytes=%llu apply_calls=%u indexed=%u pipe_create=%u apply_ms=%.3f pipe_ms=%.3f vstream_ms=%.3f istream_ms=%.3f ensure_ms=%.3f begin_ms=%.3f bind_ms=%.3f drawcall_ms=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			drawMode,
			I_msTimeF() - drawStartMs,
			traceStats.commands,
			traceStats.specialCommands,
			traceStats.texturedCommands,
			traceStats.canvasTextureCommands,
			traceStats.scissorCommands,
			traceStats.transformedCommands,
			traceStats.shapeCommands,
			traceStats.triangleCommands,
			traceStats.lineCommands,
			traceStats.pointCommands,
			traceStats.vertices,
			traceStats.indices,
			texStats.ensureCalls,
			texStats.cacheHits,
			texStats.cacheMisses,
			texStats.uploadAttempts,
			texStats.resourceRecreates,
			(unsigned long long)texStats.uploadedBytes,
			rsTrace.applyCalls,
			rsTrace.indexedCalls,
			rsTrace.pipelineCreates,
			rsTrace.applyMs,
			rsTrace.pipelineMs,
			rsTrace.vertexStreamMs,
			rsTrace.indexStreamMs,
			rsTrace.textureEnsureMs,
			rsTrace.beginRenderingMs,
			rsTrace.bindStateMs,
			rsTrace.drawCallMs);
		mRenderState->ResetPerfTraceStats();
	}
}

void NRIRenderDevice::SetVSync(bool vsync)
{
	Super::SetVSync(vsync);

	if (!mInitialized || mDevice == nullptr || mGraphicsQueue == nullptr)
	{
		return;
	}

	mFrameGeneration.NoteReset("vsync-change");
	WaitForCommands(true);
	if (!CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI failed to recreate swapchain after vsync change.\n");
	}
}

bool NRIRenderDevice::ShouldRequestFrameGenerationLowLatencySwapChain() const
{
	if (!nri_framegen || !nri_framegenlatency || !HasRequestedFrameGenerationProvider())
	{
		return false;
	}

	if (mDevice == nullptr || mSwapChainInterface.CreateSwapChain == nullptr)
	{
		return false;
	}

	if (GetSelectedAPI() != nri::GraphicsAPI::D3D12 || IsFullscreenModeActive())
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	if (!deviceDesc.features.lowLatency)
	{
		return false;
	}

	return
		mLowLatency.SetLatencySleepMode != nullptr &&
		mLowLatency.SetLatencyMarker != nullptr &&
		mLowLatency.LatencySleep != nullptr &&
		mLowLatency.GetLatencyReport != nullptr;
}

nri::SwapChainBits NRIRenderDevice::GetEffectiveRequestedSwapChainFlags() const
{
	nri::SwapChainBits flags = GetRequestedSwapChainFlags();
	if (ShouldRequestFrameGenerationLowLatencySwapChain())
	{
		flags = NRIFlags(flags, nri::SwapChainBits::ALLOW_LOW_LATENCY);
	}
	return flags;
}

bool NRIRenderDevice::ShouldUseFrameGenerationUiTarget() const
{
	if (!mInitialized || mFrameBegun == false || mUsingSaveTarget || mCurrentPresentTarget == nullptr)
	{
		return false;
	}

	const auto& policy = mFrameGeneration.GetPolicy();
	return
		policy.requestedEnabled &&
		policy.requestedProvider != NRIFrameGenerationProvider::Off &&
		policy.resolvedUiMode == NRIFrameGenerationUiMode::UiTexture;
}

uint32_t NRIRenderDevice::GetFrameGenerationSceneBlendPrefixCount() const
{
	if (twod == nullptr)
	{
		return 0u;
	}

	uint32_t count = 0u;
	for (const auto& cmd : twod->mData)
	{
		if (!IsFullscreenPaletteBlendCommand(*twod, cmd))
		{
			break;
		}

		++count;
	}

	return count;
}

bool NRIRenderDevice::EnsureFrameGenerationUiTexture(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	if (mFrameGenerationUiTexture == nullptr)
	{
		mFrameGenerationUiTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
	}

	auto* wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return false;
	}

	if (wrapper->GetWidth() != (int)width || wrapper->GetHeight() != (int)height)
	{
		delete mFrameGenerationUiTexture;
		mFrameGenerationUiTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
		wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
		if (wrapper == nullptr)
		{
			return false;
		}
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return false;
	}

	hwTex->EnsureCanvas(wrapper);
	return hwTex->GetResource().texture != nullptr && hwTex->GetResource().colorAttachmentView != nullptr;
}

NRITextureResource* NRIRenderDevice::GetFrameGenerationUiTargetResource() const
{
	auto* wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return nullptr;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return nullptr;
	}

	return &hwTex->GetResource();
}

void NRIRenderDevice::ClearTargetColor(NRITextureResource& target, float red, float green, float blue, float alpha)
{
	if (mCommandBuffer == nullptr || target.colorAttachmentView == nullptr)
	{
		return;
	}

	mRenderState->EndFrame();
	PrepareTargetForRendering(target, true);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = target.colorAttachmentView;
	colorAttachment.loadOp = nri::LoadOp::CLEAR;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = red;
	colorAttachment.clearValue.color.f.y = green;
	colorAttachment.clearValue.color.f.z = blue;
	colorAttachment.clearValue.color.f.w = alpha;

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mCore.CmdBeginRendering(*mCommandBuffer, renderingDesc);
	mCore.CmdEndRendering(*mCommandBuffer);
	mActiveTarget = &target;
	mRenderState->NotifyExternalTargetWrite();
}

void NRIRenderDevice::BeginFrameGenerationUiTarget()
{
	if (mCurrentPresentTarget == nullptr)
	{
		return;
	}

	if (!EnsureFrameGenerationUiTexture(mCurrentPresentTarget->width, mCurrentPresentTarget->height))
	{
		return;
	}

	NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource();
	if (uiTarget == nullptr)
	{
		return;
	}

	ClearTargetColor(*uiTarget, 0.0f, 0.0f, 0.0f, 0.0f);
	mActiveTarget = uiTarget;
	mFrameGenerationUiTargetActive = true;
}

void NRIRenderDevice::DrawFrameGenerationSceneBlendPrefix()
{
	if (twod == nullptr)
	{
		return;
	}

	const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
	if (sceneBlendPrefixCount == 0u)
	{
		return;
	}

	F2DDrawer sceneBlendDrawer = *twod;
	sceneBlendDrawer.mData.Clamp(sceneBlendPrefixCount);
	::Draw2D(&sceneBlendDrawer, *mRenderState);
}

void NRIRenderDevice::FinalizeFrameGenerationUiTarget()
{
	if (!mFrameGenerationUiTargetActive)
	{
		return;
	}

	NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource();
	if (uiTarget != nullptr)
	{
		TransitionTexture(*uiTarget, NRIShaderResourceState());
		mFrameGeneration.SetUiTexture(uiTarget);
	}

	mRenderState->EndFrame();
	mActiveTarget = mCurrentPresentTarget;
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::CompositeFrameGenerationUiTexture()
{
	if (mFrameGenerationUiTexture == nullptr || mCurrentPresentTarget == nullptr || twod == nullptr)
	{
		return;
	}

	SetActiveRenderTarget();
	DrawTexture(twod, mFrameGenerationUiTexture, 0, 0, DTA_Masked, false, TAG_DONE);
}

void NRIRenderDevice::DestroyFrameGenerationUiTexture()
{
	delete mFrameGenerationUiTexture;
	mFrameGenerationUiTexture = nullptr;
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::WaitForCommands(bool finish)
{
	if (mDevice == nullptr)
	{
		return;
	}

	if (finish)
	{
		mCore.DeviceWaitIdle(mDevice);
		return;
	}

	if (mFrameFence == nullptr || mQueuedFrames.empty())
	{
		return;
	}

	if (mFrameIndex < mQueuedFrames.size())
	{
		return;
	}

	const uint64_t recycleFenceValue = 1 + mFrameIndex - mQueuedFrames.size();
	if (recycleFenceValue != 0)
	{
		mCore.Wait(*mFrameFence, recycleFenceValue);
	}
}

void NRIRenderDevice::SetSaveBuffers(bool yes)
{
	mUsingSaveTarget = yes;
	if (!mInitialized)
	{
		return;
	}

	if (yes && (mSaveTarget.texture == nullptr || mSaveTarget.width != SAVEPICWIDTH || mSaveTarget.height != SAVEPICHEIGHT))
	{
		DestroyTextureResource(mSaveTarget);
		CreateOwnedTexture(mSaveTarget, SAVEPICWIDTH, SAVEPICHEIGHT, nri::Format::BGRA8_UNORM, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::COLOR_ATTACHMENT));
	}

	mRenderState->EndFrame();
	mActiveTarget = yes ? &mSaveTarget : mCurrentPresentTarget;
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::ImageTransitionScene(bool)
{
}

void NRIRenderDevice::SetSceneRenderTarget(bool)
{
	if (!mInitialized)
	{
		return;
	}

	if (mUsingSaveTarget)
	{
		mRenderState->EndFrame();
		mActiveTarget = &mSaveTarget;
		mLastFrameBoundaryStats.sceneTargetSelected = false;
		return;
	}

	if (mCurrentPresentTarget == nullptr)
	{
		return;
	}

	if (mSceneTarget.texture == nullptr ||
		mSceneTarget.width != mCurrentPresentTarget->width ||
		mSceneTarget.height != mCurrentPresentTarget->height ||
		mSceneTarget.format != mCurrentPresentTarget->format)
	{
		DestroyTextureResource(mSceneTarget);
		if (!CreateOwnedTexture(mSceneTarget,
			mCurrentPresentTarget->width,
			mCurrentPresentTarget->height,
			mCurrentPresentTarget->format,
			NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::COLOR_ATTACHMENT)))
		{
			Printf(TEXTCOLOR_RED "NRI failed to create the scene render target.\n");
			mActiveTarget = mCurrentPresentTarget;
			mLastFrameBoundaryStats.sceneTargetSelected = false;
			return;
		}
	}

	mRenderState->EndFrame();
	mActiveTarget = &mSceneTarget;
	mLastFrameBoundaryStats.sceneTargetSelected = true;
}

void NRIRenderDevice::SetActiveRenderTarget()
{
	mRenderState->EndFrame();
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
	mLastFrameBoundaryStats.sceneTargetSelected = (mActiveTarget == &mSceneTarget);
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::PostProcessScene(bool swscene, int, float, const std::function<void()> &afterBloomDrawEndScene2D)
{
	static bool sLoggedScenePostProcessCopy = false;

	if (!mInitialized)
	{
		if (afterBloomDrawEndScene2D)
		{
			afterBloomDrawEndScene2D();
		}
		return;
	}

	mLastFrameBoundaryStats.postProcessInvoked = true;

	if (!mUsingSaveTarget && !swscene && mCommandBuffer != nullptr &&
		mCurrentPresentTarget != nullptr && mSceneTarget.texture != nullptr && mActiveTarget == &mSceneTarget)
	{
		if (nri_ptdebug > 0 && !sLoggedScenePostProcessCopy)
		{
			Printf("NRI scene postprocess: copying scene target %ux%u to the present target before 2D composition.\n",
				mSceneTarget.width,
				mSceneTarget.height);
			sLoggedScenePostProcessCopy = true;
		}
		mRenderState->EndFrame();
		TransitionTexture(mSceneTarget, NRICopySourceState());
		TransitionTexture(*mCurrentPresentTarget, NRICopyDestinationState());
		mCore.CmdCopyTexture(*mCommandBuffer, *mCurrentPresentTarget->texture, nullptr, *mSceneTarget.texture, nullptr);
		mActiveTarget = mCurrentPresentTarget;
		mRenderState->NotifyExternalTargetWrite();
		mLastFrameBoundaryStats.sceneCopiedToPresent = true;
	}
	else
	{
		SetActiveRenderTarget();
	}

	if (afterBloomDrawEndScene2D)
	{
		afterBloomDrawEndScene2D();
	}

	if (ShouldUseFrameGenerationUiTarget())
	{
		BeginFrameGenerationUiTarget();
	}
}

bool NRIRenderDevice::RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal)
{
	static bool sLoggedFirstSceneAttempt = false;
	static bool sLoggedFrameShellSkip = false;

	if (!mInitialized)
	{
		return false;
	}

	if (nri_ptdebug > 0 && !sLoggedFirstSceneAttempt)
	{
		Printf("NRI RenderPathTracedScene: drawmode=%d portal=%s active_target=%s size=%ux%u\n",
			drawmode,
			portal ? "yes" : "no",
			mActiveTarget == &mSceneTarget ? "scene" : (mActiveTarget == mCurrentPresentTarget ? "present" : (mActiveTarget == &mSaveTarget ? "save" : "other")),
			mActiveTarget != nullptr ? mActiveTarget->width : 0,
			mActiveTarget != nullptr ? mActiveTarget->height : 0);
		sLoggedFirstSceneAttempt = true;
	}

	if (nri_ptsanity && drawmode == DM_MAINVIEW && !portal)
	{
		mLastFrameBoundaryStats.sanityFrameUsed = true;
		return RenderPathTracingSanityFrame();
	}

	if (!mUsingSaveTarget && (!mFrameBegun || mCommandBuffer == nullptr || mActiveTarget == nullptr))
	{
		if (nri_ptdebug > 0 && !sLoggedFrameShellSkip)
		{
			Printf(TEXTCOLOR_ORANGE "NRI skipping raster fallback because the onscreen frame shell is unavailable (frame_begun=%s command_buffer=%s active_target=%s).\n",
				mFrameBegun ? "true" : "false",
				mCommandBuffer != nullptr ? "yes" : "no",
				mActiveTarget != nullptr ? "yes" : "no");
			sLoggedFrameShellSkip = true;
		}
		return true;
	}

	if (mRenderer == nullptr)
	{
		return false;
	}

	const bool rendered = mRenderer->RenderScene(di, drawmode, portal);
	mLastFrameBoundaryStats.pathTracedSceneRendered = mLastFrameBoundaryStats.pathTracedSceneRendered || rendered;
	if (rendered && PerfLoopTraceActive())
	{
		const auto& shell = mRenderer->GetLastPerfShellTraceStats();
		const auto& resource = mRenderer->GetLastPerfResourceTraceStats();
		Printf(
			"PERF pt shell trace NRI: frame=%llu total=%.3f init=%.3f map=%.3f state=%.3f select=%.3f lights=%.3f resident=%.3f emissive=%.3f emissive_tlas=%.3f surface=%.3f graph=%.3f other=%.3f used_static=%d used_dynamic=%d persistent=%d prims=%u dyn_prims=%u mats=%u scene_instances=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.totalMs,
			shell.initResourcesMs,
			shell.mapWorldMs,
			shell.updateStateMs,
			shell.sceneSelectMs,
			shell.sceneLightsMs,
			shell.residentLightRefreshMs,
			shell.emissiveUpdateMs,
			shell.emissiveTlasMs,
			shell.surfaceProbeMs,
			shell.frameGraphMs,
			shell.otherMs,
			shell.usedStaticMapScene ? 1 : 0,
			shell.usedDynamicOverlay ? 1 : 0,
			shell.usedPersistentDynamicEmissiveCache ? 1 : 0,
			shell.activePrimitiveCount,
			shell.dynamicPrimitiveCount,
			shell.activeMaterialCount,
			shell.sceneInstanceCount);
		Printf(
			"PERF pt shell detail NRI: frame=%llu static_scene=%.3f mutation=%.3f mutation_analyze=%.3f mutation_rebuild=%.3f mutation_append=%.3f mutation_dirty=%u mutation_rebuilt=%u mutation_held=%u mutation_replaced=%u mutation_prims=%u mutation_mats=%u spacelink=%.3f spacelink_prims=%u spacelink_mats=%u debug_sphere=%.3f debug_view=%.3f debug_geo=%.3f debug_mats=%.3f debug_tune=%.3f debug_spheres=%u debug_lons=%u debug_lats=%u debug_prims=%u debug_mats_out=%u overlay=%.3f overlay_prims=%u overlay_mats=%u dynamic_capture=%.3f persistent=%.3f dynamic_as=%.3f dynamic_as_create=%.3f dynamic_as_scratch=%.3f dynamic_as_build=%.3f dynamic_as_barrier=%.3f dynamic_as_prims=%u dynamic_as_verts=%u dynamic_as_indices=%u restore_static=%.3f copy_final=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.staticSceneMs,
			shell.runtimeMutationMs,
			shell.runtimeMutationAnalyzeMs,
			shell.runtimeMutationRebuildMs,
			shell.runtimeMutationAppendMs,
			shell.runtimeMutationDirtyChunks,
			shell.runtimeMutationRebuiltChunks,
			shell.runtimeMutationHeldChunks,
			shell.runtimeMutationReplacedChunks,
			shell.runtimeMutationPrimitiveCount,
			shell.runtimeMutationMaterialCount,
			shell.runtimeSpaceLinkMs,
			shell.runtimeSpaceLinkPrimitiveCount,
			shell.runtimeSpaceLinkMaterialCount,
			shell.runtimeDebugSphereMs,
			shell.runtimeDebugSphereViewMs,
			shell.runtimeDebugSphereGeoMs,
			shell.runtimeDebugSphereMaterialMs,
			shell.runtimeDebugSphereTuneMs,
			shell.runtimeDebugSphereCount,
			shell.runtimeDebugSphereLongitudeSegments,
			shell.runtimeDebugSphereLatitudeSegments,
			shell.runtimeDebugSpherePrimitiveCount,
			shell.runtimeDebugSphereMaterialCount,
			shell.overlayAssembleMs,
			shell.overlayPrimitiveCount,
			shell.overlayMaterialCount,
			shell.dynamicCaptureMs,
			shell.persistentDynamicMs,
			shell.dynamicAsMs,
			shell.dynamicAsCreateMs,
			shell.dynamicAsScratchMs,
			shell.dynamicAsBuildMs,
			shell.dynamicAsBarrierMs,
			shell.dynamicAsPrimitiveCount,
			shell.dynamicAsVertexCount,
			shell.dynamicAsIndexCount,
			shell.restoreStaticSceneMs,
			shell.copyFinalMs);
		Printf(
			"PERF pt resource trace NRI: frame=%llu waits=%u wait_ms=%.3f grow=%u overwrite=%u scene_uploads=%u scene_bytes=%llu data_uploads=%u data_bytes=%llu emissive_uploads=%u emissive_bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			resource.waitCalls,
			resource.waitMs,
			resource.growEvents,
			resource.overwriteEvents,
			resource.sceneUploadCalls,
			(unsigned long long)resource.sceneUploadBytes,
			resource.sceneDataUploadCalls,
			(unsigned long long)resource.sceneDataUploadBytes,
			resource.emissiveUploadCalls,
			(unsigned long long)resource.emissiveUploadBytes);
	}
	return rendered;
}

bool NRIRenderDevice::HasActiveSceneFrame() const
{
	return mInitialized && mFrameBegun && mCommandBuffer != nullptr && mActiveTarget != nullptr;
}

bool NRIRenderDevice::StartPathTracingLevelPreload()
{
	if (!mInitialized || mRenderer == nullptr)
	{
		mPathTracingLevelPreloadPending = false;
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability() || !mRenderer->IsPathTracingSupported())
	{
		mPathTracingLevelPreloadPending = false;
		return false;
	}

	mPathTracingLevelPreloadPending = true;
	return true;
}

bool NRIRenderDevice::TickPathTracingLevelPreload()
{
	if (!mPathTracingLevelPreloadPending)
	{
		return true;
	}

	if (!mInitialized || mRenderer == nullptr)
	{
		mPathTracingLevelPreloadPending = false;
		return true;
	}

	if (!mFrameBegun || mCommandBuffer == nullptr || mActiveTarget == nullptr)
	{
		return false;
	}

	const uint32_t outputWidth = std::max<uint32_t>((uint32_t)mSceneViewport.width, 1u);
	const uint32_t outputHeight = std::max<uint32_t>((uint32_t)mSceneViewport.height, 1u);
	const uint32_t targetWidth = std::max<uint32_t>(mActiveTarget->width, 1u);
	const uint32_t targetHeight = std::max<uint32_t>(mActiveTarget->height, 1u);
	const bool ready = mRenderer->PreloadLevelScene(outputWidth, outputHeight, targetWidth, targetHeight);
	if (ready)
	{
		mPathTracingLevelPreloadPending = false;
	}
	return ready;
}

bool NRIRenderDevice::IsPathTracingLevelPreloadPending() const
{
	return mPathTracingLevelPreloadPending;
}

void NRIRenderDevice::CancelPathTracingLevelPreload()
{
	mPathTracingLevelPreloadPending = false;
}

bool NRIRenderDevice::ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const
{
	return !!nri_ptsanity && drawmode == DM_MAINVIEW && !portal;
}

bool NRIRenderDevice::RenderPathTracingSanityFrame()
{
	if (mCommandBuffer == nullptr || mActiveTarget == nullptr || mActiveTarget->colorAttachmentView == nullptr)
	{
		return false;
	}

	mRenderState->EndFrame();
	PrepareTargetForRendering(*mActiveTarget, true);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = mActiveTarget->colorAttachmentView;
	colorAttachment.loadOp = nri::LoadOp::CLEAR;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = mSceneClearColor[0];
	colorAttachment.clearValue.color.f.y = mSceneClearColor[1];
	colorAttachment.clearValue.color.f.z = mSceneClearColor[2];
	colorAttachment.clearValue.color.f.w = mSceneClearColor[3];

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mCore.CmdBeginRendering(*mCommandBuffer, renderingDesc);
	mCore.CmdEndRendering(*mCommandBuffer);
	mRenderState->NotifyExternalTargetWrite();
	return true;
}

IHardwareTexture* NRIRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new NRIHardwareTexture(this, numchannels);
}

IVertexBuffer* NRIRenderDevice::CreateVertexBuffer()
{
	return new NRIHardwareVertexBuffer();
}

IIndexBuffer* NRIRenderDevice::CreateIndexBuffer()
{
	return new NRIHardwareIndexBuffer();
}

IDataBuffer* NRIRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new NRIHardwareDataBuffer(bindingpoint, ssbo, needsresize);
}

FTexture* NRIRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
	return tex;
}

FTexture* NRIRenderDevice::WipeEndScreen()
{
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");
	return tex;
}

TArray<uint8_t> NRIRenderDevice::GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma)
{
	const int w = SCREENWIDTH;
	const int h = SCREENHEIGHT;

	TArray<uint8_t> buffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, buffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return buffer;
}

void NRIRenderDevice::RefreshNativeFrameGenerationHandles()
{
	mNativeD3D12Device = nullptr;
	mNativeD3D12GraphicsQueue = nullptr;

	if (GetSelectedAPI() != nri::GraphicsAPI::D3D12)
	{
		return;
	}

	if (mDevice != nullptr && mCore.GetDeviceNativeObject != nullptr)
	{
		mNativeD3D12Device = static_cast<ID3D12Device*>(mCore.GetDeviceNativeObject(mDevice));
	}

	if (mGraphicsQueue != nullptr && mCore.GetQueueNativeObject != nullptr)
	{
		mNativeD3D12GraphicsQueue = static_cast<ID3D12CommandQueue*>(mCore.GetQueueNativeObject(mGraphicsQueue));
	}
}

void NRIRenderDevice::RefreshNativeFrameGenerationSwapChain()
{
	mNativeD3D12SwapChain = nullptr;
}

bool NRIRenderDevice::RefreshFrameGenerationPresentTargets()
{
#ifndef _WIN32
	return false;
#else
	DestroyFrameGenerationPresentTargets();
	mFrameGenerationPresentAllowsTearing = false;

	if (mDevice == nullptr || mWrapperD3D12.CreateTextureD3D12 == nullptr || !mFrameGeneration.IsPresentBridgeActive())
	{
		return false;
	}

	IDXGISwapChain4* swapChain = mFrameGeneration.GetPresentSwapChain();
	if (swapChain == nullptr)
	{
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	if (FAILED(swapChain->GetDesc1(&swapChainDesc)))
	{
		return false;
	}

	mFrameGenerationPresentAllowsTearing = (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
	mFrameGenerationPresentImages.resize(swapChainDesc.BufferCount);
	for (UINT i = 0; i < swapChainDesc.BufferCount; ++i)
	{
		ID3D12Resource* nativeResource = nullptr;
		if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&nativeResource))) || nativeResource == nullptr)
		{
			DestroyFrameGenerationPresentTargets();
			return false;
		}

		nri::TextureD3D12Desc textureDesc = {};
		textureDesc.d3d12Resource = nativeResource;

		auto& target = mFrameGenerationPresentImages[i];
		if (mWrapperD3D12.CreateTextureD3D12(*mDevice, textureDesc, target.texture) != nri::Result::SUCCESS)
		{
			nativeResource->Release();
			DestroyFrameGenerationPresentTargets();
			return false;
		}
		nativeResource->Release();

		target.owned = true;
		const nri::TextureDesc& wrappedDesc = mCore.GetTextureDesc(*target.texture);
		target.width = wrappedDesc.width;
		target.height = wrappedDesc.height;
		target.layerNum = wrappedDesc.layerNum;
		target.format = wrappedDesc.format;
		target.usage = wrappedDesc.usage;
		target.type = wrappedDesc.type;
		target.shaderViewType = nri::TextureView::TEXTURE;
		target.state = {};

		if (!CreateTextureViews(target))
		{
			DestroyFrameGenerationPresentTargets();
			return false;
		}
	}

	return true;
#endif
}

void NRIRenderDevice::DestroyFrameGenerationPresentTargets()
{
	for (auto& target : mFrameGenerationPresentImages)
	{
		DestroyTextureResource(target);
	}

	mFrameGenerationPresentImages.clear();
	mFrameGenerationPresentAllowsTearing = false;
}

void NRIRenderDevice::PrintPathTracingCaps() const
{
	if (mDevice == nullptr)
	{
		Printf("NRI PT capabilities are unavailable because the device is not initialized.\n");
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	Printf("NRI PT caps: api=%s shader_model=%u.%u ray_tracing_tier=%u texture2D_max=%u root_constants=%u root_descriptors=%u descriptor_sets=%u\n",
		(const char*)nri_api,
		deviceDesc.shaderModel / 10,
		deviceDesc.shaderModel % 10,
		deviceDesc.tiers.rayTracing,
		deviceDesc.dimensions.texture2DMaxDim,
		deviceDesc.pipelineLayout.rootConstantMaxSize,
		deviceDesc.pipelineLayout.rootDescriptorMaxNum,
		deviceDesc.pipelineLayout.descriptorSetMaxNum);
	Printf("NRI PT upscalers: NIS=%s DLSS-SR=%s DLRR=%s portal_depth=%d\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no",
		(int)nri_ptportaldepth);
	const auto& frameGenPolicy = mFrameGeneration.GetPolicy();
	Printf("NRI PT framegen caps: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen native: device=%s queue=%s swapchain=%s path=%s\n",
		mNativeD3D12Device != nullptr ? "ok" : "missing",
		mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
		mNativeD3D12SwapChain != nullptr ? "ok" : "missing",
		GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "nri-public-device-queue-only" : "unsupported-api");
	const auto& frameGenProvider = mFrameGeneration.GetProviderState();
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		GetNriResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	const auto& lowLatencyState = mFrameGeneration.GetLowLatencyState();
	Printf("NRI PT low-latency: iface=%s swapchain=%s configured=%s sleep=%s count=%llu markers=%llu present=%s set_mode=%s sleep_result=%s sim=%s/%s submit=%s/%s report=%s present_us=%llu..%llu\n",
		lowLatencyState.interfaceAvailable ? "yes" : "no",
		lowLatencyState.swapChainEnabled ? "yes" : "no",
		lowLatencyState.sleepModeConfigured ? "yes" : "no",
		lowLatencyState.sleepInvoked ? "yes" : "no",
		(unsigned long long)lowLatencyState.latencySleepCount,
		(unsigned long long)lowLatencyState.markerCount,
		lowLatencyState.presentBoundarySeen ? "yes" : "no",
		GetNriResultName(lowLatencyState.setSleepModeResult),
		GetNriResultName(lowLatencyState.latencySleepResult),
		GetNriResultName(lowLatencyState.simulationStartMarkerResult),
		GetNriResultName(lowLatencyState.simulationEndMarkerResult),
		GetNriResultName(lowLatencyState.renderSubmitStartMarkerResult),
		GetNriResultName(lowLatencyState.renderSubmitEndMarkerResult),
		GetNriResultName(lowLatencyState.latencyReportResult),
		(unsigned long long)lowLatencyState.latencyReport.presentStartTimeUs,
		(unsigned long long)lowLatencyState.latencyReport.presentEndTimeUs);

	if (mRenderer != nullptr)
	{
		Printf("NRI PT availability: %s", mRenderer->IsPathTracingSupported() ? "available" : "raster-fallback");
		if (!mRenderer->IsPathTracingSupported())
		{
			Printf(" (%s)", mRenderer->GetAvailabilityReason());
		}
		Printf("\n");
	}
}

void NRIRenderDevice::PrintPathTracingStatus() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	PrintFrameShellStatus();
	Print2DTextureStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintStatus();
	}
}

void NRIRenderDevice::PrintPathTracingBuffers() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	PrintFrameShellStatus();
	Print2DTextureStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintSceneBufferStatus();
	}
}

void NRIRenderDevice::PrintPathTracingSurfaceProbeStatus() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT surface probe is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSurfaceProbeStatus();
}

void NRIRenderDevice::EmitPathTracingWeaponLightEvent(const PathTracingWeaponLightEvent& event)
{
	if (event.eventId.IsEmpty())
	{
		return;
	}

	PathTracingWeaponLightEvent queuedEvent = event;
	queuedEvent.serial = mNextPathTracingWeaponLightEventSerial++;
	mPendingPathTracingWeaponLightEvents.Push(std::move(queuedEvent));
	mPathTracingWeaponLightEventsEnqueuedThisFrame++;
}

void NRIRenderDevice::ConsumePathTracingWeaponLightEvents(TArray<PathTracingWeaponLightEvent>& outEvents)
{
	outEvents.Clear();
	outEvents.Swap(mPendingPathTracingWeaponLightEvents);
}

uint32_t NRIRenderDevice::GetPendingPathTracingWeaponLightEventCount() const
{
	return (uint32_t)mPendingPathTracingWeaponLightEvents.Size();
}

void NRIRenderDevice::PrintPathTracingMapChunkDump(int32_t chunkIndex) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT chunk dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintMapChunkDump(chunkIndex);
}

void NRIRenderDevice::PrintPathTracingMapChunkCompare(int32_t chunkIndex) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT chunk compare is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintMapChunkCompare(chunkIndex);
}

void NRIRenderDevice::PrintFrameBoundaryStatus() const
{
	const auto& stats = mLastFrameBoundaryStats;
	Printf("NRI PT frame boundary: frame=%llu frame_index=%llu qframe=%u sanity_mode=%s last_frame=%s wait=%2.3f wait_present=%2.3f acquire=%2.3f submit=%2.3f present=%2.3f wait_present_result=%s acquire_result=%s present_result=%s image=%u sem_index=%u submit_fence=%llu\n",
		(unsigned long long)stats.frameNumber,
		(unsigned long long)stats.frameIndex,
		stats.queuedFrameIndex,
		stats.sanityModeEnabled ? "on" : "off",
		stats.sanityFrameUsed ? "clear-only" : "normal",
		stats.waitMs,
		stats.waitForPresentMs,
		stats.acquireMs,
		stats.submitMs,
		stats.presentMs,
		GetNriResultName(stats.waitForPresentResult),
		GetNriResultName(stats.acquireResult),
		GetNriResultName(stats.presentResult),
		stats.swapChainImageIndex,
		stats.acquireSemaphoreIndex,
		(unsigned long long)stats.submittedFenceValue);
}

void NRIRenderDevice::PrintFrameSequenceStatus() const
{
	bool anyValid = false;
	FString line = "NRI PT frame sequence:";
	for (uint32_t i = 0; i < FrameSequenceHistorySize; ++i)
	{
		const uint32_t index = (mFrameSequenceWriteIndex + i) % FrameSequenceHistorySize;
		const FrameSequenceEntry& entry = mFrameSequenceHistory[index];
		if (!entry.valid)
		{
			continue;
		}

		anyValid = true;
		line.AppendFormat(" [f%llu i%llu q%u a%u@s%u -> p%u@s%u fence=%llu pres=%s %s]",
			(unsigned long long)entry.frameNumber,
			(unsigned long long)entry.frameIndex,
			entry.queuedFrameIndex,
			entry.acquiredImageIndex,
			entry.acquireSemaphoreIndex,
			entry.presentedImageIndex,
			entry.releaseSemaphoreIndex,
			(unsigned long long)entry.submittedFenceValue,
			GetNriResultName(entry.presentResult),
			entry.sanityFrameUsed ? "sanity" : "normal");
	}

	if (!anyValid)
	{
		line << " none";
	}

	Printf("%s\n", line.GetChars());
}

void NRIRenderDevice::PrintSwapChainStatus() const
{
	const FString flagText = DescribeSwapChainFlags(mSwapChainFlags);
	const FString acquiredImages = DescribeSwapChainImageMask(mObservedSwapChainAcquireMask, mSwapChainTextureCount);
	const FString presentedImages = DescribeSwapChainImageMask(mObservedSwapChainPresentMask, mSwapChainTextureCount);
	const FString acquireCounts = DescribeSwapChainImageCounts(mSwapChainAcquireCounts);
	const FString presentCounts = DescribeSwapChainImageCounts(mSwapChainPresentCounts);
	const FString abandonCounts = DescribeSwapChainImageCounts(mSwapChainAbandonCounts);
	Printf("NRI PT swapchain: textures=%u queued_frames=%u vsync=%s flags=%s texture_override=%d flag_override=%s wait_present=%s acquire_seen=%u/%u [%s] present_seen=%u/%u [%s]\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		flagText.GetChars(),
		(int)nri_ptswaptextures,
		DescribeSwapChainFlagOverride(),
		nri_ptwaitpresent ? "on" : "off",
		CountSetBits(mObservedSwapChainAcquireMask),
		(uint32_t)mSwapChainTextureCount,
		acquiredImages.GetChars(),
		CountSetBits(mObservedSwapChainPresentMask),
		(uint32_t)mSwapChainTextureCount,
		presentedImages.GetChars());
	Printf("NRI PT swapchain counts: acquire=[%s] present=[%s] abandoned=[%s]\n",
		acquireCounts.GetChars(),
		presentCounts.GetChars(),
		abandonCounts.GetChars());
}

const char* NRIRenderDevice::DescribeTextureTarget(const NRITextureResource* target) const
{
	if (target == nullptr)
	{
		return "null";
	}

	if (target == mCurrentPresentTarget)
	{
		return "present";
	}

	if (target == &mSceneTarget)
	{
		return "scene";
	}

	if (target == &mSaveTarget)
	{
		return "save";
	}

	return "other";
}

void NRIRenderDevice::PrintFrameShellStatus() const
{
	const auto& stats = mLastFrameBoundaryStats;
	const NRITextureResource* activeTarget = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	Printf("NRI PT frame shell: active=%s present=%s frame_begun=%s cmd_open=%s scene_selected=%s pt_rendered=%s postprocess=%s scene_copy=%s active_state=(a=%u l=%u s=0x%x) present_state=(a=%u l=%u s=0x%x) scene_state=(a=%u l=%u s=0x%x)\n",
		DescribeTextureTarget(activeTarget),
		DescribeTextureTarget(mCurrentPresentTarget),
		mFrameBegun ? "yes" : "no",
		mCommandBufferOpen ? "yes" : "no",
		stats.sceneTargetSelected ? "yes" : "no",
		stats.pathTracedSceneRendered ? "yes" : "no",
		stats.postProcessInvoked ? "yes" : "no",
		stats.sceneCopiedToPresent ? "yes" : "no",
		activeTarget != nullptr ? (uint32_t)activeTarget->state.access : 0u,
		activeTarget != nullptr ? (uint32_t)activeTarget->state.layout : 0u,
		activeTarget != nullptr ? (uint32_t)activeTarget->state.stages : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.access : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.layout : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.stages : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.access : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.layout : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.stages : 0u);
}

void NRIRenderDevice::RecordFrameSequence(uint32_t releaseSemaphoreIndex, uint64_t submittedFenceValue, nri::Result presentResult)
{
	FrameSequenceEntry& entry = mFrameSequenceHistory[mFrameSequenceWriteIndex];
	entry = {};
	entry.frameNumber = mLastFrameBoundaryStats.frameNumber;
	entry.frameIndex = mLastFrameBoundaryStats.frameIndex;
	entry.submittedFenceValue = submittedFenceValue;
	entry.queuedFrameIndex = mLastFrameBoundaryStats.queuedFrameIndex;
	entry.acquiredImageIndex = mLastFrameBoundaryStats.swapChainImageIndex;
	entry.acquireSemaphoreIndex = mLastFrameBoundaryStats.acquireSemaphoreIndex;
	entry.presentedImageIndex = mCurrentSwapChainImage;
	entry.releaseSemaphoreIndex = releaseSemaphoreIndex;
	entry.presentResult = presentResult;
	entry.sanityFrameUsed = mLastFrameBoundaryStats.sanityFrameUsed;
	entry.valid = true;
	mFrameSequenceWriteIndex = (mFrameSequenceWriteIndex + 1) % FrameSequenceHistorySize;
}

void NRIRenderDevice::Print2DTextureStatus() const
{
	const auto& stats = mTexture2DDebugStats;
	Printf("NRI 2D textures: frame=%llu ensure=%u canvas=%u hits=%u misses=%u uploads=%u failures=%u create=%u recreate=%u bytes=%llu total_bytes=%llu\n",
		(unsigned long long)stats.frameNumber,
		stats.ensureCalls,
		stats.canvasEnsures,
		stats.cacheHits,
		stats.cacheMisses,
		stats.uploadAttempts,
		stats.uploadFailures,
		stats.resourceCreates,
		stats.resourceRecreates,
		(unsigned long long)stats.uploadedBytes,
		(unsigned long long)stats.totalUploadedBytes);
	Printf("NRI 2D totals: ensures=%llu canvas=%llu hits=%llu misses=%llu uploads=%llu failures=%llu create=%llu recreate=%llu\n",
		(unsigned long long)stats.totalEnsureCalls,
		(unsigned long long)stats.totalCanvasEnsures,
		(unsigned long long)stats.totalCacheHits,
		(unsigned long long)stats.totalCacheMisses,
		(unsigned long long)stats.totalUploadAttempts,
		(unsigned long long)stats.totalUploadFailures,
		(unsigned long long)stats.totalResourceCreates,
		(unsigned long long)stats.totalResourceRecreates);
}

void NRIRenderDevice::Reset2DTextureFrameStats()
{
	mTexture2DDebugStats.frameNumber++;
	mTexture2DDebugStats.ensureCalls = 0;
	mTexture2DDebugStats.canvasEnsures = 0;
	mTexture2DDebugStats.cacheHits = 0;
	mTexture2DDebugStats.cacheMisses = 0;
	mTexture2DDebugStats.uploadAttempts = 0;
	mTexture2DDebugStats.uploadFailures = 0;
	mTexture2DDebugStats.resourceCreates = 0;
	mTexture2DDebugStats.resourceRecreates = 0;
	mTexture2DDebugStats.uploadedBytes = 0;
}

void NRIRenderDevice::Note2DTextureEnsure(bool canvas)
{
	mTexture2DDebugStats.ensureCalls++;
	mTexture2DDebugStats.totalEnsureCalls++;
	if (canvas)
	{
		mTexture2DDebugStats.canvasEnsures++;
		mTexture2DDebugStats.totalCanvasEnsures++;
	}
}

void NRIRenderDevice::Note2DTextureCacheHit()
{
	mTexture2DDebugStats.cacheHits++;
	mTexture2DDebugStats.totalCacheHits++;
}

void NRIRenderDevice::Note2DTextureCacheMiss()
{
	mTexture2DDebugStats.cacheMisses++;
	mTexture2DDebugStats.totalCacheMisses++;
}

void NRIRenderDevice::Note2DTextureUploadAttempt(uint64_t bytes, bool success)
{
	mTexture2DDebugStats.uploadAttempts++;
	mTexture2DDebugStats.totalUploadAttempts++;
	mTexture2DDebugStats.uploadedBytes += bytes;
	mTexture2DDebugStats.totalUploadedBytes += bytes;
	if (!success)
	{
		mTexture2DDebugStats.uploadFailures++;
		mTexture2DDebugStats.totalUploadFailures++;
	}
}

void NRIRenderDevice::Note2DTextureResourceCreate(bool recreated)
{
	if (recreated)
	{
		mTexture2DDebugStats.resourceRecreates++;
		mTexture2DDebugStats.totalResourceRecreates++;
	}
	else
	{
		mTexture2DDebugStats.resourceCreates++;
		mTexture2DDebugStats.totalResourceCreates++;
	}
}

void NRIRenderDevice::NoteSwapChainAcquire(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainAcquireMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainAcquireCounts.size())
	{
		mSwapChainAcquireCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainPresent(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainPresentMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainPresentCounts.size())
	{
		mSwapChainPresentCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainAbandon(uint32_t imageIndex)
{
	if (imageIndex < mSwapChainAbandonCounts.size())
	{
		mSwapChainAbandonCounts[imageIndex]++;
	}
}

void NRIRenderDevice::ResetPathTracingHistory()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT history reset is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ResetHistory();
	Printf("NRI PT history reset requested.\n");
}

bool NRIRenderDevice::SpawnPathTracingPointLight(float red, float green, float blue, float intensity, float radius, float offset, uint32_t& outId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability())
	{
		Printf("NRI PT test lights are unavailable because path tracing is not active (%s).\n", mRenderer->GetAvailabilityReason());
		return false;
	}

	if (netgame)
	{
		Printf("nri_ptlightspawn cannot be used in multiplayer.\n");
		return false;
	}

	if (gamestate != GS_LEVEL)
	{
		Printf("nri_ptlightspawn: must be in a level.\n");
		return false;
	}

	DCorePlayer* player = PlayerArray[myconnectindex];
	if (player == nullptr)
	{
		Printf("nri_ptlightspawn: no local player is available.\n");
		return false;
	}

	DCoreActor* actor = player->GetActor();
	if (actor == nullptr)
	{
		Printf("nri_ptlightspawn: local player actor is unavailable.\n");
		return false;
	}

	if (intensity <= 0.0f)
	{
		Printf("nri_ptlightspawn: intensity must be > 0.\n");
		return false;
	}

	if (radius <= 0.0f)
	{
		Printf("nri_ptlightspawn: radius must be > 0.\n");
		return false;
	}

	if (offset < 0.0f)
	{
		Printf("nri_ptlightspawn: offset must be >= 0.\n");
		return false;
	}

	const DRotator viewRotation(
		player->getPitchWithView(),
		actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
		actor->spr.Angles.Roll + player->ViewAngles.Roll);
	const DVector3 forward(viewRotation);
	const DVector3 spawnPosition = actor->getPosWithOffsetZ() + forward * offset;
	float renderPosition[3] = {};
	WorldToPathTracingPosition(spawnPosition, renderPosition);
	const float lightColor[3] = {
		red < 0.0f ? 0.0f : red,
		green < 0.0f ? 0.0f : green,
		blue < 0.0f ? 0.0f : blue,
	};
	if (!mRenderer->AddRuntimePointLight(renderPosition, lightColor, intensity, radius, outId))
	{
		Printf("nri_ptlightspawn: failed to add PT test light. active=%u limit=64\n", mRenderer->GetRuntimePointLightCount());
		return false;
	}

	Printf("NRI PT test light spawned: id=%u world_pos=(%.3f, %.3f, %.3f) render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f offset=%.3f\n",
		outId,
		spawnPosition.X,
		spawnPosition.Y,
		spawnPosition.Z,
		renderPosition[0],
		renderPosition[1],
		renderPosition[2],
		lightColor[0],
		lightColor[1],
		lightColor[2],
		intensity,
		radius,
		offset);
	return true;
}

bool NRIRenderDevice::RemovePathTracingPointLight(uint32_t id)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RemoveRuntimePointLight(id))
	{
		Printf("nri_ptlightremove: no PT test light with id=%u.\n", id);
		return false;
	}

	Printf("NRI PT test light removed: id=%u remaining=%u\n", id, mRenderer->GetRuntimePointLightCount());
	return true;
}

void NRIRenderDevice::ClearPathTracingPointLights()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return;
	}

	const uint32_t clearedCount = mRenderer->GetRuntimePointLightCount();
	mRenderer->ClearRuntimePointLights();
	Printf("NRI PT test lights cleared: count=%u\n", clearedCount);
}

void NRIRenderDevice::PrintPathTracingPointLights() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimePointLights();
}

bool NRIRenderDevice::SpawnPathTracingDebugSphere(float diameter, float distance, float metalness, float roughness, uint32_t& outId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability())
	{
		Printf("NRI PT debug spheres are unavailable because path tracing is not active (%s).\n", mRenderer->GetAvailabilityReason());
		return false;
	}

	if (netgame)
	{
		Printf("nri_ptsphere cannot be used in multiplayer.\n");
		return false;
	}

	if (gamestate != GS_LEVEL)
	{
		Printf("nri_ptsphere: must be in a level.\n");
		return false;
	}

	DCorePlayer* player = PlayerArray[myconnectindex];
	if (player == nullptr)
	{
		Printf("nri_ptsphere: no local player is available.\n");
		return false;
	}

	DCoreActor* actor = player->GetActor();
	if (actor == nullptr)
	{
		Printf("nri_ptsphere: local player actor is unavailable.\n");
		return false;
	}

	if (diameter <= 0.0f)
	{
		Printf("nri_ptsphere: diameter must be > 0.\n");
		return false;
	}

	if (distance < 0.0f)
	{
		Printf("nri_ptsphere: distance must be >= 0.\n");
		return false;
	}

	const float clampedMetalness = clamp(metalness, 0.0f, 1.0f);
	const float clampedRoughness = clamp(roughness, 0.0f, 1.0f);
	const DRotator viewRotation(
		player->getPitchWithView(),
		actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
		actor->spr.Angles.Roll + player->ViewAngles.Roll);
	const DVector3 forward(viewRotation);
	const DVector3 spawnPosition = actor->getPosWithOffsetZ() + forward * distance;
	float renderPosition[3] = {};
	WorldToPathTracingPosition(spawnPosition, renderPosition);
	if (!mRenderer->AddRuntimeDebugSphere(renderPosition, diameter, clampedMetalness, clampedRoughness, outId))
	{
		Printf("nri_ptsphere: failed to add PT debug sphere. active=%u limit=64\n", mRenderer->GetRuntimeDebugSphereCount());
		return false;
	}

	Printf("NRI PT debug sphere spawned: id=%u world_pos=(%.3f, %.3f, %.3f) render_pos=(%.3f, %.3f, %.3f) diameter=%.3f metalness=%.3f roughness=%.3f distance=%.3f\n",
		outId,
		spawnPosition.X,
		spawnPosition.Y,
		spawnPosition.Z,
		renderPosition[0],
		renderPosition[1],
		renderPosition[2],
		diameter,
		clampedMetalness,
		clampedRoughness,
		distance);
	return true;
}

bool NRIRenderDevice::RemovePathTracingDebugSphere(uint32_t id)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RemoveRuntimeDebugSphere(id))
	{
		Printf("nri_ptsphereremove: no PT debug sphere with id=%u.\n", id);
		return false;
	}

	Printf("NRI PT debug sphere removed: id=%u remaining=%u\n", id, mRenderer->GetRuntimeDebugSphereCount());
	return true;
}

void NRIRenderDevice::ClearPathTracingDebugSpheres()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return;
	}

	const uint32_t clearedCount = mRenderer->GetRuntimeDebugSphereCount();
	mRenderer->ClearRuntimeDebugSpheres();
	Printf("NRI PT debug spheres cleared: count=%u\n", clearedCount);
}

void NRIRenderDevice::PrintPathTracingDebugSpheres() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimeDebugSpheres();
}

bool NRIRenderDevice::AddPathTracingSpriteTileLightHeuristic(uint32_t textureId, float red, float green, float blue, float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (intensity <= 0.0f)
	{
		Printf("nri_ptlightheuristic_addsprite: intensity must be > 0.\n");
		return false;
	}

	if (radius <= 0.0f)
	{
		Printf("nri_ptlightheuristic_addsprite: radius must be > 0.\n");
		return false;
	}

	const float lightColor[3] = {
		red < 0.0f ? 0.0f : red,
		green < 0.0f ? 0.0f : green,
		blue < 0.0f ? 0.0f : blue,
	};
	if (!mRenderer->AddSpriteTileLightHeuristic(textureId, lightColor, intensity, radius, flickerFrames, outRuleId))
	{
		Printf("nri_ptlightheuristic_addsprite: failed to add analytic light heuristic for tile=%u.\n", textureId);
		return false;
	}

	Printf("NRI PT analytic light heuristic added: rule=%u tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
		outRuleId,
		textureId,
		lightColor[0],
		lightColor[1],
		lightColor[2],
		intensity,
		radius,
		flickerFrames);
	return true;
}

void NRIRenderDevice::ClearPathTracingLightHeuristics()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ClearSpriteTileLightHeuristics();
	Printf("NRI PT analytic light heuristics cleared.\n");
}

void NRIRenderDevice::PrintPathTracingLightHeuristics() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSpriteTileLightHeuristics();
}

void NRIRenderDevice::PrintPathTracingSceneLightDump(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT scene-light dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSceneLightDump(radius, limit);
}

void NRIRenderDevice::PrintPathTracingLightClusters() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT light-cluster debug is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimeLightClusterStatus();
}

bool NRIRenderDevice::AddPathTracingTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->AddTextureEmissiveHeuristic(textureId, emissiveMode, intensityScale, emissiveColor, hasExplicitColor, outRuleId))
	{
		Printf("NRI PT emissive heuristic add failed: tile=%u mode=%u intensity_scale=%.3f\n", textureId, emissiveMode, intensityScale);
		return false;
	}

	Printf("NRI PT emissive heuristic %u added: tile=%u mode=%u intensity_scale=%.3f explicit_color=%s\n",
		outRuleId,
		textureId,
		emissiveMode,
		intensityScale,
		hasExplicitColor ? "yes" : "no");
	return true;
}

void NRIRenderDevice::ClearPathTracingEmissiveHeuristics()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ClearTextureEmissiveHeuristics();
	Printf("NRI PT emissive heuristics cleared.\n");
}

void NRIRenderDevice::PrintPathTracingEmissiveHeuristics() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintTextureEmissiveHeuristics();
}

void NRIRenderDevice::NotifyPathTracingGlowControlChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyGlowControlChange();
}

void NRIRenderDevice::NotifyPathTracingDebugSphereTessellationChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyDebugSphereTessellationChange();
}

void NRIRenderDevice::PrintPathTracingEmissiveSurfaces(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive-surface dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintEmissiveSurfaceDump(radius, limit);
}

void NRIRenderDevice::PrintPathTracingSectorLights(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT sector-light dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSectorLightDump(radius, limit);
}

void NRIRenderDevice::LogStartup()
{
	if (mLoggedStartup || mDevice == nullptr)
	{
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	const char* startupApi = V_GetStartupNriAPI();
	mDeviceName = FStringf("NRI (%s) - %s", startupApi, deviceDesc.adapterDesc.name);
	vendorstring = mDeviceName.GetChars();

	Printf("NRI device: " TEXTCOLOR_ORANGE "%s\n", deviceDesc.adapterDesc.name);
	Printf("NRI graphics API: %s\n", startupApi);
	Printf("Max. texture size: %u\n", deviceDesc.dimensions.texture2DMaxDim);
	Printf("Root constant limit: %u\n", deviceDesc.pipelineLayout.rootConstantMaxSize);
	Printf("Shader model: %u.%u\n", deviceDesc.shaderModel / 10, deviceDesc.shaderModel % 10);
	Printf("Ray tracing tier: %u\n", deviceDesc.tiers.rayTracing);
	Printf("NRI queued frames: %u\n", (uint32_t)mQueuedFrames.size());
	Printf("NRI swapchain policy: textures=%u queued_frames=%u vsync=%s flags=%s\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		DescribeSwapChainFlags(mSwapChainFlags).GetChars());
	Printf("Upscaler support: NIS=%s DLSS-SR=%s DLRR=%s\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no");
	const auto& frameGenPolicy = mFrameGeneration.GetPolicy();
	Printf("Frame generation policy: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		frameGenPolicy.resolvedReason);

	mLoggedStartup = true;
}

bool NRIRenderDevice::LoadNRI()
{
	if (mNriModule != nullptr)
	{
		return true;
	}

	HMODULE module = LoadLibraryA("NRI.dll");
	if (module == nullptr)
	{
		FString localPath = progdir;
		localPath << "NRI.dll";
		module = LoadLibraryA(localPath.GetChars());
	}

	if (module == nullptr)
	{
		Printf(TEXTCOLOR_RED "Failed to load NRI.dll.\n");
		return false;
	}

	mEnumerateAdapters = (PFN_nriEnumerateAdapters)GetProcAddress(module, "nriEnumerateAdapters");
	mCreateDeviceFn = (PFN_nriCreateDevice)GetProcAddress(module, "nriCreateDevice");
	mDestroyDeviceFn = (PFN_nriDestroyDevice)GetProcAddress(module, "nriDestroyDevice");
	mGetInterfaceFn = (PFN_nriGetInterface)GetProcAddress(module, "nriGetInterface");

	if (mEnumerateAdapters == nullptr || mCreateDeviceFn == nullptr || mDestroyDeviceFn == nullptr || mGetInterfaceFn == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI.dll is missing required exports.\n");
		FreeLibrary(module);
		return false;
	}

	gNriDestroyDeviceForwarder = mDestroyDeviceFn;
	gNriGetInterfaceForwarder = mGetInterfaceFn;
	mNriModule = module;
	return true;
}

bool NRIRenderDevice::CreateDevice()
{
	mLoggedD3D12FailureDred = false;
	const nri::GraphicsAPI selectedApi = GetSelectedAPI();
	const bool enableGraphicsApiValidation = nri_apivalidation && selectedApi == nri::GraphicsAPI::D3D12;
	if (nri_apivalidation && selectedApi == nri::GraphicsAPI::VK)
	{
		Printf("NRI Vulkan graphics API validation is temporarily disabled; continuing with NRI validation only.\n");
	}

	if (selectedApi == nri::GraphicsAPI::D3D12)
	{
		ConfigureD3D12DebugLayer();
		ConfigureD3D12Dred();
	}

	nri::AdapterDesc adapters[8] = {};
	uint32_t adapterCount = (uint32_t)std::size(adapters);
	const nri::Result enumerateResult = mEnumerateAdapters(adapters, adapterCount);
	if (enumerateResult != nri::Result::SUCCESS || adapterCount == 0)
	{
		Printf(TEXTCOLOR_RED "Failed to enumerate NRI adapters (result=%s, count=%u).\n", GetNriResultName(enumerateResult), adapterCount);
		return false;
	}

	for (uint32_t i = 0; i < adapterCount; ++i)
	{
		const auto& adapter = adapters[i];
		const double videoMemoryGiB = (double)adapter.videoMemorySize / (1024.0 * 1024.0 * 1024.0);
		const double sharedMemoryGiB = (double)adapter.sharedSystemMemorySize / (1024.0 * 1024.0 * 1024.0);
		Printf("NRI adapter[%u]: %s (vendor=%s, video=%.2f GiB, shared=%.2f GiB, graphicsQueues=%u)\n",
			i,
			adapter.name,
			GetNriVendorName(adapter.vendor),
			videoMemoryGiB,
			sharedMemoryGiB,
			adapter.queueNum[(uint32_t)nri::QueueType::GRAPHICS]);
	}

	nri::DeviceCreationDesc creationDesc = {};
	creationDesc.graphicsAPI = selectedApi;
	creationDesc.adapterDesc = &adapters[0];
	creationDesc.callbackInterface.MessageCallback = &NriMessageCallback;
	creationDesc.enableGraphicsAPIValidation = enableGraphicsApiValidation;
	creationDesc.enableNRIValidation = !!nri_validation;
	creationDesc.disableVKRayTracing = false;
	creationDesc.disableD3D12EnhancedBarriers = false;
	creationDesc.vkBindingOffsets = {};
	const char* startupApi = V_GetStartupNriAPI();
	Printf("NRI CreateDevice config: api=%s nri_validation=%s api_validation=%s dred=%s\n",
		startupApi,
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off");

	const nri::Result createResult = mCreateDeviceFn(creationDesc, mDevice);
	if (createResult != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI device for API '%s' using adapter '%s' (result=%s).\n",
			startupApi,
			adapters[0].name,
			GetNriResultName(createResult));
		if (createResult == nri::Result::INVALID_SDK)
		{
			Printf(TEXTCOLOR_RED "NRI reported INVALID_SDK. Check that raze.exe exports D3D12SDKVersion/D3D12SDKPath and that an AgilitySDK runtime directory is staged beside the executable.\n");
		}
		return false;
	}

	if (mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::CoreInterface), &mCore) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::HelperInterface), &mHelper) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::RayTracingInterface), &mRayTracing) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::StreamerInterface), &mStreamer) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::SwapChainInterface), &mSwapChainInterface) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::UpscalerInterface), &mUpscaler) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to retrieve NRI interfaces.\n");
		return false;
	}
	if (selectedApi == nri::GraphicsAPI::D3D12)
	{
		ConfigureD3D12InfoQueue(mCore, mDevice);
		const nri::Result wrapperResult = mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::WrapperD3D12Interface), &mWrapperD3D12);
		if (wrapperResult != nri::Result::SUCCESS)
		{
			mWrapperD3D12 = {};
		}
	}

	mLowLatency = {};
	const nri::Result lowLatencyResult = mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::LowLatencyInterface), &mLowLatency);
	if (lowLatencyResult != nri::Result::SUCCESS)
	{
		mLowLatency = {};
	}
	Printf("NRI low-latency interface: requested=%s result=%s available=%s\n",
		selectedApi == nri::GraphicsAPI::D3D12 ? "yes" : "no",
		GetNriResultName(lowLatencyResult),
		mLowLatency.SetLatencySleepMode != nullptr ? "yes" : "no");

	if (mCore.GetQueue(*mDevice, nri::QueueType::GRAPHICS, 0, mGraphicsQueue) != nri::Result::SUCCESS ||
		mCore.CreateFence(*mDevice, 0, mFrameFence) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queue objects.\n");
		return false;
	}
	SetNriDebugName(mCore, mGraphicsQueue, "Raze.GraphicsQueue");
	SetNriDebugName(mCore, mFrameFence, "Raze.FrameFence");
	RefreshNativeFrameGenerationHandles();
	Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
		startupApi,
		mNativeD3D12Device != nullptr ? "ok" : "missing",
		mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
		"pending");

	if (!CreateQueuedFrames())
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queued frame resources.\n");
		return false;
	}

	nri::StreamerDesc streamerDesc = {};
	streamerDesc.constantBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.constantBufferSize = 1024 * 1024;
	streamerDesc.dynamicBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.dynamicBufferDesc = {};
	streamerDesc.dynamicBufferDesc.usage = NRIFlags(nri::BufferUsageBits::VERTEX_BUFFER, nri::BufferUsageBits::INDEX_BUFFER);
	streamerDesc.queuedFrameNum = QueuedFrameCount;
	if (mStreamer.CreateStreamer(*mDevice, streamerDesc, mStreamerInstance) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI streamer.\n");
		return false;
	}
	SetNriDebugName(mCore, mStreamerInstance, "Raze.Streamer");

	return true;
}

void NRIRenderDevice::LogD3D12FailureDiagnostics(const char* context)
{
	if (GetSelectedAPI() != nri::GraphicsAPI::D3D12)
	{
		return;
	}

	LogD3D12DeviceRemovedReason(mCore, mDevice, context);
	LogD3D12InfoQueueMessages(mCore, mDevice, context);

	if (mLoggedD3D12FailureDred || !nri_dred)
	{
		return;
	}

	mLoggedD3D12FailureDred = true;

	if (mDevice == nullptr || mCore.GetDeviceNativeObject == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: native D3D12 device is unavailable.\n",
			context != nullptr ? context : "unknown");
		return;
	}

	auto* d3d12Device = static_cast<ID3D12Device*>(mCore.GetDeviceNativeObject(mDevice));
	if (d3d12Device == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: native D3D12 device query returned null.\n",
			context != nullptr ? context : "unknown");
		return;
	}

	ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
	if (SUCCEEDED(d3d12Device->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2 != nullptr)
	{
		const D3D12_DRED_DEVICE_STATE deviceState = dred2->GetDeviceState();
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v2, device_state=%u.\n",
			context != nullptr ? context : "unknown",
			(unsigned)deviceState);

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
		const HRESULT breadcrumbsHr = dred2->GetAutoBreadcrumbsOutput1(&breadcrumbs);
		if (SUCCEEDED(breadcrumbsHr))
		{
			if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
			{
				Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
					context != nullptr ? context : "unknown");
			}
			else
			{
				LogD3D12DredBreadcrumbNodes1(breadcrumbs.pHeadAutoBreadcrumbNode, context);
			}
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(breadcrumbsHr),
				(unsigned)breadcrumbsHr);
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
		const HRESULT pageFaultHr = dred2->GetPageFaultAllocationOutput1(&pageFault);
		if (SUCCEEDED(pageFaultHr))
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
				context != nullptr ? context : "unknown",
				(unsigned long long)pageFault.PageFaultVA);
			LogD3D12DredAllocationNodes1(pageFault.pHeadExistingAllocationNode, "existing_allocation");
			LogD3D12DredAllocationNodes1(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(pageFaultHr),
				(unsigned)pageFaultHr);
		}

		dred2->Release();
		return;
	}

	ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
	if (SUCCEEDED(d3d12Device->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1 != nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v1.\n",
			context != nullptr ? context : "unknown");

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
		const HRESULT breadcrumbsHr = dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs);
		if (SUCCEEDED(breadcrumbsHr))
		{
			if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
			{
				Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
					context != nullptr ? context : "unknown");
			}
			else
			{
				LogD3D12DredBreadcrumbNodes1(breadcrumbs.pHeadAutoBreadcrumbNode, context);
			}
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(breadcrumbsHr),
				(unsigned)breadcrumbsHr);
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
		const HRESULT pageFaultHr = dred1->GetPageFaultAllocationOutput1(&pageFault);
		if (SUCCEEDED(pageFaultHr))
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
				context != nullptr ? context : "unknown",
				(unsigned long long)pageFault.PageFaultVA);
			LogD3D12DredAllocationNodes1(pageFault.pHeadExistingAllocationNode, "existing_allocation");
			LogD3D12DredAllocationNodes1(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(pageFaultHr),
				(unsigned)pageFaultHr);
		}

		dred1->Release();
		return;
	}

	ID3D12DeviceRemovedExtendedData* dred = nullptr;
	const HRESULT dredHr = d3d12Device->QueryInterface(IID_PPV_ARGS(&dred));
	if (FAILED(dredHr) || dred == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: failed to query interfaces v2/v1/v0 (last=%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(dredHr),
			(unsigned)dredHr);
		return;
	}

	Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v0.\n",
		context != nullptr ? context : "unknown");

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
	const HRESULT breadcrumbsHr = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
	if (SUCCEEDED(breadcrumbsHr))
	{
		if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
				context != nullptr ? context : "unknown");
		}
		else
		{
			LogD3D12DredBreadcrumbNodes(breadcrumbs.pHeadAutoBreadcrumbNode, context);
		}
	}
	else
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput failed (%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(breadcrumbsHr),
			(unsigned)breadcrumbsHr);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
	const HRESULT pageFaultHr = dred->GetPageFaultAllocationOutput(&pageFault);
	if (SUCCEEDED(pageFaultHr))
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
			context != nullptr ? context : "unknown",
			(unsigned long long)pageFault.PageFaultVA);
		LogD3D12DredAllocationNodes(pageFault.pHeadExistingAllocationNode, "existing_allocation");
		LogD3D12DredAllocationNodes(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
	}
	else
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput failed (%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(pageFaultHr),
			(unsigned)pageFaultHr);
	}

	dred->Release();
}

bool NRIRenderDevice::CreateSwapChain()
{
	if (mDevice == nullptr || mGraphicsQueue == nullptr || mainwindow.GetHandle() == nullptr)
	{
		return false;
	}

	DestroySwapChain();

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);

	nri::SwapChainDesc swapChainDesc = {};
	swapChainDesc.window.windows.hwnd = mainwindow.GetHandle();
	swapChainDesc.queue = mGraphicsQueue;
	swapChainDesc.width = width;
	swapChainDesc.height = height;
	swapChainDesc.textureNum = GetRequestedSwapChainTextureCount();
	swapChainDesc.format = nri::SwapChainFormat::BT709_G22_8BIT;
	swapChainDesc.flags = GetEffectiveRequestedSwapChainFlags();
	swapChainDesc.queuedFrameNum = QueuedFrameCount;

	const bool tryFrameGenPresentBridge =
		nri_framegen &&
		HasRequestedFrameGenerationProvider() &&
		GetSelectedAPI() == nri::GraphicsAPI::D3D12 &&
		!IsFullscreenModeActive() &&
		!mFrameGeneration.ConsumeNativeFallbackRequest();

	mSwapChainFlags = swapChainDesc.flags;
	mSwapChainQueuedFrameNum = swapChainDesc.queuedFrameNum;
	mSwapChainTextureCount = 0;
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.clear();
	mSwapChainPresentCounts.clear();
	mSwapChainAbandonCounts.clear();
	mHasPresentedSwapChainFrame = false;

	if (tryFrameGenPresentBridge)
	{
		RefreshNativeFrameGenerationSwapChain();
		mFrameGeneration.OnSwapChainCreated(*this);
		if (mFrameGeneration.IsPresentBridgeActive() && RefreshFrameGenerationPresentTargets())
		{
			mSwapChainTextureCount = (uint8_t)(std::min<size_t>)(mFrameGenerationPresentImages.size(), 255u);
			mSwapChainAcquireCounts.assign(mFrameGenerationPresentImages.size(), 0);
			mSwapChainPresentCounts.assign(mFrameGenerationPresentImages.size(), 0);
			mSwapChainAbandonCounts.assign(mFrameGenerationPresentImages.size(), 0);
			Printf("NRI framegen proxy swapchain created: textures=%u queued_frames=%u vsync=%s flags=%s wait_present=%s size=%ux%u\n",
				(uint32_t)mSwapChainTextureCount,
				(uint32_t)mSwapChainQueuedFrameNum,
				vid_vsync ? "on" : "off",
				DescribeSwapChainFlags(mSwapChainFlags).GetChars(),
				nri_ptwaitpresent ? "on" : "off",
				width,
				height);
			Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
				(const char*)nri_api,
				mNativeD3D12Device != nullptr ? "ok" : "missing",
				mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
				mNativeD3D12SwapChain != nullptr ? "ok" : "missing");
			return true;
		}

		DestroyFrameGenerationPresentTargets();
		mFrameGeneration.OnSwapChainDestroyed(*this);
		Printf(TEXTCOLOR_YELLOW "NRI framegen proxy swapchain creation failed; falling back to the native NRI swapchain path.\n");
	}

	if (mSwapChainInterface.CreateSwapChain(*mDevice, swapChainDesc, mSwapChain) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI swapchain.\n");
		return false;
	}
	RefreshNativeFrameGenerationSwapChain();
	mFrameGeneration.OnSwapChainCreated(*this);
	if (mFrameGeneration.IsPresentBridgeActive() && !RefreshFrameGenerationPresentTargets())
	{
		Printf(TEXTCOLOR_RED "NRI framegen present bridge is active but proxy backbuffer wrapping failed; falling back to native present path.\n");
	}
	SetNriDebugName(mCore, mSwapChain, "Raze.SwapChain");

	uint32_t textureCount = 0;
	nri::Texture* const* textures = mSwapChainInterface.GetSwapChainTextures(*mSwapChain, textureCount);
	mSwapChainImages.resize(textureCount);
	mSwapChainQueuedFrameNum = swapChainDesc.queuedFrameNum;
	mSwapChainTextureCount = (uint8_t)(std::min<uint32_t>)(textureCount, 255u);
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.assign(textureCount, 0);
	mSwapChainPresentCounts.assign(textureCount, 0);
	mSwapChainAbandonCounts.assign(textureCount, 0);
	mHasPresentedSwapChainFrame = false;

	for (uint32_t i = 0; i < textureCount; ++i)
	{
		auto& image = mSwapChainImages[i];
		image.target.texture = textures[i];
		image.target.owned = false;
		const std::string imageName = "Raze.SwapChainImage[" + std::to_string(i) + "]";
		SetNriDebugName(mCore, image.target.texture, imageName.c_str());

		const nri::TextureDesc& desc = mCore.GetTextureDesc(*textures[i]);
		image.target.width = desc.width;
		image.target.height = desc.height;
		image.target.format = desc.format;
		image.target.usage = desc.usage;
		image.target.state = {};

		if (!CreateTextureViews(image.target))
		{
			return false;
		}

		if (mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.acquireSemaphore) != nri::Result::SUCCESS ||
			mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.releaseSemaphore) != nri::Result::SUCCESS)
		{
			return false;
		}
		const std::string acquireFenceName = "Raze.SwapChainAcquire[" + std::to_string(i) + "]";
		const std::string releaseFenceName = "Raze.SwapChainRelease[" + std::to_string(i) + "]";
		SetNriDebugName(mCore, image.acquireSemaphore, acquireFenceName.c_str());
		SetNriDebugName(mCore, image.releaseSemaphore, releaseFenceName.c_str());
	}

	Printf("NRI swapchain created: textures=%u queued_frames=%u vsync=%s flags=%s texture_override=%d flag_override=%s wait_present=%s size=%ux%u\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		DescribeSwapChainFlags(mSwapChainFlags).GetChars(),
		(int)nri_ptswaptextures,
		DescribeSwapChainFlagOverride(),
		nri_ptwaitpresent ? "on" : "off",
		width,
		height);
	Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
		(const char*)nri_api,
		mNativeD3D12Device != nullptr ? "ok" : "missing",
		mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
		mNativeD3D12SwapChain != nullptr ? "ok" : "missing");

	return true;
}

bool NRIRenderDevice::CreateQueuedFrames()
{
	DestroyQueuedFrames();

	mQueuedFrames.resize(QueuedFrameCount);
	for (size_t i = 0; i < mQueuedFrames.size(); ++i)
	{
		QueuedFrame& queuedFrame = mQueuedFrames[i];
		if (mCore.CreateCommandAllocator(*mGraphicsQueue, queuedFrame.commandAllocator) != nri::Result::SUCCESS ||
			mCore.CreateCommandBuffer(*queuedFrame.commandAllocator, queuedFrame.commandBuffer) != nri::Result::SUCCESS)
		{
			DestroyQueuedFrames();
			return false;
		}

		const std::string allocatorName = "Raze.QueuedFrameAllocator[" + std::to_string(i) + "]";
		const std::string commandBufferName = "Raze.QueuedFrameCommandBuffer[" + std::to_string(i) + "]";
		SetNriDebugName(mCore, queuedFrame.commandAllocator, allocatorName.c_str());
		SetNriDebugName(mCore, queuedFrame.commandBuffer, commandBufferName.c_str());
	}

	SelectQueuedFrame(0);
	return true;
}

void NRIRenderDevice::DestroySwapChain()
{
	DestroyFrameGenerationPresentTargets();
	RefreshNativeFrameGenerationSwapChain();
	ResetFrameTracking();
	mSwapChainFlags = nri::SwapChainBits::NONE;
	mSwapChainQueuedFrameNum = 0;
	mSwapChainTextureCount = 0;
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.clear();
	mSwapChainPresentCounts.clear();
	mSwapChainAbandonCounts.clear();
	mHasPresentedSwapChainFrame = false;
	mFrameGeneration.OnSwapChainDestroyed(*this);

	for (auto& image : mSwapChainImages)
	{
		if (image.target.colorAttachmentView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.colorAttachmentView);
			image.target.colorAttachmentView = nullptr;
		}

		if (image.target.shaderView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.shaderView);
			image.target.shaderView = nullptr;
		}

		if (image.acquireSemaphore != nullptr)
		{
			mCore.DestroyFence(image.acquireSemaphore);
			image.acquireSemaphore = nullptr;
		}

		if (image.releaseSemaphore != nullptr)
		{
			mCore.DestroyFence(image.releaseSemaphore);
			image.releaseSemaphore = nullptr;
		}
	}

	mSwapChainImages.clear();

	if (mSwapChain != nullptr)
	{
		mSwapChainInterface.DestroySwapChain(mSwapChain);
		mSwapChain = nullptr;
	}
	RefreshNativeFrameGenerationSwapChain();
}

void NRIRenderDevice::DestroyQueuedFrames()
{
	mCommandAllocator = nullptr;
	mCommandBuffer = nullptr;
	mCurrentQueuedFrameIndex = 0;

	for (QueuedFrame& queuedFrame : mQueuedFrames)
	{
		if (queuedFrame.commandBuffer != nullptr)
		{
			mCore.DestroyCommandBuffer(queuedFrame.commandBuffer);
			queuedFrame.commandBuffer = nullptr;
		}

		if (queuedFrame.commandAllocator != nullptr)
		{
			mCore.DestroyCommandAllocator(queuedFrame.commandAllocator);
			queuedFrame.commandAllocator = nullptr;
		}
	}

	mQueuedFrames.clear();
}

bool NRIRenderDevice::CreateRenderResources()
{
	if (!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.vs.dxil" : "Nri2D.vs.spirv", mVertexShaderBlob) ||
		!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.ps.dxil" : "Nri2D.ps.spirv", mPixelShaderBlob))
	{
		return false;
	}

	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = 1;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIShaderStages();

	nri::DescriptorRangeDesc textureRange = {};
	textureRange.baseRegisterIndex = 0;
	textureRange.descriptorNum = 1;
	textureRange.descriptorType = nri::DescriptorType::TEXTURE;
	textureRange.shaderStages = NRIShaderStages();
	textureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &textureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRIShaderConstants);
	rootConstant.shaderStages = NRIShaderStages();

	nri::PipelineLayoutDesc pipelineLayoutDesc = {};
	pipelineLayoutDesc.rootRegisterSpace = 2;
	pipelineLayoutDesc.rootConstants = &rootConstant;
	pipelineLayoutDesc.rootConstantNum = 1;
	pipelineLayoutDesc.descriptorSets = descriptorSets;
	pipelineLayoutDesc.descriptorSetNum = 2;
	pipelineLayoutDesc.shaderStages = NRIShaderStages();

	if (mCore.CreatePipelineLayout(*mDevice, pipelineLayoutDesc, mPipelineLayout) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI pipeline layout.\n");
		return false;
	}

	nri::DescriptorPoolDesc poolDesc = {};
	poolDesc.descriptorSetMaxNum = 4096;
	poolDesc.samplerMaxNum = 8;
	poolDesc.textureMaxNum = 4096;
	poolDesc.storageTextureMaxNum = 64;
	poolDesc.structuredBufferMaxNum = 64;
	poolDesc.accelerationStructureMaxNum = 8;

	if (mCore.CreateDescriptorPool(*mDevice, poolDesc, mDescriptorPool) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI descriptor pool.\n");
		return false;
	}

	auto createSampler = [this](NRISamplerMode mode, bool clamp, bool linear)
	{
		nri::SamplerDesc samplerDesc = {};
		samplerDesc.filters.min = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mag = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mip = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.op = nri::FilterOp::AVERAGE;
		samplerDesc.addressModes.u = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.v = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.w = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.compareOp = nri::CompareOp::NONE;

		return mCore.CreateSampler(*mDevice, samplerDesc, mSamplers[(size_t)mode]) == nri::Result::SUCCESS;
	};

	if (!createSampler(NRISamplerMode::ClampLinear, true, true) ||
		!createSampler(NRISamplerMode::WrapLinear, false, true) ||
		!createSampler(NRISamplerMode::ClampPoint, true, false) ||
		!createSampler(NRISamplerMode::WrapPoint, false, false))
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI samplers.\n");
		return false;
	}

	for (size_t i = 0; i < (size_t)NRISamplerMode::Count; ++i)
	{
		nri::DescriptorSet* set = nullptr;
		if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 0, &set, 1, 0) != nri::Result::SUCCESS)
		{
			return false;
		}

		const nri::Descriptor* samplerDescriptor = mSamplers[i];
		nri::UpdateDescriptorRangeDesc updateDesc = {};
		updateDesc.descriptorSet = set;
		updateDesc.rangeIndex = 0;
		updateDesc.descriptors = &samplerDescriptor;
		updateDesc.descriptorNum = 1;
		mCore.UpdateDescriptorRanges(&updateDesc, 1);
		mSamplerSets[i] = set;
	}

	mWhiteTexture = new NRIHardwareTexture(this, 4);
	uint32_t whitePixel = 0xffffffffu;
	mWhiteTexture->CreateTexture((unsigned char*)&whitePixel, 1, 1, 0, false, "WhiteTexture");
	mWhiteTextureSet = mWhiteTexture->GetResource().textureSet;
	return mWhiteTextureSet != nullptr;
}

void NRIRenderDevice::DestroyRenderResources()
{
	DestroyFrameGenerationPresentTargets();
	DestroyFrameGenerationUiTexture();

	delete mWhiteTexture;
	mWhiteTexture = nullptr;
	mWhiteTextureSet = nullptr;

	DestroyTextureResource(mSaveTarget);
	DestroyTextureResource(mSceneTarget);

	if (mPipelineLayout != nullptr)
	{
		mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}

	if (mDescriptorPool != nullptr)
	{
		mCore.DestroyDescriptorPool(mDescriptorPool);
		mDescriptorPool = nullptr;
	}

	for (auto& samplerSet : mSamplerSets)
	{
		samplerSet = nullptr;
	}

	for (auto& sampler : mSamplers)
	{
		if (sampler != nullptr)
		{
			mCore.DestroyDescriptor(sampler);
			sampler = nullptr;
		}
	}
}

bool NRIRenderDevice::BeginCommandList(const char* reason, bool waitForSlotReuse)
{
	if (mDescriptorPool == nullptr || mQueuedFrames.empty())
	{
		return false;
	}

	SelectQueuedFrame(mCurrentQueuedFrameIndex);
	if (mCommandAllocator == nullptr || mCommandBuffer == nullptr)
	{
		return false;
	}

	if (mCommandBufferOpen)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList blocked: command buffer already open (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
		return false;
	}

	if (waitForSlotReuse && mFrameFence != nullptr)
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			mCore.Wait(*mFrameFence, queuedFrame.lastSubmittedFenceValue);
		}
	}

	mCore.ResetCommandAllocator(*mCommandAllocator);
	const bool success = mCore.BeginCommandBuffer(*mCommandBuffer, mDescriptorPool) == nri::Result::SUCCESS;
	mCommandBufferOpen = success;
	if (!success)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList failed (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
	}
	return success;
}

bool NRIRenderDevice::EnsureSwapChainSize()
{
	if (mSwapChain == nullptr)
	{
		if (!mFrameGenerationPresentImages.empty() && IsFrameGenerationPresentPathActive())
		{
			const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
			const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);
			const nri::SwapChainBits requestedFlags = GetEffectiveRequestedSwapChainFlags();
			if (mFrameGenerationPresentImages[0].width == width &&
				mFrameGenerationPresentImages[0].height == height &&
				mSwapChainFlags == requestedFlags)
			{
				return true;
			}

			mFrameGeneration.NoteReset(
				(mFrameGenerationPresentImages[0].width != width || mFrameGenerationPresentImages[0].height != height) ?
					"swapchain-resize" :
					"swapchain-flags-change");
			WaitForCommands(true);
		}
		return CreateSwapChain();
	}

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);
	const nri::SwapChainBits requestedFlags = GetEffectiveRequestedSwapChainFlags();
	if (!mSwapChainImages.empty() &&
		mSwapChainImages[0].target.width == width &&
		mSwapChainImages[0].target.height == height &&
		mSwapChainFlags == requestedFlags)
	{
		return true;
	}

	mFrameGeneration.NoteReset(
		(!mSwapChainImages.empty() && (mSwapChainImages[0].target.width != width || mSwapChainImages[0].target.height != height)) ?
			"swapchain-resize" :
			"swapchain-flags-change");
	WaitForCommands(true);
	return CreateSwapChain();
}

void NRIRenderDevice::EndFrameAndPresent()
{
	const double presentShellStartMs = I_msTimeF();
	double frameGenEndMs = 0.0;
	double configureDispatchMs = 0.0;
	double transitionMs = 0.0;
	double endCommandMs = 0.0;
	double simulationEndMs = 0.0;
	double submitPrepMs = 0.0;
	double submitCallMs = 0.0;
	double streamerEndMs = 0.0;
	double presentCallMs = 0.0;
	double tracePrintMs = 0.0;
	double resetMs = 0.0;
	double stageStartMs = I_msTimeF();

	mFrameGeneration.EndFrame(*this);
	frameGenEndMs = I_msTimeF() - stageStartMs;

	static int sLoggedPresentCount = 0;

	if (!mFrameBegun || mCommandBuffer == nullptr || mCurrentPresentTarget == nullptr)
	{
		ResetFrameTracking();
		return;
	}

	stageStartMs = I_msTimeF();
	mFrameGeneration.ConfigureAndDispatchFrame(*this);
	configureDispatchMs = I_msTimeF() - stageStartMs;
	stageStartMs = I_msTimeF();
	TransitionTexture(*mCurrentPresentTarget, { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE });
	transitionMs = I_msTimeF() - stageStartMs;
	stageStartMs = I_msTimeF();
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;
	endCommandMs = I_msTimeF() - stageStartMs;

	const uint64_t submittedFenceValue = 1 + mFrameIndex;
	mSubmittedFenceValue = submittedFenceValue;
	mLastFrameBoundaryStats.submittedFenceValue = submittedFenceValue;
	const nri::FenceSubmitDesc frameFence = { mFrameFence, submittedFenceValue, nri::StageBits::NONE };
	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };

	stageStartMs = I_msTimeF();
	mFrameGeneration.OnSimulationEnd(*this);
	simulationEndMs = I_msTimeF() - stageStartMs;
	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	nri::FenceSubmitDesc waitFence = {};
	nri::FenceSubmitDesc signalFences[2] = {};
	if (!IsFrameGenerationPresentPathActive())
	{
		waitFence = { mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, 0, NRISwapChainAcquireWaitStages() };
		const nri::FenceSubmitDesc releaseFence = { mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore, 0, nri::StageBits::NONE };
		signalFences[0] = releaseFence;
		signalFences[1] = frameFence;
		submitDesc.waitFences = &waitFence;
		submitDesc.waitFenceNum = 1;
		submitDesc.signalFences = signalFences;
		submitDesc.signalFenceNum = 2;
		const bool lowLatencySwapChainEnabled = ((uint32_t)mSwapChainFlags & (uint32_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0;
		submitDesc.swapChain = lowLatencySwapChainEnabled ? mSwapChain : nullptr;
	}
	else
	{
		signalFences[0] = frameFence;
		submitDesc.signalFences = signalFences;
		submitDesc.signalFenceNum = 1;
		submitDesc.swapChain = nullptr;
	}
	nri::Result submitResult = nri::Result::FAILURE;
	stageStartMs = I_msTimeF();
	mFrameGeneration.OnRenderSubmitStart(*this);
	submitPrepMs = I_msTimeF() - stageStartMs;
	{
		ScopedNriTiming submitTiming(NriPTQueueSubmit, mLastFrameBoundaryStats.submitMs);
		stageStartMs = I_msTimeF();
		submitResult = mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
		submitCallMs = I_msTimeF() - stageStartMs;
	}
	mFrameGeneration.OnRenderSubmitEnd(*this);
	if (submitResult != nri::Result::SUCCESS)
	{
		if (submitResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
		}
		Printf(TEXTCOLOR_RED "NRI QueueSubmit failed with result '%s'.\n", GetNriResultName(submitResult));
		LogD3D12FailureDiagnostics("QueueSubmit");
	}

	stageStartMs = I_msTimeF();
	mStreamer.EndStreamerFrame(*mStreamerInstance);
	streamerEndMs = I_msTimeF() - stageStartMs;
	nri::Result presentResult = nri::Result::FAILURE;
	mFrameGeneration.OnPresentStart(*this);
	{
		ScopedNriTiming presentTiming(NriPTQueuePresent, mLastFrameBoundaryStats.presentMs);
		stageStartMs = I_msTimeF();
		if (IsFrameGenerationPresentPathActive())
		{
			if (!mFrameGeneration.Present(*this, !!vid_vsync, mFrameGenerationPresentAllowsTearing, presentResult))
			{
				presentResult = nri::Result::FAILURE;
			}
		}
		else
		{
			presentResult = mSwapChainInterface.QueuePresent(*mSwapChain, *mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore);
		}
		presentCallMs = I_msTimeF() - stageStartMs;
	}
	mFrameGeneration.OnPresentEnd(*this, presentResult);
	mLastFrameBoundaryStats.presentResult = presentResult;
	if (presentResult == nri::Result::SUCCESS)
	{
		if (!IsFrameGenerationPresentPathActive())
		{
			NoteSwapChainPresent(mCurrentSwapChainImage);
		}
		mHasPresentedSwapChainFrame = true;
		if (nri_ptdebug > 0 && sLoggedPresentCount < 4)
		{
			Printf("NRI present: frame_index=%llu image=%u queued_frame=%u\n",
				(unsigned long long)mFrameIndex,
				mCurrentSwapChainImage,
				mCurrentQueuedFrameIndex);
			sLoggedPresentCount++;
		}
	}
	else
	{
		mHasPresentedSwapChainFrame = false;
		if (presentResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
		}
		Printf(TEXTCOLOR_RED "NRI QueuePresent failed with result '%s'.\n", GetNriResultName(presentResult));
		LogD3D12FailureDiagnostics(IsFrameGenerationPresentPathActive() ? "FramegenPresent" : "QueuePresent");
		if (IsFrameGenerationPresentPathActive() && presentResult != nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.RequestNativeFallback("proxy-present-failed");
			WaitForCommands(true);
			if (CreateSwapChain())
			{
				Printf(TEXTCOLOR_YELLOW "NRI framegen present fallback: recreated the native swapchain path after proxy present failure.\n");
			}
			else
			{
				Printf(TEXTCOLOR_RED "NRI framegen present fallback failed to recreate the native swapchain path.\n");
			}
		}
	}
	if (mCurrentQueuedFrameIndex < mQueuedFrames.size())
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		queuedFrame.lastSubmittedFenceValue = submittedFenceValue;
		queuedFrame.lastSubmittedFrameIndex = mFrameIndex;
		queuedFrame.hasSubmittedWork = true;
	}
	RecordFrameSequence(mCurrentSwapChainImage, submittedFenceValue, presentResult);
	const bool tracedGameplayFrame = mTraceThisFrame && (mLastFrameBoundaryStats.pathTracedSceneRendered || mLastFrameBoundaryStats.postProcessInvoked);
	if (tracedGameplayFrame)
	{
		stageStartMs = I_msTimeF();
		PrintFrameBoundaryStatus();
		PrintSwapChainStatus();
		PrintFrameShellStatus();
		Print2DTextureStatus();
		tracePrintMs = I_msTimeF() - stageStartMs;
		const int remainingTraceFrames = (int)nri_pttraceframes - 1;
		nri_pttraceframes = remainingTraceFrames > 0 ? remainingTraceFrames : 0;
	}
	if (nri_ptdebug > 0 && mPathTracingWeaponLightEventsEnqueuedThisFrame > 0)
	{
		Printf("NRI PT weapon-light events: frame=%llu enqueued=%u pending=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			mPathTracingWeaponLightEventsEnqueuedThisFrame,
			(uint32_t)mPendingPathTracingWeaponLightEvents.Size());
	}
	stageStartMs = I_msTimeF();
	ResetFrameTracking(presentResult == nri::Result::SUCCESS);
	resetMs = I_msTimeF() - stageStartMs;
	mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
	if (PerfLoopTraceActive())
	{
		Printf(
			"PERF present trace NRI: frame=%llu fg_end=%.3f dispatch=%.3f transition=%.3f endcmd=%.3f sim_end=%.3f submit_prep=%.3f submit_call=%.3f streamer=%.3f present_call=%.3f trace_print=%.3f reset=%.3f total=%.3f present_ok=%d\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			frameGenEndMs,
			configureDispatchMs,
			transitionMs,
			endCommandMs,
			simulationEndMs,
			submitPrepMs,
			submitCallMs,
			streamerEndMs,
			presentCallMs,
			tracePrintMs,
			resetMs,
			I_msTimeF() - presentShellStartMs,
			presentResult == nri::Result::SUCCESS ? 1 : 0);
	}
	mFrameIndex++;
}

void NRIRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc)
{
	if (!mInitialized || tex == nullptr)
	{
		return;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	hwTex->EnsureCanvas(tex);

	NRITextureResource* previousTarget = mActiveTarget;
	mRenderState->EndFrame();
	mActiveTarget = &hwTex->GetResource();

	IntRect bounds = {};
	bounds.width = tex->GetWidth();
	bounds.height = tex->GetHeight();
	renderFunc(bounds);

	mRenderState->EndFrame();
	TransitionTexture(hwTex->GetResource(), NRIShaderResourceState());
	mActiveTarget = previousTarget;
	tex->SetUpdated(true);
}

void NRIRenderDevice::CopyScreenToBuffer(int width, int height, uint8_t* buffer)
{
	if (buffer == nullptr || width <= 0 || height <= 0)
	{
		return;
	}

	NRITextureResource* source = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!BeginCommandList("CopyScreenToBuffer", !mFrameBegun))
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	TransitionTexture(*source, NRICopySourceState());

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	const uint32_t rowPitch = AlignUp((uint32_t)width * 4u, deviceDesc.memoryAlignment.uploadBufferTextureRow);
	const uint32_t slicePitch = AlignUp(rowPitch * (uint32_t)height, deviceDesc.memoryAlignment.uploadBufferTextureSlice);

	nri::BufferDesc readbackDesc = {};
	readbackDesc.size = slicePitch;
	nri::Buffer* readbackBuffer = nullptr;
	if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, readbackBuffer) != nri::Result::SUCCESS)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	nri::TextureRegionDesc region = {};
	region.width = (uint32_t)width;
	region.height = (uint32_t)height;
	region.depth = 1;
	region.planes = nri::PlaneBits::COLOR;

	nri::TextureDataLayoutDesc layout = {};
	layout.rowPitch = rowPitch;
	layout.slicePitch = slicePitch;

	mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *readbackBuffer, layout, *source->texture, region);
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* readbackFence = nullptr;
	if (mCore.CreateFence(*mDevice, 0, readbackFence) != nri::Result::SUCCESS)
	{
		mCore.DestroyBuffer(readbackBuffer);
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = readbackFence;
	frameFence.value = 1;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*readbackFence, frameFence.value);

	const uint8_t* pixels = (const uint8_t*)mCore.MapBuffer(*readbackBuffer, 0, slicePitch);
	for (int y = 0; y < height; ++y)
	{
		const uint8_t* src = pixels + (size_t)(height - y - 1) * rowPitch;
		uint8_t* dst = buffer + (size_t)y * (size_t)width * 3u;

		for (int x = 0; x < width; ++x)
		{
			dst[x * 3 + 0] = src[x * 4 + 2];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 0];
		}
	}

	mCore.UnmapBuffer(*readbackBuffer);
	mCore.DestroyFence(readbackFence);
	mCore.DestroyBuffer(readbackBuffer);
}

void NRIRenderDevice::TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after)
{
	if (texture.texture == nullptr)
	{
		return;
	}

	if (texture.state.access == after.access && texture.state.layout == after.layout && texture.state.stages == after.stages)
	{
		return;
	}

	nri::TextureBarrierDesc barrier = {};
	barrier.texture = texture.texture;
	barrier.before = texture.state;
	barrier.after = after;
	barrier.mipNum = 1;
	barrier.layerNum = 1;
	barrier.planes = nri::PlaneBits::COLOR;

	nri::BarrierDesc barriers = {};
	barriers.textures = &barrier;
	barriers.textureNum = 1;
	mCore.CmdBarrier(*mCommandBuffer, barriers);
	texture.state = after;
}

void NRIRenderDevice::PrepareTargetForRendering(NRITextureResource& target, bool)
{
	TransitionTexture(target, NRIColorAttachmentState());
}

void NRIRenderDevice::FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after)
{
	TransitionTexture(target, after);
}

void NRIRenderDevice::DestroyTextureResource(NRITextureResource& resource)
{
	if (resource.colorAttachmentView != nullptr)
	{
		mCore.DestroyDescriptor(resource.colorAttachmentView);
		resource.colorAttachmentView = nullptr;
	}

	if (resource.storageView != nullptr)
	{
		mCore.DestroyDescriptor(resource.storageView);
		resource.storageView = nullptr;
	}

	if (resource.shaderView != nullptr)
	{
		mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}

	if (resource.owned && resource.texture != nullptr)
	{
		mCore.DestroyTexture(resource.texture);
	}

	resource.texture = nullptr;
	resource.owned = false;
	resource.width = 0;
	resource.height = 0;
	resource.layerNum = 1;
	resource.format = nri::Format::UNKNOWN;
	resource.type = nri::TextureType::TEXTURE_2D;
	resource.shaderViewType = nri::TextureView::TEXTURE;
	resource.usage = nri::TextureUsageBits::NONE;
	resource.state = {};
}

bool NRIRenderDevice::CreateTextureViews(NRITextureResource& resource)
{
	const uint32_t usage = (uint32_t)resource.usage;
	nri::TextureViewDesc shaderViewDesc = {};
	shaderViewDesc.texture = resource.texture;
	shaderViewDesc.type = resource.shaderViewType;
	shaderViewDesc.format = resource.format;
	shaderViewDesc.mipNum = 1;
	shaderViewDesc.layerNum = resource.layerNum;
	shaderViewDesc.sliceNum = 1;
	shaderViewDesc.readonlyPlanes = nri::PlaneBits::COLOR;
	shaderViewDesc.components = { nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY };

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE) != 0)
	{
		if (mCore.CreateTextureView(shaderViewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}

		if (resource.textureSet == nullptr)
		{
			resource.textureSet = CreateTextureSet(resource.shaderView);
			if (resource.textureSet == nullptr)
			{
				return false;
			}
		}
		else
		{
			const nri::Descriptor* descriptor = resource.shaderView;
			nri::UpdateDescriptorRangeDesc updateDesc = {};
			updateDesc.descriptorSet = resource.textureSet;
			updateDesc.rangeIndex = 0;
			updateDesc.descriptors = &descriptor;
			updateDesc.descriptorNum = 1;
			mCore.UpdateDescriptorRanges(&updateDesc, 1);
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0)
	{
		nri::TextureViewDesc storageViewDesc = shaderViewDesc;
		if (resource.format == nri::Format::BGRA8_SRGB)
		{
			storageViewDesc.format = nri::Format::BGRA8_UNORM;
		}
		else if (resource.format == nri::Format::RGBA8_SRGB)
		{
			// Match NRD-Sample: UAVs use the non-sRGB twin of the underlying texture format.
			storageViewDesc.format = nri::Format::RGBA8_UNORM;
		}
		storageViewDesc.type = nri::TextureView::STORAGE_TEXTURE;
		if (mCore.CreateTextureView(storageViewDesc, resource.storageView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::COLOR_ATTACHMENT) != 0)
	{
		nri::TextureViewDesc colorViewDesc = shaderViewDesc;
		colorViewDesc.type = nri::TextureView::COLOR_ATTACHMENT;
		if (mCore.CreateTextureView(colorViewDesc, resource.colorAttachmentView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	return true;
}

bool NRIRenderDevice::CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage, nri::TextureType type, uint32_t layerNum, nri::TextureView shaderViewType)
{
	nri::TextureDesc textureDesc = {};
	textureDesc.type = type;
	textureDesc.usage = usage;
	textureDesc.format = format;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.depth = 1;
	textureDesc.mipNum = 1;
	textureDesc.layerNum = layerNum;
	textureDesc.sampleNum = 1;

	if (mCore.CreateCommittedTexture(*mDevice, nri::MemoryLocation::DEVICE, 0.0f, textureDesc, resource.texture) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.width = width;
	resource.height = height;
	resource.layerNum = layerNum;
	resource.format = format;
	resource.type = type;
	resource.shaderViewType = shaderViewType;
	resource.usage = usage;
	resource.owned = true;
	resource.state = {};
	return CreateTextureViews(resource);
}

bool NRIRenderDevice::UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch)
{
	if (data == nullptr || width == 0 || height == 0 || rowPitch == 0)
	{
		return false;
	}

	const uint32_t slicePitch = rowPitch * height;
	std::vector<uint8_t> uploadCopy(slicePitch);
	memcpy(uploadCopy.data(), data, slicePitch);

	nri::TextureSubresourceUploadDesc subresource = {};
	subresource.slices = uploadCopy.data();
	subresource.sliceNum = 1;
	subresource.rowPitch = rowPitch;
	subresource.slicePitch = slicePitch;

	return UploadTextureSubresources(resource, &subresource, 1, width, height);
}

bool NRIRenderDevice::UploadTextureSubresources(NRITextureResource& resource, const nri::TextureSubresourceUploadDesc* subresources, uint32_t subresourceNum, uint32_t width, uint32_t height)
{
	if (subresources == nullptr || subresourceNum == 0 || width == 0 || height == 0)
	{
		return false;
	}

	nri::TextureUploadDesc uploadDesc = {};
	uploadDesc.subresources = subresources;
	uploadDesc.texture = resource.texture;
	uploadDesc.after = NRIShaderResourceState();
	uploadDesc.planes = nri::PlaneBits::COLOR;

	const nri::Result result = mHelper.UploadData(*mGraphicsQueue, &uploadDesc, 1, nullptr, 0);
	if (result == nri::Result::SUCCESS)
	{
		resource.state = NRIShaderResourceState();
		resource.width = width;
		resource.height = height;
		return true;
	}

	return false;
}

bool NRIRenderDevice::CopyCurrentTargetToTexture(NRITextureResource& destination)
{
	NRITextureResource* source = mFrameBegun && mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr || destination.texture == nullptr)
	{
		return false;
	}

	const bool useActiveFrameCommandBuffer = mFrameBegun && mCommandBuffer != nullptr;
	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!useActiveFrameCommandBuffer && !BeginCommandList("CopyCurrentTargetToTexture", true))
	{
		return false;
	}

	const nri::AccessLayoutStage sourceStateBeforeCopy = source->state;
	TransitionTexture(*source, NRICopySourceState());
	TransitionTexture(destination, NRICopyDestinationState());
	mCore.CmdCopyTexture(*mCommandBuffer, *destination.texture, nullptr, *source->texture, nullptr);
	TransitionTexture(*source, sourceStateBeforeCopy);
	TransitionTexture(destination, NRIShaderResourceState());

	if (useActiveFrameCommandBuffer)
	{
		return true;
	}

	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* copyFence = nullptr;
	if (mCore.CreateFence(*mDevice, 0, copyFence) != nri::Result::SUCCESS)
	{
		return false;
	}
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = copyFence;
	frameFence.value = 1;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	mCore.Wait(*copyFence, frameFence.value);
	mCore.DestroyFence(copyFence);
	return true;
}

bool NRIRenderDevice::LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob)
{
	FString shaderPath = progdir;
	shaderPath << "shaders/nri/" << fileName;

	std::ifstream file(shaderPath.GetChars(), std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		Printf(TEXTCOLOR_RED "Failed to open NRI shader '%s'.\n", shaderPath.GetChars());
		return false;
	}

	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	outBlob.resize((size_t)size);
	return file.read((char*)outBlob.data(), size).good();
}

const void* NRIRenderDevice::GetVertexShaderBytecode(size_t& size) const
{
	size = mVertexShaderBlob.size();
	return mVertexShaderBlob.empty() ? nullptr : mVertexShaderBlob.data();
}

const void* NRIRenderDevice::GetPixelShaderBytecode(size_t& size) const
{
	size = mPixelShaderBlob.size();
	return mPixelShaderBlob.empty() ? nullptr : mPixelShaderBlob.data();
}

nri::GraphicsAPI NRIRenderDevice::GetSelectedAPI() const
{
	return FString(V_GetStartupNriAPI()).CompareNoCase("d3d12") == 0 ? nri::GraphicsAPI::D3D12 : nri::GraphicsAPI::VK;
}

NRISamplerMode NRIRenderDevice::GetSamplerMode(int clampMode) const
{
	const bool point = clampMode == CLAMP_NOFILTER || clampMode == CLAMP_NOFILTER_X || clampMode == CLAMP_NOFILTER_Y || clampMode == CLAMP_NOFILTER_XY;
	const bool clamp = clampMode != CLAMP_NONE && clampMode != CLAMP_CAMTEX;

	if (point)
	{
		return clamp ? NRISamplerMode::ClampPoint : NRISamplerMode::WrapPoint;
	}

	return clamp ? NRISamplerMode::ClampLinear : NRISamplerMode::WrapLinear;
}

nri::DescriptorSet* NRIRenderDevice::GetSamplerSet(NRISamplerMode mode) const
{
	return mSamplerSets[(size_t)mode];
}

nri::DescriptorSet* NRIRenderDevice::CreateTextureSet(nri::Descriptor* shaderView)
{
	nri::DescriptorSet* set = nullptr;
	if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 1, &set, 1, 0) != nri::Result::SUCCESS)
	{
		return nullptr;
	}

	const nri::Descriptor* descriptor = shaderView;
	nri::UpdateDescriptorRangeDesc updateDesc = {};
	updateDesc.descriptorSet = set;
	updateDesc.rangeIndex = 0;
	updateDesc.descriptors = &descriptor;
	updateDesc.descriptorNum = 1;
	mCore.UpdateDescriptorRanges(&updateDesc, 1);
	return set;
}

void NRIRenderDevice::ResetFrameTracking(bool presentedAcquiredImage)
{
	if (mHasAcquiredSwapChainImage && !presentedAcquiredImage)
	{
		NoteSwapChainAbandon(mCurrentSwapChainImage);
	}

	mFrameBegun = false;
	mCommandBufferOpen = false;
	mCurrentPresentTarget = nullptr;
	mActiveTarget = nullptr;
	mFrameGenerationUiTargetActive = false;
	mHasAcquiredSwapChainImage = false;
	mCurrentSwapChainImage = 0;
}

uint32_t NRIRenderDevice::GetQueuedFrameIndex(uint64_t frameIndex) const
{
	return mQueuedFrames.empty() ? 0 : (uint32_t)(frameIndex % mQueuedFrames.size());
}

void NRIRenderDevice::SelectQueuedFrame(uint32_t queuedFrameIndex)
{
	if (mQueuedFrames.empty())
	{
		mCurrentQueuedFrameIndex = 0;
		mCommandAllocator = nullptr;
		mCommandBuffer = nullptr;
		return;
	}

	mCurrentQueuedFrameIndex = queuedFrameIndex % (uint32_t)mQueuedFrames.size();
	QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
	mCommandAllocator = queuedFrame.commandAllocator;
	mCommandBuffer = queuedFrame.commandBuffer;
}
