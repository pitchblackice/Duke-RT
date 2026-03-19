#pragma once

#include "win32basevideo.h"

class Win32NRIVideo : public Win32BaseVideo
{
public:
	Win32NRIVideo() = default;

	void Shutdown() override;
	DFrameBuffer *CreateFrameBuffer() override;
};
