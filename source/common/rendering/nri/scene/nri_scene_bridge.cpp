#include "nri_scene_bridge.h"

#include "nri_portal_bridge.h"

#include "hw_portal.h"
#include "hw_voxels.h"
#include "image.h"
#include "model_kvx.h"
#include "texturemanager.h"
#include "textures.h"
#include "v_video.h"

namespace
{
	using namespace nri_scene;

	CapturedVertex MakeCapturedVertex(const FFlatVertex& source)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = source.x;
		vertex.position[1] = source.z;
		vertex.position[2] = source.y;
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = source.u;
		vertex.uv[1] = source.v;
		return vertex;
	}

	unsigned int CountDrawListItems(HWDrawInfo& di, DrawListType type)
	{
		return di.drawlists[type].Size();
	}

	void ApplyActorPreviousTransform(SurfaceRef& surface, DCoreActor* actor)
	{
		if (actor == nullptr)
		{
			return;
		}

		const DVector3 worldDelta = actor->spr.pos - actor->opos;
		const float renderDelta[3] = {
			(float)worldDelta.X,
			(float)-worldDelta.Z,
			(float)-worldDelta.Y
		};

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.prevPosition[0] = vertex.position[0] - renderDelta[0];
			vertex.prevPosition[1] = vertex.position[1] - renderDelta[1];
			vertex.prevPosition[2] = vertex.position[2] - renderDelta[2];
		}
	}

	bool IsOpaqueSurface(const HWWall& wall)
	{
		return wall.texture != nullptr &&
			wall.vertcount >= 3 &&
			wall.alpha >= 0.999f &&
			wall.RenderStyle.BlendOp == STYLEOP_Add &&
			wall.RenderStyle.SrcAlpha == STYLEALPHA_One &&
			wall.RenderStyle.DestAlpha == STYLEALPHA_Zero;
	}

	bool IsSkyWall(const HWWall& wall)
	{
		return (wall.flags & HWWall::HWF_SKYHACK) != 0;
	}

	bool IsPortalSourceWall(const HWWall& wall)
	{
		if (wall.seg == nullptr)
		{
			return false;
		}

		switch (wall.seg->portalflags)
		{
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
			return true;
		default:
			return false;
		}
	}

	bool IsOpaqueSurface(const HWFlat& flat)
	{
		return flat.texture != nullptr &&
			flat.vertcount >= 3 &&
			flat.alpha >= 0.999f &&
			flat.Sprite == nullptr &&
			flat.RenderStyle.BlendOp == STYLEOP_Add &&
			flat.RenderStyle.SrcAlpha == STYLEALPHA_One &&
			flat.RenderStyle.DestAlpha == STYLEALPHA_Zero;
	}

	bool IsSkyFlat(const HWFlat& flat)
	{
		if (flat.sec == nullptr)
		{
			return false;
		}

		if (flat.plane == plane_ceiling)
		{
			return (flat.sec->ceilingstat & CSTAT_SECTOR_SKY) != 0;
		}

		return (flat.sec->floorstat & CSTAT_SECTOR_SKY) != 0;
	}

	bool IsPortalSourceFlat(const HWFlat& flat)
	{
		if (flat.stack || flat.sec == nullptr)
		{
			return true;
		}

		const int flags = flat.sec->portalflags;
		if (flat.plane == plane_ceiling)
		{
			return flags == PORTAL_SECTOR_CEILING || flags == PORTAL_SECTOR_CEILING_REFLECT;
		}

		return flags == PORTAL_SECTOR_FLOOR || flags == PORTAL_SECTOR_FLOOR_REFLECT;
	}

	MaterialRef MakeMaterialRef(FGameTexture* texture, int palette, int shade, float alpha, uint32_t extraFlags)
	{
		MaterialRef material = {};
		material.texture = texture;
		material.palette = palette;
		material.shade = shade;
		material.alpha = alpha;
		material.flags = extraFlags;

		if (texture != nullptr)
		{
			auto* baseTexture = texture->GetTexture();
			if (baseTexture != nullptr && baseTexture->GetImage() != nullptr && baseTexture->GetImage()->UseGamePalette())
			{
				material.flags |= MaterialFlag_Indexed;
			}

			if (texture->isFullbright())
			{
				material.flags |= MaterialFlag_Fullbright;
			}
		}

		return material;
	}

	void CaptureWalls(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || !IsOpaqueSurface(*wall))
			{
				continue;
			}

			if (IsSkyWall(*wall))
			{
				stats.skySurfaces++;
				continue;
			}

			if (IsPortalSourceWall(*wall))
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, MaterialFlag_None);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			surface.vertices.reserve(wall->vertcount);
			for (uint32_t i = 0; i < wall->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (wall->Sprite != nullptr && wall->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, wall->Sprite->ownerActor);
			}

			outWalls.push_back(std::move(surface));
		}
	}

	void CaptureMirrorBorders(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || wall->type != RENDERWALL_MIRRORSURFACE)
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, MaterialFlag_Mirror);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			surface.vertices.reserve(wall->vertcount);
			for (uint32_t i = 0; i < wall->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			outWalls.push_back(std::move(surface));
			stats.mirrorSurfaces++;
		}
	}

	void CaptureFlats(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outFlats, SceneDebugStats& stats, SceneView& outView)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr || !IsOpaqueSurface(*flat))
			{
				continue;
			}

			if (IsSkyFlat(*flat))
			{
				stats.skySurfaces++;
				if (flat->texture != nullptr)
				{
					FTextureBuffer texBuffer = flat->texture->GetTexture()->CreateTexBuffer(0, CTF_ProcessData);
					if (texBuffer.mBuffer != nullptr && texBuffer.mWidth > 0 && texBuffer.mHeight > 0)
					{
						double sum[3] = {};
						const size_t pixelCount = (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight;
						for (size_t i = 0; i < pixelCount; ++i)
						{
							const uint8_t* pixel = texBuffer.mBuffer + i * 4u;
							sum[0] += pixel[2];
							sum[1] += pixel[1];
							sum[2] += pixel[0];
						}

						const double scale = pixelCount > 0 ? 1.0 / (255.0 * (double)pixelCount) : 0.0;
						outView.skyColor[0] = (float)(sum[0] * scale);
						outView.skyColor[1] = (float)(sum[1] * scale);
						outView.skyColor[2] = (float)(sum[2] * scale);
					}
				}
				continue;
			}

			if (IsPortalSourceFlat(*flat))
			{
				continue;
			}

			flat->MakeVertices(&di);
			if (flat->vertcount < 3)
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			surface.vertices.reserve((uint32_t)flat->vertcount);
			for (int i = 0; i < flat->vertcount; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (flat->Sprite != nullptr && flat->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, flat->Sprite->ownerActor);
			}

			outFlats.push_back(std::move(surface));
		}
	}

	bool IsOpaqueSprite(const HWSprite& sprite)
	{
		return sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			sprite.alpha >= 0.999f &&
			sprite.RenderStyle.BlendOp == STYLEOP_Add &&
			sprite.RenderStyle.SrcAlpha == STYLEALPHA_One &&
			sprite.RenderStyle.DestAlpha == STYLEALPHA_Zero;
	}

	void CaptureFacingSprites(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOpaqueSprite(*sprite))
			{
				continue;
			}

			if (sprite->vertexindex < 0)
			{
				sprite->CreateVertices(&di);
			}

			if (sprite->vertexindex < 0)
			{
				continue;
			}

			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(sprite->vertexindex);
			if (vertices == nullptr)
			{
				continue;
			}

			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, MaterialFlag_Sprite);
			surface.vertices.reserve(4);
			for (uint32_t i = 0; i < 4; ++i)
			{
				surface.vertices.push_back(MakeCapturedVertex(vertices[i]));
			}

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
			}

			outSprites.push_back(std::move(surface));
		}
	}

	void TransformModelPoint(const VSMatrix& matrix, float x, float y, float z, CapturedVertex& outVertex, float u, float v)
	{
		float point[4] = { x, y, z, 1.0f };
		float transformed[4] = {};
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, transformed);

		outVertex.position[0] = transformed[0];
		outVertex.position[1] = transformed[1];
		outVertex.position[2] = transformed[2];
		outVertex.prevPosition[0] = transformed[0];
		outVertex.prevPosition[1] = transformed[1];
		outVertex.prevPosition[2] = transformed[2];
		outVertex.uv[0] = u;
		outVertex.uv[1] = v;
	}

	void AddVoxelFace(const VSMatrix& matrix, const float* extents, const int* indices, SurfaceRef& outSurface)
	{
		static const float corners[8][3] = {
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 1.0f, 1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 1.0f, 0.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 1.0f, 1.0f },
		};

		static const float uvs[4][2] = {
			{ 0.0f, 0.0f },
			{ 1.0f, 0.0f },
			{ 1.0f, 1.0f },
			{ 0.0f, 1.0f },
		};

		for (int i = 0; i < 4; ++i)
		{
			const float* local = corners[indices[i]];
			CapturedVertex vertex = {};
			TransformModelPoint(matrix, local[0] * extents[0], local[1] * extents[1], local[2] * extents[2], vertex, uvs[i][0], uvs[i][1]);
			outSurface.vertices.push_back(vertex);
		}
	}

	void CaptureModelSprites(HWDrawList& list, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats)
	{
		static const int faces[6][4] = {
			{ 0, 1, 2, 3 },
			{ 4, 5, 6, 7 },
			{ 0, 4, 7, 3 },
			{ 1, 5, 6, 2 },
			{ 3, 2, 6, 7 },
			{ 0, 1, 5, 4 },
		};

		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr)
			{
				continue;
			}

			stats.modelDrawItems++;

			if (sprite->modelframe > 0)
			{
				stats.unsupportedModelDrawItems++;
				continue;
			}

			if (sprite->modelframe >= 0 || sprite->voxel == nullptr || sprite->voxel->model == nullptr)
			{
				continue;
			}

			FGameTexture* voxelTexture = TexMan.GetGameTexture(sprite->voxel->model->GetPaletteTexture());
			if (voxelTexture == nullptr || !voxelTexture->isValid())
			{
				continue;
			}

			const float extents[3] = {
				(float)sprite->voxel->siz.X,
				(float)sprite->voxel->siz.Z,
				(float)sprite->voxel->siz.Y
			};

			for (const auto& face : faces)
			{
				SurfaceRef surface = {};
				surface.material = MakeMaterialRef(voxelTexture, sprite->palette, sprite->shade, sprite->alpha, MaterialFlag_Sprite);
				surface.vertices.reserve(4);
				AddVoxelFace(sprite->rotmat, extents, face, surface);
				if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
				{
					ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				}
				outSprites.push_back(std::move(surface));
			}

			stats.voxelProxyDrawItems++;
		}
	}
}

namespace nri_scene
{
SceneDebugStats CollectDebugStats(HWDrawInfo& di)
{
	SceneDebugStats stats = {};

	stats.wallDrawItems =
		CountDrawListItems(di, GLDL_PLAINWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSD) +
		CountDrawListItems(di, GLDL_MASKEDWALLSV) +
		CountDrawListItems(di, GLDL_MASKEDWALLSH) +
		CountDrawListItems(di, GLDL_TRANSLUCENTBORDER);

	stats.flatDrawItems =
		CountDrawListItems(di, GLDL_PLAINFLATS) +
		CountDrawListItems(di, GLDL_MASKEDFLATS) +
		CountDrawListItems(di, GLDL_MASKEDSLOPEFLATS);

	stats.spriteDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT) + CountDrawListItems(di, GLDL_MODELS);
	stats.modelDrawItems = CountDrawListItems(di, GLDL_MODELS);
	stats.translucentDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT);
	stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems + stats.translucentDrawItems;
	stats.triangleEstimate = 0;
	stats.materialRefs = 0;
	return stats;
}

bool CaptureScene(HWDrawInfo& di, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
	outView.stats = CollectDebugStats(di);

	CaptureWalls(di, di.drawlists[GLDL_PLAINWALLS], outView.opaqueWalls, outView.stats);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLS], outView.opaqueWalls, outView.stats);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], outView.opaqueWalls, outView.stats);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], outView.opaqueWalls, outView.stats);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], outView.opaqueWalls, outView.stats);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], outView.opaqueWalls, outView.stats);
	CaptureMirrorBorders(di, di.drawlists[GLDL_TRANSLUCENTBORDER], outView.opaqueWalls, outView.stats);

	CaptureFlats(di, di.drawlists[GLDL_PLAINFLATS], outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDFLATS], outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], outView.opaqueFlats, outView.stats, outView);

	CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], outView.opaqueSprites);
	CaptureModelSprites(di.drawlists[GLDL_MODELS], outView.opaqueSprites, outView.stats);
	CapturePortalViews(di, outView);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += wall.vertices.size() >= 3 ? (unsigned int)wall.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += (unsigned int)(flat.vertices.size() / 3);
		outView.stats.materialRefs++;
	}

	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += sprite.vertices.size() >= 3 ? (unsigned int)sprite.vertices.size() - 2 : 0;
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}
}
