#include "nri_scene_bridge.h"

#include "image.h"
#include "textures.h"
#include "v_video.h"

namespace
{
	using namespace nri_scene;

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
			surface.vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			surface.vertexCount = wall->vertcount;
			surface.material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, MaterialFlag_None);
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
			surface.vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			surface.vertexCount = (uint32_t)flat->vertcount;
			surface.material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat);
			outFlats.push_back(surface);
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

	stats.spriteDrawItems = CountDrawListItems(di, GLDL_MODELS);
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
	CaptureFlats(di, di.drawlists[GLDL_PLAINFLATS], outView.opaqueFlats);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += wall.vertexCount >= 3 ? wall.vertexCount - 2 : 0;
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += flat.vertexCount / 3;
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty();
}
}
