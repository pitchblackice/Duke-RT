#pragma once

#include <cstdint>
#include <cstring>

inline uint64_t NRIHashCombine64(uint64_t hash, uint64_t value)
{
	return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

inline uint64_t NRIHashUploadPayloadBytes(const void* data, uint64_t size)
{
	uint64_t hash = 1469598103934665603ull;
	hash = NRIHashCombine64(hash, size);
	if (data == nullptr || size == 0)
	{
		return hash != 0 ? hash : 1;
	}

	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	uint64_t offset = 0;
	for (; offset + sizeof(uint64_t) <= size; offset += sizeof(uint64_t))
	{
		uint64_t word = 0;
		std::memcpy(&word, bytes + offset, sizeof(word));
		hash = NRIHashCombine64(hash, word);
	}
	if (offset < size)
	{
		uint64_t tail = 0;
		std::memcpy(&tail, bytes + offset, (size_t)(size - offset));
		hash = NRIHashCombine64(hash, tail);
	}
	return hash != 0 ? hash : 1;
}
