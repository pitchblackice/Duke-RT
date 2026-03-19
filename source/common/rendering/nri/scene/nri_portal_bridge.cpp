#include "nri_portal_bridge.h"

#include "hw_portal.h"
#include "image.h"
#include "textures.h"
#include "v_video.h"

namespace
{
	using namespace nri_scene;

	constexpr int NRI_MAX_PORTAL_CAPTURE_DEPTH = 2;
	thread_local int gPortalCaptureDepth = 0;

	struct CaptureState
	{
		FRenderState* renderState = nullptr;
	};

	void TranslateSurface(SurfaceRef& surface, const float delta[3])
	{
		for (CapturedVertex& vertex : surface.vertices)
		{
			for (int i = 0; i < 3; ++i)
			{
				vertex.position[i] += delta[i];
				vertex.prevPosition[i] += delta[i];
			}
		}
	}

	void AppendTranslatedSurfaces(const std::vector<SurfaceRef>& surfaces, const float delta[3], std::vector<SurfaceRef>& outSurfaces)
	{
		for (const SurfaceRef& source : surfaces)
		{
			SurfaceRef translated = source;
			TranslateSurface(translated, delta);
			outSurfaces.push_back(std::move(translated));
		}
	}

	void AppendTranslatedScene(const SceneView& source, const float delta[3], SceneView& outView)
	{
		AppendTranslatedSurfaces(source.opaqueWalls, delta, outView.opaqueWalls);
		AppendTranslatedSurfaces(source.opaqueFlats, delta, outView.opaqueFlats);
		AppendTranslatedSurfaces(source.opaqueSprites, delta, outView.opaqueSprites);
		outView.stats.portalViews += 1;
		outView.stats.materialRefs += source.stats.materialRefs;
		outView.stats.triangleEstimate += source.stats.triangleEstimate;
		outView.stats.mirrorSurfaces += source.stats.mirrorSurfaces;
		outView.stats.skySurfaces += source.stats.skySurfaces;
		outView.stats.modelDrawItems += source.stats.modelDrawItems;
	}

	void UpdateSkyColor(float* outColor, FGameTexture* texture, PalEntry fallback)
	{
		if (texture == nullptr || texture->GetTexture() == nullptr)
		{
			outColor[0] = fallback.r / 255.0f;
			outColor[1] = fallback.g / 255.0f;
			outColor[2] = fallback.b / 255.0f;
			return;
		}

		FTextureBuffer texBuffer = texture->GetTexture()->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			outColor[0] = fallback.r / 255.0f;
			outColor[1] = fallback.g / 255.0f;
			outColor[2] = fallback.b / 255.0f;
			return;
		}

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
		outColor[0] = (float)(sum[0] * scale);
		outColor[1] = (float)(sum[1] * scale);
		outColor[2] = (float)(sum[2] * scale);
	}

	void MergeSkyFromPortals(HWDrawInfo& di, SceneView& outView)
	{
		for (HWPortal* portal : di.Portals)
		{
			auto* skyPortal = portal != nullptr && portal->IsSky() ? portal : nullptr;
			if (skyPortal == nullptr || skyPortal->lines.Size() == 0)
			{
				continue;
			}

			for (const HWWall& wall : skyPortal->lines)
			{
				if (wall.sky != nullptr)
				{
					UpdateSkyColor(outView.skyColor, wall.sky->texture, wall.sky->fadecolor);
					outView.stats.skySurfaces++;
					return;
				}
			}
		}
	}

	bool ShouldCapturePortal(HWPortal* portal)
	{
		if (portal == nullptr)
		{
			return false;
		}

		switch (portal->GetType())
		{
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
		case PORTAL_SECTOR_FLOOR:
		case PORTAL_SECTOR_CEILING:
			return true;
		default:
			return false;
		}
	}

	void CapturePortalsRecursive(HWDrawInfo& di, SceneView& outView, CaptureState& state)
	{
		if (gPortalCaptureDepth >= NRI_MAX_PORTAL_CAPTURE_DEPTH || state.renderState == nullptr)
		{
			return;
		}

		gPortalCaptureDepth++;
		MergeSkyFromPortals(di, outView);

		for (HWPortal* portal : di.Portals)
		{
			if (!ShouldCapturePortal(portal))
			{
				continue;
			}

			auto* scenePortal = static_cast<HWScenePortalBase*>(portal);
			HWDrawInfo* child = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
			if (child == nullptr)
			{
				continue;
			}

			const bool setup = scenePortal->SetupForSceneCapture(child, *state.renderState);
			if (!setup)
			{
				child->EndDrawInfo();
				continue;
			}

			const bool portalFlag = portal->GetType() == PORTAL_SECTOR_CEILING;
			child->CreateScene(portalFlag);

			SceneView childView;
			if (CaptureScene(*child, childView))
			{
				const float translation[3] = {
					(float)(di.Viewpoint.Pos.X - child->Viewpoint.Pos.X),
					(float)(di.Viewpoint.Pos.Z - child->Viewpoint.Pos.Z),
					(float)(di.Viewpoint.Pos.Y - child->Viewpoint.Pos.Y)
				};

				AppendTranslatedScene(childView, translation, outView);
			}

			scenePortal->ShutdownAfterSceneCapture(child, *state.renderState);
			child->EndDrawInfo();
		}

		gPortalCaptureDepth--;
	}
}

namespace nri_scene
{
void CapturePortalViews(HWDrawInfo& di, SceneView& outView)
{
	CaptureState state = {};
	state.renderState = screen != nullptr ? screen->RenderState() : nullptr;
	CapturePortalsRecursive(di, outView, state);
}
}
