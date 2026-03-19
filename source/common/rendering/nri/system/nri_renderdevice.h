#pragma once

#include "base_sysfb.h"

#include <memory>

class NRIRenderState;

class NRIRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;

public:
	NRIRenderDevice(void *hMonitor, bool fullscreen);
	~NRIRenderDevice();

	void Update() override;
	void InitializeState() override;
	bool CompileNextShader() override;
	int GetShaderCount() override;
	int Backend() override { return 4; }
	const char* DeviceName() const override;
	void BeginFrame() override;
	FRenderState* RenderState() override;
	void Draw2D() override;

private:
	void LogStubWarning();

	std::unique_ptr<NRIRenderState> mRenderState;
	bool mLoggedStubWarning = false;
};
