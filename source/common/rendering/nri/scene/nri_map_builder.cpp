#include "nri_map_builder.h"

#include "build.h"
#include "gamefuncs.h"
#include "hw_sections.h"
#include "mapinfo.h"
#include "sectorgeometry.h"
#include "texturemanager.h"

#include <algorithm>
#include <cmath>

namespace
{
	using namespace nri_scene;

	uint64_t gPendingLevelGeometryBuildSerial = 0;

	enum class PTWallBandKind : uint32_t
	{
		OneSided = 0,
		Upper,
		Middle,
		Lower,
		Portal,
	};

	struct PTWallTexCoord
	{
		float u = 0.0f;
		float v = 0.0f;
	};

	struct PTWallQuad
	{
		float x1 = 0.0f;
		float y1 = 0.0f;
		float x2 = 0.0f;
		float y2 = 0.0f;
		float fracLeft = 0.0f;
		float fracRight = 1.0f;
		float zTop[2] = {};
		float zBottom[2] = {};
		PTWallTexCoord texcoords[4] = {};
	};

	enum PTWallTexCoordIndex
	{
		PTWallTexCoord_LowerLeft = 0,
		PTWallTexCoord_UpperLeft,
		PTWallTexCoord_UpperRight,
		PTWallTexCoord_LowerRight,
	};

	struct PTWallBandDesc
	{
		PTMapSurfaceKind surfaceKind = PTMapSurfaceKind::WallOneSided;
		PTWallBandKind bandKind = PTWallBandKind::OneSided;
		SurfaceSourceType sourceType = SurfaceSourceType::MapWallBand;
		walltype* wall = nullptr;
		walltype* refWall = nullptr;
		sectortype* frontSector = nullptr;
		sectortype* backSector = nullptr;
		FGameTexture* texture = nullptr;
		int shade = 0;
		int palette = 0;
		float alpha = 1.0f;
		uint32_t materialFlags = MaterialFlag_None;
		float topLeft = 0.0f;
		float topRight = 0.0f;
		float bottomLeft = 0.0f;
		float bottomRight = 0.0f;
		float referenceHeight = 0.0f;
	};

	uint32_t CountTriangles(const SurfaceRef& surface)
	{
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2 : 0;
	}

	void AppendSurface(PTMapWorld& outWorld, PTMapChunk& chunk, PTMapSurface&& surface)
	{
		chunk.triangleCount += CountTriangles(surface.surface);
		outWorld.surfaces.push_back(std::move(surface));
	}

	bool IsPortalWall(const walltype* wal)
	{
		if (wal == nullptr)
		{
			return false;
		}

		switch (wal->portalflags)
		{
		case PORTAL_WALL_MIRROR:
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
			return true;
		default:
			return false;
		}
	}

	bool IsPortalPlane(const sectortype* sec, int plane)
	{
		if (sec == nullptr)
		{
			return false;
		}

		if (plane == 0)
		{
			return sec->portalflags == PORTAL_SECTOR_FLOOR || sec->portalflags == PORTAL_SECTOR_FLOOR_REFLECT;
		}

		return sec->portalflags == PORTAL_SECTOR_CEILING || sec->portalflags == PORTAL_SECTOR_CEILING_REFLECT;
	}

	bool IsSkyPlane(const sectortype* sec, int plane)
	{
		if (sec == nullptr)
		{
			return false;
		}

		return plane == 0 ? (sec->floorstat & CSTAT_SECTOR_SKY) != 0 : (sec->ceilingstat & CSTAT_SECTOR_SKY) != 0;
	}

	void FillPlaneProvenance(SurfaceProvenance& provenance, const sectortype* sec, int plane, int sectionIndex, int chunkIndex, uint32_t materialFlags)
	{
		provenance.sourceType = plane == 0 ? SurfaceSourceType::MapFloorSection : SurfaceSourceType::MapCeilingSection;
		provenance.sectorIndex = sec != nullptr ? sector.IndexOf(sec) : -1;
		provenance.sectionIndex = sectionIndex;
		provenance.mapChunkIndex = chunkIndex;
		provenance.drawListType = UINT32_MAX;
		provenance.materialFlags = materialFlags;
		if (sec != nullptr)
		{
			provenance.cstat = plane == 0 ? (uint32_t)sec->floorstat : (uint32_t)sec->ceilingstat;
		}
	}

	void FillWallProvenance(SurfaceProvenance& provenance, const PTWallBandDesc& desc, int chunkIndex)
	{
		provenance.sourceType = desc.sourceType;
		provenance.sectorIndex = desc.frontSector != nullptr ? sector.IndexOf(desc.frontSector) : -1;
		provenance.wallIndex = desc.wall != nullptr ? wall.IndexOf(desc.wall) : -1;
		provenance.sectionIndex = -1;
		provenance.mapChunkIndex = chunkIndex;
		provenance.nextSectorIndex = desc.wall != nullptr ? desc.wall->nextsector : -1;
		provenance.drawListType = UINT32_MAX;
		provenance.materialFlags = desc.materialFlags;
		if (desc.wall != nullptr)
		{
			provenance.cstat = (uint32_t)desc.wall->cstat;
		}
	}

	bool SetWallCoordinates(float topLeft, float topRight, float bottomLeft, float bottomRight, PTWallQuad& outQuad)
	{
		if (topLeft <= bottomLeft && topRight <= bottomRight)
		{
			return false;
		}

		if (topLeft >= bottomLeft)
		{
			outQuad.zTop[0] = topLeft;
			outQuad.zBottom[0] = bottomLeft;
		}
		else
		{
			const float deltaTop = topRight - topLeft;
			const float deltaBottom = bottomRight - bottomLeft;
			const float intersectionX = (bottomLeft - topLeft) / (deltaTop - deltaBottom);
			const float intersectionY = topLeft + intersectionX * deltaTop;

			outQuad.x1 = outQuad.x1 + intersectionX * (outQuad.x2 - outQuad.x1);
			outQuad.y1 = outQuad.y1 + intersectionX * (outQuad.y2 - outQuad.y1);
			outQuad.fracLeft = intersectionX;
			outQuad.zTop[0] = intersectionY;
			outQuad.zBottom[0] = intersectionY;
		}

		if (topRight >= bottomRight)
		{
			outQuad.zTop[1] = topRight;
			outQuad.zBottom[1] = bottomRight;
		}
		else
		{
			const float deltaTop = topRight - topLeft;
			const float deltaBottom = bottomRight - bottomLeft;
			const float intersectionX = (bottomLeft - topLeft) / (deltaTop - deltaBottom);
			const float intersectionY = topLeft + intersectionX * deltaTop;

			outQuad.x2 = outQuad.x1 + intersectionX * (outQuad.x2 - outQuad.x1);
			outQuad.y2 = outQuad.y1 + intersectionX * (outQuad.y2 - outQuad.y1);
			outQuad.fracRight = intersectionX;
			outQuad.zTop[1] = intersectionY;
			outQuad.zBottom[1] = intersectionY;
		}

		return outQuad.zTop[0] > outQuad.zBottom[0] || outQuad.zTop[1] > outQuad.zBottom[1];
	}

	void ComputeWallTexcoords(const PTWallBandDesc& desc, PTWallQuad& quad)
	{
		if (desc.texture == nullptr || desc.wall == nullptr || desc.refWall == nullptr)
		{
			return;
		}

		const bool xFlipped = (desc.wall->cstat & CSTAT_WALL_XFLIP) != 0;
		const float leftDistance = xFlipped ? 1.0f - quad.fracLeft : quad.fracLeft;
		const float rightDistance = xFlipped ? 1.0f - quad.fracRight : quad.fracRight;

		float textureWidth = desc.texture->GetDisplayWidth();
		float textureHeight = desc.texture->GetDisplayHeight();
		if ((desc.wall->cstat & CSTAT_WALL_ROTATE_90) != 0)
		{
			std::swap(textureWidth, textureHeight);
		}

		int pow2Size = 1 << sizeToBits((int)textureHeight);
		if ((float)pow2Size < textureHeight)
		{
			pow2Size *= 2;
		}

		const float yPanning = desc.refWall->ypan_ != 0 ? pow2Size * desc.refWall->ypan_ / (256.0f * textureHeight) : 0.0f;
		quad.texcoords[PTWallTexCoord_LowerLeft].u = quad.texcoords[PTWallTexCoord_UpperLeft].u = ((leftDistance * 8.0f * desc.wall->xrepeat) + desc.refWall->xpan_) / textureWidth;
		quad.texcoords[PTWallTexCoord_LowerRight].u = quad.texcoords[PTWallTexCoord_UpperRight].u = ((rightDistance * 8.0f * desc.wall->xrepeat) + desc.refWall->xpan_) / textureWidth;

		const auto setV = [&](float heightLeft, float heightRight, float fraction)
		{
			float h = heightLeft + (heightRight - heightLeft) * fraction;
			h = (-(float)((desc.referenceHeight + h) * 256) / ((textureHeight * 2048.0f) / (float)std::max<int>((int)desc.wall->yrepeat, 1))) + yPanning;
			if ((desc.refWall->cstat & CSTAT_WALL_YFLIP) != 0)
			{
				h = -h;
			}
			return h;
		};

		quad.texcoords[PTWallTexCoord_UpperLeft].v = setV(desc.topLeft, desc.topRight, quad.fracLeft);
		quad.texcoords[PTWallTexCoord_LowerLeft].v = setV(desc.bottomLeft, desc.bottomRight, quad.fracLeft);
		quad.texcoords[PTWallTexCoord_UpperRight].v = setV(desc.topLeft, desc.topRight, quad.fracRight);
		quad.texcoords[PTWallTexCoord_LowerRight].v = setV(desc.bottomLeft, desc.bottomRight, quad.fracRight);
	}

	void AppendWallVertex(SurfaceRef& surface, float x, float z, float y, const PTWallTexCoord& texcoord)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = x;
		vertex.position[1] = z;
		vertex.position[2] = y;
		vertex.prevPosition[0] = x;
		vertex.prevPosition[1] = z;
		vertex.prevPosition[2] = y;
		vertex.uv[0] = texcoord.u;
		vertex.uv[1] = texcoord.v;
		surface.vertices.push_back(vertex);
	}

	bool BuildWallSurface(const PTWallBandDesc& desc, int chunkIndex, PTMapSurface& outSurface)
	{
		if (desc.wall == nullptr)
		{
			return false;
		}

		PTWallQuad quad = {};
		quad.x1 = (float)desc.wall->pos.X;
		quad.y1 = (float)-desc.wall->pos.Y;
		quad.x2 = (float)desc.wall->point2Wall()->pos.X;
		quad.y2 = (float)-desc.wall->point2Wall()->pos.Y;
		if (!SetWallCoordinates(desc.topLeft, desc.topRight, desc.bottomLeft, desc.bottomRight, quad))
		{
			return false;
		}

		ComputeWallTexcoords(desc, quad);

		outSurface = {};
		outSurface.kind = desc.surfaceKind;
		outSurface.key.primary = desc.wall != nullptr ? (uint32_t)wall.IndexOf(desc.wall) : UINT32_MAX;
		outSurface.key.secondary = (uint32_t)desc.bandKind;
		outSurface.chunkIndex = (uint32_t)chunkIndex;
		outSurface.surface.material = MakeMaterialRef(desc.texture, desc.palette, desc.shade, desc.alpha, desc.materialFlags);
		FillWallProvenance(outSurface.surface.provenance, desc, chunkIndex);
		outSurface.surface.vertices.reserve(4);
		AppendWallVertex(outSurface.surface, quad.x1, quad.zBottom[0], quad.y1, quad.texcoords[PTWallTexCoord_LowerLeft]);
		AppendWallVertex(outSurface.surface, quad.x1, quad.zTop[0], quad.y1, quad.texcoords[PTWallTexCoord_UpperLeft]);
		AppendWallVertex(outSurface.surface, quad.x2, quad.zTop[1], quad.y2, quad.texcoords[PTWallTexCoord_UpperRight]);
		AppendWallVertex(outSurface.surface, quad.x2, quad.zBottom[1], quad.y2, quad.texcoords[PTWallTexCoord_LowerRight]);
		return true;
	}

	void FinalizeSurfaceStats(const PTMapSurface& surface, PTMapWorldStats& stats)
	{
		stats.surfaceCount++;
		stats.triangleCount += CountTriangles(surface.surface);
		switch (surface.kind)
		{
		case PTMapSurfaceKind::Floor:
		case PTMapSurfaceKind::Ceiling:
			stats.flatSurfaceCount++;
			break;
		case PTMapSurfaceKind::Portal:
			stats.portalSurfaceCount++;
			stats.wallSurfaceCount++;
			break;
		default:
			stats.wallSurfaceCount++;
			break;
		}

		if ((surface.surface.material.flags & MaterialFlag_Sky) != 0)
		{
			stats.skySurfaceCount++;
		}
	}

	void BuildPlaneSurface(PTMapWorld& outWorld, PTMapChunk& chunk, sectortype* sec, int sectionIndex, int plane)
	{
		if (sec == nullptr || sectionIndex < 0 || sectionIndex >= (int)sections.Size())
		{
			return;
		}

		FGameTexture* texture = TexMan.GetGameTexture(plane == 0 ? sec->floortexture : sec->ceilingtexture, true);
		if (texture == nullptr || !texture->isValid())
		{
			return;
		}

		// Phase-2 opaque residency excludes sky and sector-portal planes. They
		// need separate PT sky / portal handling instead of entering the opaque BLAS.
		if (IsSkyPlane(sec, plane) || IsPortalPlane(sec, plane))
		{
			return;
		}

		TArray<int>* indices = nullptr;
		auto* mesh = sectionGeometry.get(&sections[sectionIndex], plane, { 0.0f, 0.0f }, &indices);
		if (mesh == nullptr || indices == nullptr || indices->Size() < 3)
		{
			return;
		}

		uint32_t materialFlags = MaterialFlag_Flat;

		PTMapSurface surface = {};
		surface.kind = plane == 0 ? PTMapSurfaceKind::Floor : PTMapSurfaceKind::Ceiling;
		surface.key.primary = (uint32_t)sectionIndex;
		surface.key.secondary = (uint32_t)plane;
		surface.chunkIndex = chunk.chunkIndex;
		surface.surface.material = MakeMaterialRef(texture, plane == 0 ? sec->floorpal : sec->ceilingpal, plane == 0 ? sec->floorshade : sec->ceilingshade, 1.0f, materialFlags);
		FillPlaneProvenance(surface.surface.provenance, sec, plane, sectionIndex, (int)chunk.chunkIndex, surface.surface.material.flags);
		surface.surface.vertices.reserve((uint32_t)indices->Size());

		const float base = -(plane == 0 ? sec->floorz : sec->ceilingz);
		for (unsigned i = 0; i < indices->Size(); ++i)
		{
			const int index = (*indices)[i];
			const auto& point = mesh->vertices[index];
			const auto& uv = mesh->texcoords[index];

			CapturedVertex vertex = {};
			vertex.position[0] = point.X;
			vertex.position[1] = base + point.Z;
			vertex.position[2] = point.Y;
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
			vertex.uv[0] = uv.X;
			vertex.uv[1] = uv.Y;
			surface.surface.vertices.push_back(vertex);
		}

		FinalizeSurfaceStats(surface, outWorld.stats);
		AppendSurface(outWorld, chunk, std::move(surface));
	}

	void TryAppendWallBand(PTMapWorld& outWorld, PTMapChunk& chunk, const PTWallBandDesc& desc)
	{
		PTMapSurface surface = {};
		if (!BuildWallSurface(desc, (int)chunk.chunkIndex, surface))
		{
			return;
		}

		FinalizeSurfaceStats(surface, outWorld.stats);
		AppendSurface(outWorld, chunk, std::move(surface));
	}

	void BuildWallGeometry(PTMapWorld& outWorld, PTMapChunk& chunk, sectortype* frontSector, walltype* wal)
	{
		if (frontSector == nullptr || wal == nullptr)
		{
			return;
		}

		walltype* backWall = wal->twoSided() ? wal->nextWall() : nullptr;
		sectortype* backSector = wal->twoSided() ? wal->nextSector() : nullptr;
		float frontCeilingLeft = 0.0f;
		float frontFloorLeft = 0.0f;
		float frontCeilingRight = 0.0f;
		float frontFloorRight = 0.0f;
		PlanesAtPoint(frontSector, wal->pos.X, wal->pos.Y, &frontCeilingLeft, &frontFloorLeft);
		PlanesAtPoint(frontSector, wal->point2Wall()->pos.X, wal->point2Wall()->pos.Y, &frontCeilingRight, &frontFloorRight);

		if (IsPortalWall(wal))
		{
			PTWallBandDesc portalDesc = {};
			portalDesc.surfaceKind = PTMapSurfaceKind::Portal;
			portalDesc.bandKind = PTWallBandKind::Portal;
			portalDesc.sourceType = SurfaceSourceType::MapPortalSurface;
			portalDesc.wall = wal;
			portalDesc.frontSector = frontSector;
			portalDesc.backSector = backSector;
			portalDesc.topLeft = frontCeilingLeft;
			portalDesc.topRight = frontCeilingRight;
			portalDesc.bottomLeft = frontFloorLeft;
			portalDesc.bottomRight = frontFloorRight;
			portalDesc.materialFlags = (wal->portalflags == PORTAL_WALL_MIRROR) ? MaterialFlag_Mirror : MaterialFlag_Portal;
			TryAppendWallBand(outWorld, chunk, portalDesc);
			return;
		}

		if (backSector == nullptr || backWall == nullptr)
		{
			const FTextureID tileNum = ((wal->cstat & CSTAT_WALL_1WAY) != 0 && wal->nextwall != -1) ? wal->overtexture : wal->walltexture;
			FGameTexture* texture = TexMan.GetGameTexture(tileNum, true);
			if (texture == nullptr || !texture->isValid())
			{
				return;
			}

			float referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->floorz : frontSector->ceilingz;
			PTWallBandDesc desc = {};
			desc.surfaceKind = PTMapSurfaceKind::WallOneSided;
			desc.bandKind = PTWallBandKind::OneSided;
			desc.wall = wal;
			desc.refWall = wal;
			desc.frontSector = frontSector;
			desc.texture = texture;
			desc.shade = wal->shade;
			desc.palette = wal->pal;
			desc.materialFlags = (wal->cstat & CSTAT_WALL_1WAY) != 0 ? MaterialFlag_OneWay : MaterialFlag_None;
			desc.topLeft = frontCeilingLeft;
			desc.topRight = frontCeilingRight;
			desc.bottomLeft = frontFloorLeft;
			desc.bottomRight = frontFloorRight;
			desc.referenceHeight = referenceHeight;
			TryAppendWallBand(outWorld, chunk, desc);
			return;
		}

		float backFloorLeft = 0.0f;
		float backFloorRight = 0.0f;
		float backCeilingLeft = 0.0f;
		float backCeilingRight = 0.0f;
		PlanesAtPoint(backSector, wal->pos.X, wal->pos.Y, &backCeilingLeft, &backFloorLeft);
		PlanesAtPoint(backSector, wal->point2Wall()->pos.X, wal->point2Wall()->pos.Y, &backCeilingRight, &backFloorRight);

		if ((frontSector->ceilingstat & backSector->ceilingstat & CSTAT_SECTOR_SKY) == 0)
		{
			float adjustedBackCeilingLeft = backCeilingLeft;
			float adjustedBackCeilingRight = backCeilingRight;
			if (frontFloorLeft > backCeilingLeft || frontFloorRight > backCeilingRight)
			{
				if ((frontFloorLeft > backCeilingLeft && frontFloorRight > backCeilingRight) || frontSector->portalflags == PORTAL_SECTOR_FLOOR)
				{
					adjustedBackCeilingLeft = frontFloorLeft;
					adjustedBackCeilingRight = frontFloorRight;
				}
			}

			if (adjustedBackCeilingLeft < frontCeilingLeft || adjustedBackCeilingRight < frontCeilingRight)
			{
				FGameTexture* texture = TexMan.GetGameTexture(wal->walltexture, true);
				if (texture != nullptr && texture->isValid())
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallUpper;
					desc.bandKind = PTWallBandKind::Upper;
					desc.wall = wal;
					desc.refWall = wal;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = wal->shade;
					desc.palette = wal->pal;
					desc.topLeft = frontCeilingLeft;
					desc.topRight = frontCeilingRight;
					desc.bottomLeft = adjustedBackCeilingLeft;
					desc.bottomRight = adjustedBackCeilingRight;
					desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->ceilingz;
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}

		if ((wal->cstat & (CSTAT_WALL_MASKED | CSTAT_WALL_1WAY)) != 0)
		{
			FGameTexture* texture = TexMan.GetGameTexture(wal->overtexture, true);
			if (texture != nullptr && texture->isValid())
			{
				float topLeft = 0.0f;
				float topRight = 0.0f;
				float bottomLeft = 0.0f;
				float bottomRight = 0.0f;
				if ((backCeilingLeft - frontCeilingLeft) * (backCeilingRight - frontCeilingRight) >= 0)
				{
					topLeft = std::min(backCeilingLeft, frontCeilingLeft);
					topRight = std::min(backCeilingRight, frontCeilingRight);
				}
				else
				{
					topLeft = backCeilingLeft;
					topRight = backCeilingRight;
				}

				if ((backFloorLeft - frontFloorLeft) * (backFloorRight - frontFloorRight) >= 0)
				{
					bottomLeft = std::max(backFloorLeft, frontFloorLeft);
					bottomRight = std::max(backFloorRight, frontFloorRight);
				}
				else
				{
					bottomLeft = backFloorLeft;
					bottomRight = backFloorRight;
				}

				if (topLeft > bottomLeft || topRight > bottomRight)
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallMiddle;
					desc.bandKind = PTWallBandKind::Middle;
					desc.wall = wal;
					desc.refWall = wal;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = wal->shade;
					desc.palette = wal->pal;
					desc.materialFlags = (wal->cstat & CSTAT_WALL_1WAY) != 0 ? MaterialFlag_OneWay : MaterialFlag_None;
					desc.topLeft = topLeft;
					desc.topRight = topRight;
					desc.bottomLeft = bottomLeft;
					desc.bottomRight = bottomRight;
					if ((wal->cstat & CSTAT_WALL_1WAY) != 0)
					{
						desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->ceilingz;
					}
					else
					{
						desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? std::min(frontSector->floorz, backSector->floorz) : std::max(frontSector->ceilingz, backSector->ceilingz);
					}
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}

		if ((frontSector->floorstat & backSector->floorstat & CSTAT_SECTOR_SKY) == 0)
		{
			float adjustedBackFloorLeft = backFloorLeft;
			float adjustedBackFloorRight = backFloorRight;
			if (frontCeilingLeft < backFloorLeft || frontCeilingRight < backFloorRight)
			{
				if ((frontCeilingLeft < backFloorLeft && frontCeilingRight < backFloorRight) || frontSector->portalflags == PORTAL_SECTOR_CEILING)
				{
					adjustedBackFloorLeft = frontCeilingLeft;
					adjustedBackFloorRight = frontCeilingRight;
				}
			}

			if (adjustedBackFloorLeft > frontFloorLeft || adjustedBackFloorRight > frontFloorRight)
			{
				walltype* referenceWall = (wal->cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 ? backWall : wal;
				FGameTexture* texture = TexMan.GetGameTexture(referenceWall->walltexture, true);
				if (texture != nullptr && texture->isValid())
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallLower;
					desc.bandKind = PTWallBandKind::Lower;
					desc.wall = wal;
					desc.refWall = referenceWall;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = referenceWall->shade;
					desc.palette = referenceWall->pal;
					desc.topLeft = adjustedBackFloorLeft;
					desc.topRight = adjustedBackFloorRight;
					desc.bottomLeft = frontFloorLeft;
					desc.bottomRight = frontFloorRight;
					desc.referenceHeight = (referenceWall->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->floorz;
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}
	}
}

namespace nri_scene
{
void NotifyLevelGeometryReady()
{
	++gPendingLevelGeometryBuildSerial;
}

uint64_t GetPendingLevelGeometryBuildSerial()
{
	return gPendingLevelGeometryBuildSerial;
}

bool BuildMapWorld(PTMapWorld& outWorld)
{
	outWorld.Reset();
	outWorld.level = currentLevel;
	outWorld.buildSerial = gPendingLevelGeometryBuildSerial;

	if (sector.Size() == 0 || sections.Size() == 0 || sectionsPerSector.Size() == 0)
	{
		return false;
	}

	outWorld.stats.sectorCount = (uint32_t)sector.Size();
	outWorld.stats.sectionCount = (uint32_t)sections.Size();
	outWorld.chunks.reserve(sector.Size());
	outWorld.surfaces.reserve((size_t)sections.Size() * 2u + (size_t)wall.Size() * 3u);

	for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
	{
		PTMapChunk chunk = {};
		chunk.kind = PTMapChunkKind::Sector;
		chunk.chunkIndex = (uint32_t)outWorld.chunks.size();
		chunk.sectorIndex = (int32_t)sectorIndex;
		chunk.firstSurface = (uint32_t)outWorld.surfaces.size();

		sectortype* sec = &sector[sectorIndex];
		for (int sectionIndex : sectionsPerSector[sectorIndex])
		{
			BuildPlaneSurface(outWorld, chunk, sec, sectionIndex, 0);
			BuildPlaneSurface(outWorld, chunk, sec, sectionIndex, 1);
		}

		for (auto& wal : sec->walls)
		{
			BuildWallGeometry(outWorld, chunk, sec, &wal);
		}

		chunk.surfaceCount = (uint32_t)outWorld.surfaces.size() - chunk.firstSurface;
		outWorld.chunks.push_back(chunk);
	}

	outWorld.stats.chunkCount = (uint32_t)outWorld.chunks.size();
	outWorld.valid = true;
	return true;
}
}
