#pragma once

#include "hwrenderer/data/buffers.h"

#include <array>
#include <vector>

class NRIHardwareBuffer : virtual public IBuffer
{
public:
	void SetData(size_t size, const void* data, BufferUsageType type) override;
	void SetSubData(size_t offset, size_t size, const void* data) override;
	void* Lock(unsigned int size) override;
	void Unlock() override;
	void Resize(size_t newsize) override;
	void Map() override;
	void Unmap() override;

	const uint8_t* Data() const { return mStorage.empty() ? nullptr : mStorage.data(); }
	uint8_t* Data() { return mStorage.empty() ? nullptr : mStorage.data(); }
	uint64_t Generation() const { return mGeneration; }

protected:
	std::vector<uint8_t> mStorage;
	bool mLocked = false;
	uint64_t mGeneration = 1;
};

class NRIHardwareVertexBuffer final : public IVertexBuffer, public NRIHardwareBuffer
{
public:
	void SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs) override;

	size_t GetStride() const { return mStride; }
	int GetAttributeCount() const { return mAttributeCount; }
	const FVertexBufferAttribute* GetAttributes() const { return mAttributes.data(); }
	uint64_t GetLayoutHash() const { return mLayoutHash; }

private:
	size_t mStride = 0;
	std::array<FVertexBufferAttribute, VATTR_MAX> mAttributes = {};
	int mAttributeCount = 0;
	uint64_t mLayoutHash = 0;
};

class NRIHardwareIndexBuffer final : public IIndexBuffer, public NRIHardwareBuffer
{
};

class NRIHardwareDataBuffer final : public IDataBuffer, public NRIHardwareBuffer
{
public:
	NRIHardwareDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

	void BindRange(FRenderState* state, size_t start, size_t length) override;

private:
	int mBindingPoint = 0;
	bool mIsSSBO = false;
	bool mNeedsResize = false;
};
