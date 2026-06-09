#pragma once

#include <cstddef>
#include <cstdint>

namespace nri_scene
{
constexpr uint64_t NRIHashFnv1a64OffsetBasis = 1469598103934665603ull;
constexpr uint64_t NRIHashFnv1a64Prime = 1099511628211ull;

inline uint64_t HashCombine64(uint64_t hash, uint64_t value)
{
	return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

inline uint64_t Fnv1a64(const uint8_t* data, size_t size)
{
	uint64_t hash = NRIHashFnv1a64OffsetBasis;
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= (uint64_t)data[i];
		hash *= NRIHashFnv1a64Prime;
	}
	return hash;
}

inline void Fnv1a64Append(uint64_t& hash, const void* data, size_t size)
{
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= (uint64_t)bytes[i];
		hash *= NRIHashFnv1a64Prime;
	}
}
}
