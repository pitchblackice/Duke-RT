#include "nri_scene_bridge.h"

#include "image.h"
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

	bool IsOpaqueSurface(const HWWall& wall)
	{
		return wall.texture != nullptr &&
			wall.vertcount >= 3 &&
			wall.alpha >= 0.999f &&
			wall.RenderStyle.BlendOp == STYLEOP_Add &&
			wall.RenderStyle.SrcAlpha == STYLEALPHA_One &&
			wall.RenderStyle.DestAlpha == STYLEALPHA_Zero;
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

	void CaptureWalls(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outWalls)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || !IsOpaqueSurface(*wall))
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
			outWalls.push_back(surface);
		}
	}

	void CaptureFlats(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outFlats)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr || !IsOpaqueSurface(*flat))
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
			outFlats.push_back(surface);
		}
	}

	bool IsOpaqueSurface(const HWSprite& sprite)
	{
		return sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			sprite.alpha >= 0.999f &&
			sprite.RenderStyle.BlendOp == STYLEOP_Add &&
			sprite.RenderStyle.SrcAlpha == STYLEALPHA_One &&
			sprite.RenderStyle.DestAlpha == STYLEALPHA_Zero;
	}

	void CaptureSprites(HWDrawInfo& di, HWDrawList& list, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOpaqueSurface(*sprite))
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

			outSprites.push_back(surface);
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

	CaptureWalls(di, di.drawlists[GLDL_PLAINWALLS], outView.opaqueWalls);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLS], outView.opaqueWalls);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], outView.opaqueWalls);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], outView.opaqueWalls);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], outView.opaqueWalls);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], outView.opaqueWalls);

	CaptureFlats(di, di.drawlists[GLDL_PLAINFLATS], outView.opaqueFlats);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDFLATS], outView.opaqueFlats);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], outView.opaqueFlats);

	CaptureSprites(di, di.drawlists[GLDL_TRANSLUCENT], outView.opaqueSprites);

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
