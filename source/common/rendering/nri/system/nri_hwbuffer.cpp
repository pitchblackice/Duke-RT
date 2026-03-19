#include "nri_hwbuffer.h"

#include "hw_renderstate.h"

#include <algorithm>
#include <cstring>

void NRIHardwareBuffer::SetData(size_t size, const void* data, BufferUsageType)
{
	mStorage.resize(size);
	buffersize = size;

	if (data != nullptr && size != 0)
	{
		std::memcpy(mStorage.data(), data, size);
	}
	else if (size != 0)
	{
		std::memset(mStorage.data(), 0, size);
	}

	map = mStorage.empty() ? nullptr : mStorage.data();
	mGeneration++;
}

void NRIHardwareBuffer::SetSubData(size_t offset, size_t size, const void* data)
{
	if (offset + size > mStorage.size())
	{
		mStorage.resize(offset + size);
		buffersize = mStorage.size();
	}

	if (size != 0 && data != nullptr)
	{
		std::memcpy(mStorage.data() + offset, data, size);
	}

	map = mStorage.empty() ? nullptr : mStorage.data();
	mGeneration++;
}

void* NRIHardwareBuffer::Lock(unsigned int size)
{
	if (size > mStorage.size())
	{
		mStorage.resize(size);
		buffersize = size;
	}

	mLocked = true;
	map = mStorage.empty() ? nullptr : mStorage.data();
	return map;
}

void NRIHardwareBuffer::Unlock()
{
	mLocked = false;
	map = mStorage.empty() ? nullptr : mStorage.data();
	mGeneration++;
}

void NRIHardwareBuffer::Resize(size_t newsize)
{
	mStorage.resize(newsize);
	buffersize = newsize;
	map = mStorage.empty() ? nullptr : mStorage.data();
	mGeneration++;
}

void NRIHardwareBuffer::Map()
{
	map = mStorage.empty() ? nullptr : mStorage.data();
}

void NRIHardwareBuffer::Unmap()
{
	map = mStorage.empty() ? nullptr : mStorage.data();
	mGeneration++;
}

void NRIHardwareVertexBuffer::SetFormat(int, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs)
{
	mStride = stride;
	mAttributeCount = numAttributes < VATTR_MAX ? numAttributes : VATTR_MAX;
	mLayoutHash = (uint64_t)stride;

	for (int i = 0; i < mAttributeCount; ++i)
	{
		mAttributes[i] = attrs[i];
		mLayoutHash = (mLayoutHash * 1099511628211ull) ^ (uint64_t)mAttributes[i].binding;
		mLayoutHash = (mLayoutHash * 1099511628211ull) ^ (uint64_t)mAttributes[i].location;
		mLayoutHash = (mLayoutHash * 1099511628211ull) ^ (uint64_t)mAttributes[i].format;
		mLayoutHash = (mLayoutHash * 1099511628211ull) ^ (uint64_t)mAttributes[i].offset;
	}
}

NRIHardwareDataBuffer::NRIHardwareDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
	: mBindingPoint(bindingpoint), mIsSSBO(ssbo), mNeedsResize(needsresize)
{
}

void NRIHardwareDataBuffer::BindRange(FRenderState*, size_t, size_t)
{
}
