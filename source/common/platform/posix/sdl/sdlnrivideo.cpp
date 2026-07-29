//
// sdlnrivideo.cpp
//
// Native window handle export for the NRI path tracing backend.
//
// Unlike the Vulkan backend, NRI creates its own Vulkan instance and surface, so
// it needs the raw X11 or Wayland handles rather than a ready-made VkSurfaceKHR.
//
// This lives in its own translation unit on purpose: <SDL_syswm.h> pulls in
// Xlib.h, whose "GC" typedef collides with the engine's GC namespace. Keeping the
// include isolated avoids that clash, so no engine headers are included here.
//

#include <cstdint>

#include <SDL.h>
#include <SDL_syswm.h>

// Defined in sdlglvideo.cpp.
extern SDL_Window* I_GetSDLWindowHandle();

// Fills exactly one of the X11 or Wayland handle pairs. Returns false when there
// is no window yet, or when the window system is not one NRI can target.
bool I_GetNriWindowHandles(void** x11Display, uint64_t* x11Window, void** waylandDisplay, void** waylandSurface)
{
	*x11Display = nullptr;
	*x11Window = 0;
	*waylandDisplay = nullptr;
	*waylandSurface = nullptr;

	SDL_Window* window = I_GetSDLWindowHandle();
	if (window == nullptr)
	{
		return false;
	}

	SDL_SysWMinfo info = {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE)
	{
		return false;
	}

	switch (info.subsystem)
	{
#ifdef SDL_VIDEO_DRIVER_X11
	case SDL_SYSWM_X11:
		*x11Display = (void*)info.info.x11.display;
		*x11Window = (uint64_t)info.info.x11.window;
		return true;
#endif
#ifdef SDL_VIDEO_DRIVER_WAYLAND
	case SDL_SYSWM_WAYLAND:
		*waylandDisplay = (void*)info.info.wl.display;
		*waylandSurface = (void*)info.info.wl.surface;
		return true;
#endif
	default:
		SDL_Log("Unsupported SDL window system for the NRI backend.");
		return false;
	}
}
