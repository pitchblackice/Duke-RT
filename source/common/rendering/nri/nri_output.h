#pragma once

#include <cstdint>

enum class NRIPTOutputMode : uint32_t
{
	SDR = 0,
	HDRAuto = 1,
	HDRLinear16 = 2,
	HDR10PQ = 3
};

enum class NRIPTTonemapMode : uint32_t
{
	Hable = 0,
	ACESFitted = 1,
	Reinhard = 2
};

struct NRIPTOutputPolicy
{
	NRIPTOutputMode requestedMode = NRIPTOutputMode::SDR;
	NRIPTOutputMode resolvedMode = NRIPTOutputMode::SDR;
	NRIPTTonemapMode tonemapMode = NRIPTTonemapMode::Hable;
	float exposure = 1.0f;
	float paperWhiteNits = 200.0f;
	float displayMaxLuminance = 80.0f;
	float displaySdrLuminance = 80.0f;
	bool displayInfoAvailable = false;
	bool displayHdrSupported = false;
	bool hdrSwapChainActive = false;
	bool offscreenHdrTarget = true;
};

inline const char* GetNRIPTOutputModeName(NRIPTOutputMode mode)
{
	switch (mode)
	{
	case NRIPTOutputMode::SDR: return "sdr";
	case NRIPTOutputMode::HDRAuto: return "hdr-auto";
	case NRIPTOutputMode::HDRLinear16: return "hdr-linear16";
	case NRIPTOutputMode::HDR10PQ: return "hdr10-pq";
	default: return "unknown";
	}
}

inline const char* GetNRIPTTonemapModeName(NRIPTTonemapMode mode)
{
	switch (mode)
	{
	case NRIPTTonemapMode::Hable: return "hable";
	case NRIPTTonemapMode::ACESFitted: return "aces-fitted";
	case NRIPTTonemapMode::Reinhard: return "reinhard";
	default: return "unknown";
	}
}
