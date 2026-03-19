#include "nri_renderstate.h"

NRIRenderState::NRIRenderState()
{
	Reset();
}

void NRIRenderState::ClearScreen()
{
}

void NRIRenderState::Draw(int dt, int index, int count, bool apply)
{
	static_cast<void>(dt);
	static_cast<void>(index);
	static_cast<void>(count);
	static_cast<void>(apply);
}

void NRIRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	static_cast<void>(dt);
	static_cast<void>(index);
	static_cast<void>(count);
	static_cast<void>(apply);
}

bool NRIRenderState::SetDepthClamp(bool on)
{
	static_cast<void>(on);
	return true;
}

void NRIRenderState::SetDepthMask(bool on)
{
	static_cast<void>(on);
}

void NRIRenderState::SetDepthFunc(int func)
{
	static_cast<void>(func);
}

void NRIRenderState::SetDepthRange(float min, float max)
{
	static_cast<void>(min);
	static_cast<void>(max);
}

void NRIRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	static_cast<void>(r);
	static_cast<void>(g);
	static_cast<void>(b);
	static_cast<void>(a);
}

void NRIRenderState::SetStencil(int offs, int op, int flags)
{
	static_cast<void>(offs);
	static_cast<void>(op);
	static_cast<void>(flags);
}

void NRIRenderState::SetCulling(int mode)
{
	static_cast<void>(mode);
}

void NRIRenderState::EnableClipDistance(int num, bool state)
{
	static_cast<void>(num);
	static_cast<void>(state);
}

void NRIRenderState::Clear(int targets)
{
	static_cast<void>(targets);
}

void NRIRenderState::EnableStencil(bool on)
{
	static_cast<void>(on);
}

void NRIRenderState::SetScissor(int x, int y, int w, int h)
{
	static_cast<void>(x);
	static_cast<void>(y);
	static_cast<void>(w);
	static_cast<void>(h);
}

void NRIRenderState::SetViewport(int x, int y, int w, int h)
{
	static_cast<void>(x);
	static_cast<void>(y);
	static_cast<void>(w);
	static_cast<void>(h);
}

void NRIRenderState::EnableDepthTest(bool on)
{
	static_cast<void>(on);
}

void NRIRenderState::EnableMultisampling(bool on)
{
	static_cast<void>(on);
}

void NRIRenderState::EnableLineSmooth(bool on)
{
	static_cast<void>(on);
}

void NRIRenderState::EnableDrawBuffers(int count, bool apply)
{
	static_cast<void>(count);
	static_cast<void>(apply);
}
