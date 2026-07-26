#include "nri_persistent_voxel_shadow_proxy.h"
#include "nri_voxel_compute_meshing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
	enum class FaceDirection : uint8_t { NegX, PosX, NegY, PosY, Top, Bottom };

	struct FaceMask
	{
		FaceDirection direction = FaceDirection::NegX;
		uint32_t planeCount = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> cells;

		uint64_t CellCount() const { return (uint64_t)planeCount * width * height; }
		uint8_t& At(uint32_t plane, uint32_t x, uint32_t y)
		{
			return cells[((size_t)plane * height + y) * width + x];
		}
	};

	bool CheckedCellCount(uint32_t planes, uint32_t width, uint32_t height, uint64_t& outCount)
	{
		outCount = (uint64_t)planes * width * height;
		return planes != 0 && width != 0 && height != 0 && outCount <= (uint64_t)std::numeric_limits<size_t>::max();
	}

	std::array<float, 3> TransformVoxelPoint(
		const NRIVoxelComputeRawSourceArchiveSnapshot& source,
		int32_t x,
		int32_t y,
		int32_t z)
	{
		return { (float)x - source.pivotX, -(float)z + source.pivotZ, -(float)y + source.pivotY };
	}

	void EmitQuad(
		const NRIVoxelComputeRawSourceArchiveSnapshot& source,
		const std::array<std::array<int32_t, 3>, 4>& raw,
		NRIVoxelShadowProxyCpuGeometry& target)
	{
		const uint32_t vertexBase = (uint32_t)target.vertices.size();
		// Match the exact voxel emitter's p0,p1,p3,p2 raw-corner ordering.
		constexpr uint32_t CornerOrder[4] = { 0u, 1u, 3u, 2u };
		for (uint32_t corner : CornerOrder)
		{
			NRIVoxelShadowProxyVertex vertex = {};
			const std::array<float, 3> position = TransformVoxelPoint(
				source, raw[corner][0], raw[corner][1], raw[corner][2]);
			for (uint32_t axis = 0; axis < 3; ++axis)
			{
				vertex.position[axis] = position[axis];
				vertex.prevPosition[axis] = position[axis];
			}
			vertex.uv[0] = vertex.uv[1] = 0.5f;
			target.vertices.push_back(vertex);
		}
		const uint32_t indices[6] =
		{
			vertexBase + 0u, vertexBase + 1u, vertexBase + 3u,
			vertexBase + 1u, vertexBase + 2u, vertexBase + 3u,
		};
		target.indices.insert(target.indices.end(), std::begin(indices), std::end(indices));
	}

	void EmitRectangle(
		const NRIVoxelComputeRawSourceArchiveSnapshot& source,
		FaceDirection direction,
		uint32_t plane,
		uint32_t u,
		uint32_t v,
		uint32_t width,
		uint32_t height,
		NRIVoxelShadowProxyCpuGeometry& target)
	{
		const int32_t p = (int32_t)plane;
		const int32_t u0 = (int32_t)u;
		const int32_t u1 = (int32_t)(u + width);
		const int32_t v0 = (int32_t)v;
		const int32_t v1 = (int32_t)(v + height);
		std::array<std::array<int32_t, 3>, 4> raw = {};
		switch (direction)
		{
		case FaceDirection::NegX:
			raw = {{{p,u0,v0}, {p,u1,v0}, {p,u0,v1}, {p,u1,v1}}};
			break;
		case FaceDirection::PosX:
			raw = {{{p,u1,v0}, {p,u0,v0}, {p,u1,v1}, {p,u0,v1}}};
			break;
		case FaceDirection::NegY:
			raw = {{{u1,p,v0}, {u0,p,v0}, {u1,p,v1}, {u0,p,v1}}};
			break;
		case FaceDirection::PosY:
			raw = {{{u0,p,v0}, {u1,p,v0}, {u0,p,v1}, {u1,p,v1}}};
			break;
		case FaceDirection::Top:
			raw = {{{u0,v0,p}, {u1,v0,p}, {u0,v1,p}, {u1,v1,p}}};
			break;
		case FaceDirection::Bottom:
			raw = {{{u1,v0,p}, {u0,v0,p}, {u1,v1,p}, {u0,v1,p}}};
			break;
		}
		EmitQuad(source, raw, target);
	}

	void GreedyEmit(
		const NRIVoxelComputeRawSourceArchiveSnapshot& source,
		FaceMask& mask,
		NRIVoxelShadowProxyCpuGeometry& target)
	{
		for (uint32_t plane = 0; plane < mask.planeCount; ++plane)
		{
			for (uint32_t y = 0; y < mask.height; ++y)
			{
				for (uint32_t x = 0; x < mask.width; ++x)
				{
					if (mask.At(plane, x, y) == 0)
					{
						continue;
					}
					uint32_t width = 1;
					while (x + width < mask.width && mask.At(plane, x + width, y) != 0)
					{
						width++;
					}
					uint32_t height = 1;
					for (; y + height < mask.height; ++height)
					{
						bool fullRow = true;
						for (uint32_t offset = 0; offset < width; ++offset)
						{
							fullRow = fullRow && mask.At(plane, x + offset, y + height) != 0;
						}
						if (!fullRow) break;
					}
					for (uint32_t clearY = 0; clearY < height; ++clearY)
					{
						for (uint32_t clearX = 0; clearX < width; ++clearX)
						{
							mask.At(plane, x + clearX, y + clearY) = 0;
						}
					}
					EmitRectangle(source, mask.direction, plane, x, y, width, height, target);
				}
			}
		}
	}

}

bool BuildNRIVoxelShadowProxyGeometry(
	const NRIVoxelComputeRawSourceArchiveSnapshot& source,
	const NRIVoxelShadowProxyBuildLimits& limits,
	NRIVoxelShadowProxyCpuGeometry& outGeometry,
	NRIVoxelShadowProxyRejectReason& outReason)
{
	outGeometry = {};
	outReason = NRIVoxelShadowProxyRejectReason::None;
	if (source.recordSerial == 0 || source.sizeX == 0 || source.sizeY == 0 || source.sizeZ == 0 ||
		source.slabs.empty() || source.exactPrimitiveCount == 0)
	{
		outReason = NRIVoxelShadowProxyRejectReason::MissingSource;
		return false;
	}

	FaceMask masks[6] =
	{
		{ FaceDirection::NegX, source.sizeX + 1u, source.sizeY, source.sizeZ },
		{ FaceDirection::PosX, source.sizeX + 1u, source.sizeY, source.sizeZ },
		{ FaceDirection::NegY, source.sizeY + 1u, source.sizeX, source.sizeZ },
		{ FaceDirection::PosY, source.sizeY + 1u, source.sizeX, source.sizeZ },
		{ FaceDirection::Top, source.sizeZ + 1u, source.sizeX, source.sizeY },
		{ FaceDirection::Bottom, source.sizeZ + 1u, source.sizeX, source.sizeY },
	};
	uint64_t totalCells = 0;
	for (FaceMask& mask : masks)
	{
		uint64_t cells = 0;
		if (!CheckedCellCount(mask.planeCount, mask.width, mask.height, cells) ||
			cells > limits.maxTemporaryMaskCells - std::min(totalCells, limits.maxTemporaryMaskCells))
		{
			outReason = NRIVoxelShadowProxyRejectReason::TemporaryMemoryLimit;
			return false;
		}
		totalCells += cells;
		mask.cells.resize((size_t)cells);
	}

	for (const NRIVoxelComputeRawSlabSnapshot& slab : source.slabs)
	{
		if (slab.x >= source.sizeX || slab.y >= source.sizeY || slab.zLength == 0 ||
			slab.zTop >= source.sizeZ || (uint64_t)slab.zTop + slab.zLength > source.sizeZ)
		{
			outReason = NRIVoxelShadowProxyRejectReason::InvalidRawRange;
			return false;
		}
		for (uint32_t z = slab.zTop; z < slab.zTop + slab.zLength; ++z)
		{
			if ((slab.cullMask & 1u) != 0) masks[0].At(slab.x, slab.y, z) = 1;
			if ((slab.cullMask & 2u) != 0) masks[1].At(slab.x + 1u, slab.y, z) = 1;
			if ((slab.cullMask & 4u) != 0) masks[2].At(slab.y, slab.x, z) = 1;
			if ((slab.cullMask & 8u) != 0) masks[3].At(slab.y + 1u, slab.x, z) = 1;
		}
		if ((slab.cullMask & 16u) != 0) masks[4].At(slab.zTop, slab.x, slab.y) = 1;
		if ((slab.cullMask & 32u) != 0) masks[5].At(slab.zTop + slab.zLength, slab.x, slab.y) = 1;
	}

	outGeometry.temporaryMaskCells = totalCells;
	outGeometry.exactPrimitiveCount = source.exactPrimitiveCount;
	for (FaceMask& mask : masks)
	{
		GreedyEmit(source, mask, outGeometry);
	}
	outGeometry.proxyPrimitiveCount = (uint32_t)(outGeometry.indices.size() / 3u);
	if (outGeometry.vertices.empty() || outGeometry.proxyPrimitiveCount == 0)
	{
		outReason = NRIVoxelShadowProxyRejectReason::EmptyGeometry;
		return false;
	}
	if (outGeometry.proxyPrimitiveCount >= source.exactPrimitiveCount)
	{
		outReason = NRIVoxelShadowProxyRejectReason::NoPrimitiveSavings;
		return false;
	}

	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		outGeometry.boundsMin[axis] = outGeometry.vertices[0].position[axis];
		outGeometry.boundsMax[axis] = outGeometry.vertices[0].position[axis];
	}
	for (const NRIVoxelShadowProxyVertex& vertex : outGeometry.vertices)
	{
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			outGeometry.boundsMin[axis] = std::min(outGeometry.boundsMin[axis], vertex.position[axis]);
			outGeometry.boundsMax[axis] = std::max(outGeometry.boundsMax[axis], vertex.position[axis]);
		}
	}
	outGeometry.boundsValid = true;
	return true;
}

bool CertifyNRIVoxelShadowProxyPrimitiveFacts(const NRIVoxelShadowProxyPrimitiveFacts& facts)
{
	return facts.flagsSupported && facts.portalFree && facts.materialInRange;
}

bool CertifyNRIVoxelShadowProxyMaterialFacts(
	const NRIVoxelShadowProxyMaterialFacts& facts,
	NRIVoxelShadowProxyRejectReason& outReason)
{
	outReason = NRIVoxelShadowProxyRejectReason::None;
	if (!facts.flagsSupported)
	{
		outReason = NRIVoxelShadowProxyRejectReason::MaterialFlags;
		return false;
	}
	if (!facts.alphaOpaque)
	{
		outReason = NRIVoxelShadowProxyRejectReason::MaterialAlpha;
		return false;
	}
	if (!facts.lightingNeutral)
	{
		outReason = NRIVoxelShadowProxyRejectReason::NoShadowMaterial;
		return false;
	}
	if (!facts.emissiveFree)
	{
		outReason = NRIVoxelShadowProxyRejectReason::EmissiveMaterial;
		return false;
	}
	if (!facts.actorOverlayFree)
	{
		outReason = NRIVoxelShadowProxyRejectReason::ActorOverlay;
		return false;
	}
	return true;
}

bool CertifyNRIVoxelShadowProxyMaterialClosureFacts(
	const std::vector<NRIVoxelShadowProxyMaterialFacts>& materials,
	bool hasActorOverlayLights,
	NRIVoxelShadowProxyRejectReason& outReason)
{
	outReason = NRIVoxelShadowProxyRejectReason::None;
	if (hasActorOverlayLights)
	{
		outReason = NRIVoxelShadowProxyRejectReason::ActorOverlay;
		return false;
	}
	if (materials.empty())
	{
		outReason = NRIVoxelShadowProxyRejectReason::MaterialClosure;
		return false;
	}
	for (const NRIVoxelShadowProxyMaterialFacts& material : materials)
	{
		if (!CertifyNRIVoxelShadowProxyMaterialFacts(material, outReason))
		{
			return false;
		}
	}
	return true;
}

bool IsNRIVoxelShadowProxyTransformValid(const std::array<float, 12>& transform)
{
	for (float value : transform) if (!std::isfinite(value)) return false;
	const float determinant =
		transform[0] * (transform[5] * transform[10] - transform[6] * transform[9]) -
		transform[1] * (transform[4] * transform[10] - transform[6] * transform[8]) +
		transform[2] * (transform[4] * transform[9] - transform[5] * transform[8]);
	return std::isfinite(determinant) && std::abs(determinant) > 1.0e-8f;
}

bool IsNRIVoxelShadowProxyBoundsEquivalent(
	const float exactMin[3], const float exactMax[3],
	const float proxyMin[3], const float proxyMax[3])
{
	constexpr float Epsilon = 0.0001f;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite(exactMin[axis]) || !std::isfinite(exactMax[axis]) ||
			!std::isfinite(proxyMin[axis]) || !std::isfinite(proxyMax[axis]) ||
			std::abs(exactMin[axis] - proxyMin[axis]) > Epsilon ||
			std::abs(exactMax[axis] - proxyMax[axis]) > Epsilon)
		{
			return false;
		}
	}
	return true;
}

const char* GetNRIVoxelShadowProxyRejectReasonName(NRIVoxelShadowProxyRejectReason reason)
{
	switch (reason)
	{
	case NRIVoxelShadowProxyRejectReason::None: return "none";
	case NRIVoxelShadowProxyRejectReason::Disabled: return "disabled";
	case NRIVoxelShadowProxyRejectReason::MissingSource: return "missing-source";
	case NRIVoxelShadowProxyRejectReason::ArchiveUnavailable: return "archive-unavailable";
	case NRIVoxelShadowProxyRejectReason::ArchiveMismatch: return "archive-mismatch";
	case NRIVoxelShadowProxyRejectReason::NonLocalGeometry: return "non-local";
	case NRIVoxelShadowProxyRejectReason::InvalidRawRange: return "invalid-raw-range";
	case NRIVoxelShadowProxyRejectReason::TemporaryMemoryLimit: return "temporary-memory-limit";
	case NRIVoxelShadowProxyRejectReason::EmptyGeometry: return "empty-geometry";
	case NRIVoxelShadowProxyRejectReason::NoPrimitiveSavings: return "no-primitive-savings";
	case NRIVoxelShadowProxyRejectReason::BoundsMismatch: return "bounds-mismatch";
	case NRIVoxelShadowProxyRejectReason::PrimitiveSemantics: return "primitive-semantics";
	case NRIVoxelShadowProxyRejectReason::MaterialClosure: return "material-closure";
	case NRIVoxelShadowProxyRejectReason::MaterialFlags: return "material-flags";
	case NRIVoxelShadowProxyRejectReason::MaterialAlpha: return "material-alpha";
	case NRIVoxelShadowProxyRejectReason::NoShadowMaterial: return "no-shadow-material";
	case NRIVoxelShadowProxyRejectReason::EmissiveMaterial: return "emissive-material";
	case NRIVoxelShadowProxyRejectReason::ActorOverlay: return "actor-overlay";
	case NRIVoxelShadowProxyRejectReason::InvalidTransform: return "invalid-transform";
	case NRIVoxelShadowProxyRejectReason::ResourceUnavailable: return "resource-unavailable";
	case NRIVoxelShadowProxyRejectReason::TransitionLimited: return "transition-limited";
	default: return "unknown";
	}
}
