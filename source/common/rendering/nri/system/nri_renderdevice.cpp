#include "nri_renderdevice.h"

#include "../renderer/nri_renderstate.h"
#include "c_cvars.h"
#include "printf.h"
#include "v_2ddrawer.h"
#include "v_draw.h"

EXTERN_CVAR(String, nri_api)

NRIRenderDevice::NRIRenderDevice(void *hMonitor, bool fullscreen)
	: SystemBaseFrameBuffer(hMonitor, fullscreen), mRenderState(std::make_unique<NRIRenderState>())
{
	vendorstring = "NRI";
	glslversion = 6.6f;
}

NRIRenderDevice::~NRIRenderDevice() = default;

void NRIRenderDevice::Update()
{
	Super::Update();
}

void NRIRenderDevice::InitializeState()
{
	LogStubWarning();
	SetViewportRects(nullptr);
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
	return "NRI backend shell";
}

void NRIRenderDevice::BeginFrame()
{
}

FRenderState* NRIRenderDevice::RenderState()
{
	return mRenderState.get();
}

void NRIRenderDevice::Draw2D()
{
	if (twod != nullptr)
	{
		::Draw2D(twod, *mRenderState);
	}
}

void NRIRenderDevice::LogStubWarning()
{
	if (mLoggedStubWarning)
	{
		return;
	}

	Printf("NRI backend shell initialized with API '%s'.\n", (const char*)nri_api);
	mLoggedStubWarning = true;
}
