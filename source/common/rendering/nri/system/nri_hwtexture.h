#pragma once

#include "nri_local.h"

#include "hw_ihwtexture.h"

class FCanvasTexture;
class FTexture;
class NRIRenderDevice;

class NRIHardwareTexture final : public IHardwareTexture
{
public:
	NRIHardwareTexture(NRIRenderDevice* fb, int numchannels);
	~NRIHardwareTexture() override;

	void AllocateBuffer(int w, int h, int texelsize) override;
	uint8_t* MapBuffer() override;
	unsigned int CreateTexture(unsigned char* buffer, int w, int h, int texunit, bool mipmap, const char* name) override;

	void EnsureTexture(FTexture* tex, int translation, int flags);
	void EnsureCanvas(FTexture* tex, nri::Format format = nri::Format::BGRA8_UNORM);
	void CreateWipeTexture(int w, int h, const char* name);
	void Reset();

	NRITextureResource& GetResource() { return mResource; }
	const NRITextureResource& GetResource() const { return mResource; }

private:
	void CreateTextureResource(uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage);
	bool UploadTextureData(const void* data, uint32_t width, uint32_t height, nri::Format format, uint32_t rowPitch);

	NRIRenderDevice* mFrameBuffer = nullptr;
	NRITextureResource mResource;
	std::vector<uint8_t> mStagingPixels;
	int mChannels = 4;
	uint64_t mContentId = 0;
};
