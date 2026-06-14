#include "nri_diagnostic_names.h"

#include "../scene/nri_map_world.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_surface_types.h"
#include "hw_drawinfo.h"

namespace nri_diag
{
	const char* GetSceneDataSourceName(uint32_t dataSource)
	{
		switch (dataSource)
		{
		case SceneDataSourceStatic: return "static";
		case SceneDataSourceDynamic: return "dynamic";
		case SceneDataSourcePersistentVoxel: return "persistent_voxel";
		default: return "unknown";
		}
	}

	const char* GetSurfaceProbeSceneOwnerName(uint32_t owner)
	{
		switch (owner)
		{
		case SurfaceProbeOwnerStaticMap: return "static_map";
		case SurfaceProbeOwnerCapturedScene: return "captured_scene";
		case SurfaceProbeOwnerRuntimeLink: return "runtime_link_overlay";
		case SurfaceProbeOwnerRuntimeMutation: return "runtime_mutation_overlay";
		case SurfaceProbeOwnerDynamicOverlay: return "dynamic_overlay";
		default: return "unknown";
		}
	}

	const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall: return "draw_list_wall";
		case nri_scene::SurfaceSourceType::MirrorWall: return "mirror_wall";
		case nri_scene::SurfaceSourceType::FloorFlat: return "floor_flat";
		case nri_scene::SurfaceSourceType::CeilingFlat: return "ceiling_flat";
		case nri_scene::SurfaceSourceType::FacingSprite: return "facing_sprite";
		case nri_scene::SurfaceSourceType::VoxelProxySprite: return "voxel_proxy_sprite";
		case nri_scene::SurfaceSourceType::MapWallBand: return "map_wall_band";
		case nri_scene::SurfaceSourceType::MapFloorSection: return "map_floor_section";
		case nri_scene::SurfaceSourceType::MapCeilingSection: return "map_ceiling_section";
		case nri_scene::SurfaceSourceType::MapPortalSurface: return "map_portal_surface";
		case nri_scene::SurfaceSourceType::DebugSphere: return "debug_sphere";
		case nri_scene::SurfaceSourceType::SurfaceLightOverlay: return "surface_light_overlay";
		default: return "unknown";
		}
	}

	const char* GetMapSurfaceKindName(nri_scene::PTMapSurfaceKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor: return "floor";
		case nri_scene::PTMapSurfaceKind::Ceiling: return "ceiling";
		case nri_scene::PTMapSurfaceKind::WallOneSided: return "wall_one_sided";
		case nri_scene::PTMapSurfaceKind::WallUpper: return "wall_upper";
		case nri_scene::PTMapSurfaceKind::WallMiddle: return "wall_middle";
		case nri_scene::PTMapSurfaceKind::WallLower: return "wall_lower";
		case nri_scene::PTMapSurfaceKind::Portal: return "portal";
		default: return "unknown";
		}
	}

	const char* GetMaterialEmissiveModeName(uint32_t mode)
	{
		switch (mode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: return "base";
		case nri_scene::MaterialEmissiveMode_UseConstantColor: return "constant";
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: return "glowmap";
		default: return "none";
		}
	}

	const char* GetDrawListTypeName(uint32_t drawListType)
	{
		switch (drawListType)
		{
		case GLDL_PLAINWALLS: return "plain_walls";
		case GLDL_MASKEDWALLS: return "masked_walls";
		case GLDL_MASKEDWALLSS: return "masked_walls_split";
		case GLDL_MASKEDWALLSD: return "masked_walls_decal";
		case GLDL_MASKEDWALLSV: return "masked_walls_view";
		case GLDL_MASKEDWALLSH: return "masked_walls_horizon";
		case GLDL_TRANSLUCENTBORDER: return "translucent_border";
		case GLDL_PLAINFLATS: return "plain_flats";
		case GLDL_MASKEDFLATS: return "masked_flats";
		case GLDL_MASKEDSLOPEFLATS: return "masked_slope_flats";
		case GLDL_TRANSLUCENT: return "translucent";
		case GLDL_MODELS: return "models";
		case UINT32_MAX: return "none";
		default: return "unknown";
		}
	}
}
