//
// nri_framegen_stub.cpp
//
// Frame generation is implemented on top of the AMD FidelityFX SDK, which NRI
// gates on WIN32 (see NRI_ENABLE_FFX_SDK in NRI's CMakeLists). There is no Linux
// build of that SDK, so this stub stands in for the real implementation and
// reports the feature as permanently unavailable. Every entry point is safe to
// call; the renderer already handles a provider that never becomes ready.
//

#include "nri_framegen.h"

#include "nri/system/nri_renderdevice.h"

void NRIFrameGenerationContext::Initialize(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::Shutdown() {}
void NRIFrameGenerationContext::RefreshPolicy(const NRIRenderDevice&, bool) {}
void NRIFrameGenerationContext::OnSwapChainCreated(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnSwapChainDestroyed(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::BeginFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::EndFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnSimulationEnd(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnRenderSubmitStart(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnRenderSubmitEnd(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnPresentStart(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::OnPresentEnd(const NRIRenderDevice&, nri::Result) {}
void NRIFrameGenerationContext::SetFrameDesc(const NRIRenderDevice&, const NRIFrameGenerationFrameDesc&) {}
void NRIFrameGenerationContext::SetUiTexture(const NRITextureResource*) {}
void NRIFrameGenerationContext::ConfigureAndDispatchFrame(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::NoteReset(const char*) {}
void NRIFrameGenerationContext::RequestNativeFallback(const char*) {}

bool NRIFrameGenerationContext::Present(const NRIRenderDevice&, bool, bool, nri::Result&)
{
	// Never takes over presentation; the caller falls back to the native path.
	return false;
}

bool NRIFrameGenerationContext::ConsumeNativeFallbackRequest() { return false; }
bool NRIFrameGenerationContext::IsPresentBridgeActive() const { return false; }
bool NRIFrameGenerationContext::ShouldUsePresentBridge() const { return false; }
// GetPresentSwapChain is declared only under _WIN32, so it is intentionally absent here.

const char* NRIFrameGenerationContext::GetProviderName(NRIFrameGenerationProvider) { return "off"; }
const char* NRIFrameGenerationContext::GetUiModeName(NRIFrameGenerationUiMode) { return "auto"; }
const char* NRIFrameGenerationContext::GetColorSourceName(NRIFrameGenerationColorSource) { return "unknown"; }
const char* NRIFrameGenerationContext::GetMotionVectorSpaceName(NRIFrameGenerationMotionVectorSpace) { return "unknown"; }
const char* NRIFrameGenerationContext::GetMotionVectorDirectionName(NRIFrameGenerationMotionVectorDirection) { return "unknown"; }
const char* NRIFrameGenerationContext::GetDepthTypeName(NRIFrameGenerationDepthType) { return "unknown"; }
const char* NRIFrameGenerationContext::GetAdapterRequirementName(NRIFrameGenerationAdapterRequirement) { return "unknown"; }
const char* NRIFrameGenerationContext::GetOutputContractName(NRIFrameGenerationOutputContract) { return "none"; }
const char* NRIFrameGenerationContext::GetPresentTransferFunctionName(NRIFrameGenerationPresentTransferFunction) { return "unknown"; }
const char* NRIFrameGenerationContext::GetSwapChainFormatName(nri::SwapChainFormat) { return "unknown"; }
const char* NRIFrameGenerationContext::GetNriFormatName(nri::Format) { return "unknown"; }
const char* NRIFrameGenerationContext::GetDxgiFormatName(uint32_t) { return "unknown"; }
const char* NRIFrameGenerationContext::GetWindowModeName(bool fullscreen) { return fullscreen ? "fullscreen" : "windowed"; }
const char* NRIFrameGenerationContext::GetAvailabilityName(bool available) { return available ? "yes" : "no"; }
const char* NRIFrameGenerationContext::GetProviderReturnCodeName(uint32_t) { return "unavailable"; }
const char* NRIFrameGenerationContext::GetPresentResultName(nri::Result) { return "unavailable"; }

NRIFrameGenerationSettings NRIFrameGenerationContext::CaptureSettings()
{
	return NRIFrameGenerationSettings{};
}

NRIFrameGenerationPolicy NRIFrameGenerationContext::BuildPolicy(const NRIRenderDevice&, const NRIFrameGenerationPresentContract&) const
{
	NRIFrameGenerationPolicy policy = {};
	policy.resolvedReason = "unsupported-platform";
	return policy;
}

NRIFrameGenerationPresentContract NRIFrameGenerationContext::BuildPresentContract(const NRIRenderDevice&) const
{
	NRIFrameGenerationPresentContract contract = {};
	contract.resolvedReason = "unsupported-platform";
	return contract;
}

NRIFrameGenerationInputAudit NRIFrameGenerationContext::BuildInputAudit(const NRIFrameGenerationFrameDesc&) const
{
	return NRIFrameGenerationInputAudit{};
}

bool NRIFrameGenerationContext::IsLowLatencyOperational(const NRIRenderDevice&) const { return false; }
void NRIFrameGenerationContext::ConfigureLowLatencyMode(const NRIRenderDevice&) {}
void NRIFrameGenerationContext::SetLowLatencyMarker(const NRIRenderDevice&, nri::LatencyMarker, nri::Result&) {}
void NRIFrameGenerationContext::ResetLowLatencyState() {}
void NRIFrameGenerationContext::ResetProviderState() {}
void NRIFrameGenerationContext::DestroyProviderPresentBridge() {}
void NRIFrameGenerationContext::ShutdownProvider() {}
bool NRIFrameGenerationContext::EnsureProviderRuntime(const NRIRenderDevice&) { return false; }
