#include "nri_persistent_voxel_shadow_proxy.h"
#include "nri_frame_resources.h"
#include "nri_voxel_compute_meshing.h"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace
{
	NRIVoxelComputeRawSourceArchiveSnapshot TwoVoxelSource()
	{
		NRIVoxelComputeRawSourceArchiveSnapshot source = {};
		source.recordSerial = 4u;
		source.contentHash = 9u;
		source.sizeX = 2u;
		source.sizeY = 1u;
		source.sizeZ = 1u;
		source.exactFaceCount = 10u;
		source.exactPrimitiveCount = 20u;
		source.slabs =
		{
			{ 0u, 0u, 0u, 1u | 4u | 8u | 16u | 32u, 1u },
			{ 1u, 0u, 0u, 2u | 4u | 8u | 16u | 32u, 1u },
		};
		return source;
	}

	bool GreedyMeshPreservesBoundsAndReducesPrimitives()
	{
		NRIVoxelShadowProxyCpuGeometry geometry = {};
		NRIVoxelShadowProxyRejectReason reason = NRIVoxelShadowProxyRejectReason::None;
		const bool built = BuildNRIVoxelShadowProxyGeometry(
			TwoVoxelSource(), NRIVoxelShadowProxyBuildLimits{}, geometry, reason);
		return built && reason == NRIVoxelShadowProxyRejectReason::None &&
			geometry.exactPrimitiveCount == 20u && geometry.proxyPrimitiveCount == 12u &&
			geometry.vertices.size() == 24u && geometry.indices.size() == 36u &&
			geometry.boundsValid &&
			geometry.boundsMin[0] == 0.0f && geometry.boundsMax[0] == 2.0f &&
			geometry.boundsMin[1] == -1.0f && geometry.boundsMax[1] == 0.0f &&
			geometry.boundsMin[2] == -1.0f && geometry.boundsMax[2] == 0.0f;
	}

	bool TemporaryMemoryCapFailsClosed()
	{
		NRIVoxelShadowProxyCpuGeometry geometry = {};
		NRIVoxelShadowProxyRejectReason reason = NRIVoxelShadowProxyRejectReason::None;
		NRIVoxelShadowProxyBuildLimits limits = {};
		limits.maxTemporaryMaskCells = 1u;
		return !BuildNRIVoxelShadowProxyGeometry(TwoVoxelSource(), limits, geometry, reason) &&
			reason == NRIVoxelShadowProxyRejectReason::TemporaryMemoryLimit;
	}

	bool MaterialCertificationIsConservative()
	{
		NRIVoxelShadowProxyRejectReason reason = NRIVoxelShadowProxyRejectReason::None;
		const std::vector<NRIVoxelShadowProxyMaterialFacts> valid(1);
		if (!CertifyNRIVoxelShadowProxyMaterialClosureFacts(valid, false, reason)) return false;

		auto alpha = valid;
		alpha[0].flagsSupported = false;
		if (CertifyNRIVoxelShadowProxyMaterialClosureFacts(alpha, false, reason) ||
			reason != NRIVoxelShadowProxyRejectReason::MaterialFlags) return false;

		auto noShadow = valid;
		noShadow[0].lightingNeutral = false;
		if (CertifyNRIVoxelShadowProxyMaterialClosureFacts(noShadow, false, reason) ||
			reason != NRIVoxelShadowProxyRejectReason::NoShadowMaterial) return false;

		auto emissive = valid;
		emissive[0].emissiveFree = false;
		if (CertifyNRIVoxelShadowProxyMaterialClosureFacts(emissive, false, reason) ||
			reason != NRIVoxelShadowProxyRejectReason::EmissiveMaterial) return false;

		if (CertifyNRIVoxelShadowProxyMaterialClosureFacts(valid, true, reason) ||
			reason != NRIVoxelShadowProxyRejectReason::ActorOverlay) return false;
		return true;
	}

	bool PrimitiveAndTransformCertificationFailsClosed()
	{
		NRIVoxelShadowProxyPrimitiveFacts primitive = {};
		if (!CertifyNRIVoxelShadowProxyPrimitiveFacts(primitive)) return false;
		primitive.portalFree = false;
		if (CertifyNRIVoxelShadowProxyPrimitiveFacts(primitive)) return false;

		std::array<float, 12> transform =
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f, 0.0f, 0.0f,
			0.0f, 0.0f, -1.0f, 0.0f,
		};
		if (!IsNRIVoxelShadowProxyTransformValid(transform)) return false;
		transform[10] = 0.0f;
		return !IsNRIVoxelShadowProxyTransformValid(transform);
	}

	bool VisibilityMarkerIsReservedAndReversible()
	{
		const uint32_t marker = EncodeNRIVoxelShadowProxyVisibility(1234u);
		return marker != UINT32_MAX && IsNRIVoxelShadowProxyVisibility(marker) &&
			DecodeNRIVoxelShadowProxyPrimitiveCount(marker) == 1234u &&
			!IsNRIVoxelShadowProxyVisibility(17u) &&
			DecodeNRIVoxelShadowProxyPrimitiveCount(UINT32_MAX) == 0u;
	}

	bool Run(const char* name, bool (*test)())
	{
		if (test()) return true;
		std::cerr << "FAILED: " << name << '\n';
		return false;
	}
}

int main()
{
	bool passed = true;
	passed &= Run("greedy exposed surface", GreedyMeshPreservesBoundsAndReducesPrimitives);
	passed &= Run("temporary memory cap", TemporaryMemoryCapFailsClosed);
	passed &= Run("material certification", MaterialCertificationIsConservative);
	passed &= Run("primitive and transform certification", PrimitiveAndTransformCertificationFailsClosed);
	passed &= Run("visibility marker", VisibilityMarkerIsReservedAndReversible);
	return passed ? 0 : 1;
}
