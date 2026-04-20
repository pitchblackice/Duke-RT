#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "i_video.h"
#include "win32nrivideo.h"

#include "c_cvars.h"
#include "nri/system/nri_renderdevice.h"

EXTERN_CVAR(Bool, vid_fullscreen)

void Win32NRIVideo::Shutdown()
{
}

DFrameBuffer *Win32NRIVideo::CreateFrameBuffer()
{
	return new NRIRenderDevice(m_hMonitor, vid_fullscreen);
}
