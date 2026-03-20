#include "nri_hwtexture.h"

#include "nri_renderdevice.h"
#include "image.h"
#include "printf.h"
#include "textures.h"

NRIHardwareTexture::NRIHardwareTexture(NRIRenderDevice* fb, int numchannels)
	: mFrameBuffer(fb), mChannels(numchannels)
{
}

NRIHardwareTexture::~NRIHardwareTexture()
{
	Reset();
}

void NRIHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (w <= 0 || h <= 0 || texelsize <= 0)
	{
		mStagingPixels.clear();
		bufferpitch = 0;
		return;
	}

	mChannels = texelsize;
	bufferpitch = w;
	mStagingPixels.resize((size_t)w * (size_t)h * (size_t)texelsize);

	const nri::Format format = texelsize == 1 ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	CreateTextureResource((uint32_t)w, (uint32_t)h, format, nri::TextureUsageBits::SHADER_RESOURCE);
	mContentId = 0;
}

uint8_t* NRIHardwareTexture::MapBuffer()
{
	return mStagingPixels.empty() ? nullptr : mStagingPixels.data();
}

unsigned int NRIHardwareTexture::CreateTexture(unsigned char* buffer, int w, int h, int, bool, const char*)
{
	if (w <= 0 || h <= 0)
	{
		return 0;
	}

	const nri::Format format = mChannels == 1 ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	CreateTextureResource((uint32_t)w, (uint32_t)h, format, nri::TextureUsageBits::SHADER_RESOURCE);

	if (buffer != nullptr)
	{
		UploadTextureData(buffer, (uint32_t)w, (uint32_t)h, format, (uint32_t)w * (uint32_t)mChannels);
	}
	else if (!mStagingPixels.empty())
	{
		UploadTextureData(mStagingPixels.data(), (uint32_t)w, (uint32_t)h, format, (uint32_t)w * (uint32_t)mChannels);
	}

	mContentId = 0;
	return 0;
}

void NRIHardwareTexture::EnsureTexture(FTexture* tex, int translation, int flags)
{
	static int sTextureUploadLogCount = 0;

	if (tex == nullptr)
	{
		return;
	}

	if (tex->isHardwareCanvas())
	{
		EnsureCanvas(tex);
		return;
	}

	FTextureBuffer texBuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
	if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mContentId == texBuffer.mContentId &&
		mResource.width == (uint32_t)texBuffer.mWidth &&
		mResource.height == (uint32_t)texBuffer.mHeight &&
		mResource.format == nri::Format::BGRA8_UNORM)
	{
		return;
	}

	CreateTextureResource((uint32_t)texBuffer.mWidth, (uint32_t)texBuffer.mHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE);
	if (sTextureUploadLogCount < 32)
	{
		auto* image = tex->GetImage();
		Printf("NRI texture upload[%d]: ptr=%p size=%dx%d src=%dx%d rowPitch=%u translation=%d flags=0x%x lump=%d image=%s imageId=%d content=%llu\n",
			sTextureUploadLogCount,
			texBuffer.mBuffer,
			texBuffer.mWidth,
			texBuffer.mHeight,
			tex->GetWidth(),
			tex->GetHeight(),
			(uint32_t)texBuffer.mWidth * 4u,
			translation,
			flags,
			tex->GetSourceLump(),
			image != nullptr ? "yes" : "no",
			image != nullptr ? image->GetId() : -1,
			(unsigned long long)texBuffer.mContentId);
		sTextureUploadLogCount++;
	}
	UploadTextureData(texBuffer.mBuffer, (uint32_t)texBuffer.mWidth, (uint32_t)texBuffer.mHeight, nri::Format::BGRA8_UNORM, (uint32_t)texBuffer.mWidth * 4u);
	mContentId = texBuffer.mContentId;
}

void NRIHardwareTexture::EnsureCanvas(FTexture* tex)
{
	if (tex == nullptr)
	{
		return;
	}

	const uint32_t width = (uint32_t)tex->GetWidth();
	const uint32_t height = (uint32_t)tex->GetHeight();
	if (width == 0 || height == 0)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mResource.width == width &&
		mResource.height == height &&
		mResource.colorAttachmentView != nullptr)
	{
		return;
	}

	CreateTextureResource(width, height, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT);
	mContentId = 0;
}

void NRIHardwareTexture::CreateWipeTexture(int w, int h, const char*)
{
	if (w <= 0 || h <= 0)
	{
		return;
	}

	CreateTextureResource((uint32_t)w, (uint32_t)h, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT);
	mFrameBuffer->CopyCurrentTargetToTexture(mResource);
	mContentId = 0;
}

void NRIHardwareTexture::Reset()
{
	if (mFrameBuffer != nullptr)
	{
		mFrameBuffer->DestroyTextureResource(mResource);
	}

	mStagingPixels.clear();
	bufferpitch = 0;
	mContentId = 0;
}

void NRIHardwareTexture::CreateTextureResource(uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage)
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mResource.width == width &&
		mResource.height == height &&
		mResource.format == format &&
		((((uint32_t)usage & (uint32_t)nri::TextureUsageBits::COLOR_ATTACHMENT) == 0) || mResource.colorAttachmentView != nullptr))
	{
		return;
	}

	mFrameBuffer->DestroyTextureResource(mResource);
	mFrameBuffer->CreateOwnedTexture(mResource, width, height, format, usage);
}

void NRIHardwareTexture::UploadTextureData(const void* data, uint32_t width, uint32_t height, nri::Format format, uint32_t rowPitch)
{
	if (mFrameBuffer == nullptr || mResource.texture == nullptr || data == nullptr)
	{
		return;
	}

	mFrameBuffer->UploadTextureData(mResource, data, width, height, rowPitch);
	mResource.format = format;
}
