#include "nri_material_bridge.h"

#include "palette.h"
#include "tiletexture.h"
#include "textures.h"

#include <algorithm>
#include <unordered_map>

EXTERN_CVAR(Int, hw_lightmode)

namespace
{
	using namespace nri_scene;

	enum MaterialClass : uint32_t
	{
		MaterialClass_DefaultDiffuse = 0,
		MaterialClass_UnstableDiffuse = 1,
		MaterialClass_Emissive = 2,
		MaterialClass_SpecularSpecial = 3,
	};

	float ComputeLightLevel(const MaterialRef& material)
	{
		const float shadeDiv = lookups.tables[clamp(material.palette, 0, MAXPALOOKUPS - 1)].ShadeFactor;
		const bool fullbright = (material.flags & MaterialFlag_Fullbright) != 0 || shadeDiv < 1.0f / 1000.0f || material.shade < -numshades;
		if (fullbright)
		{
			return 1.0f;
		}

		float inverseLight = material.shade * 255.0f / numshades;
		inverseLight /= shadeDiv;
		const float lightLevel = clamp(255.0f - inverseLight, 0.0f, 255.0f);
		return lightLevel / 255.0f;
	}

	float ComputeRoughnessHint(const MaterialRef& materialRef, float lightLevel)
	{
		const uint32_t flags = materialRef.flags;
		if ((flags & MaterialFlag_Mirror) != 0)
		{
			return 0.02f;
		}

		if ((flags & MaterialFlag_Portal) != 0)
		{
			return 0.04f;
		}

		if ((flags & MaterialFlag_Fullbright) != 0)
		{
			return 1.0f;
		}

		float roughness = 0.45f;
		if ((flags & MaterialFlag_Sprite) != 0)
		{
			roughness = 0.85f;
		}
		else if ((flags & MaterialFlag_Flat) != 0)
		{
			roughness = 0.65f;
		}
		else if ((flags & MaterialFlag_Indexed) != 0)
		{
			roughness = 0.55f;
		}

		if ((flags & MaterialFlag_OneWay) != 0)
		{
			roughness = std::max(roughness, 0.70f);
		}

		if (materialRef.alpha < 0.999f)
		{
			roughness = std::max(roughness, 0.85f);
		}

		if (lightLevel < 0.25f)
		{
			roughness = std::min(1.0f, roughness + 0.05f);
		}

		return clamp(roughness, 0.02f, 1.0f);
	}

	float ComputeMetalnessHint(const MaterialRef&)
	{
		// Build surfaces are overwhelmingly non-metallic. Keep this explicit and conservative.
		return 0.0f;
	}

	uint32_t ComputeMaterialClass(const MaterialRef& materialRef)
	{
		const uint32_t flags = materialRef.flags;
		if ((flags & MaterialFlag_Fullbright) != 0)
		{
			return MaterialClass_Emissive;
		}

		if ((flags & (MaterialFlag_Mirror | MaterialFlag_Portal)) != 0)
		{
			return MaterialClass_SpecularSpecial;
		}

		if ((flags & (MaterialFlag_Sprite | MaterialFlag_Indexed | MaterialFlag_OneWay)) != 0 || materialRef.alpha < 0.999f)
		{
			return MaterialClass_UnstableDiffuse;
		}

		return MaterialClass_DefaultDiffuse;
	}

	uint64_t Fnv1a64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	uint64_t MakeTextureKey(FGameTexture* texture, bool indexed)
	{
		return (uint64_t)(uintptr_t)texture ^ (indexed ? 1ull : 0ull);
	}

	TextureUpload BuildTextureUpload(FGameTexture* texture, bool indexed)
	{
		TextureUpload upload = {};
		upload.indexed = indexed;
		upload.key = MakeTextureKey(texture, indexed);

		if (texture == nullptr || texture->GetTexture() == nullptr)
		{
			return upload;
		}

		auto* baseTexture = texture->GetTexture();
		if (indexed)
		{
			FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_Indexed);
			if (texBuffer.mBuffer != nullptr && texBuffer.mWidth > 0 && texBuffer.mHeight > 0)
			{
				upload.width = (uint32_t)texBuffer.mWidth;
				upload.height = (uint32_t)texBuffer.mHeight;
				upload.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight);
				upload.key = Fnv1a64(upload.pixels.data(), upload.pixels.size());
			}
		}
		else
		{
			FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
			if (texBuffer.mBuffer != nullptr && texBuffer.mWidth > 0 && texBuffer.mHeight > 0)
			{
				upload.width = (uint32_t)texBuffer.mWidth;
				upload.height = (uint32_t)texBuffer.mHeight;
				upload.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
				upload.key = texBuffer.mContentId != 0 ? texBuffer.mContentId : Fnv1a64(upload.pixels.data(), upload.pixels.size());
			}
		}

		if (upload.width != 0 && upload.height != 0)
		{
			upload.key ^= ((uint64_t)upload.width << 32) | (uint64_t)upload.height;
			upload.key ^= indexed ? (1ull << 63) : 0ull;
		}

		return upload;
	}

	void AppendSurfaceMaterial(const MaterialRef& materialRef, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		MaterialData material = {};
		material.flags = materialRef.flags;
		material.paletteIndex = (uint32_t)clamp(materialRef.palette, 0, MAXPALOOKUPS - 1);
		material.lightLevel = ComputeLightLevel(materialRef);
		material.alpha = materialRef.alpha;
		material.roughnessHint = ComputeRoughnessHint(materialRef, material.lightLevel);
		material.metalnessHint = ComputeMetalnessHint(materialRef);
		material.materialClass = ComputeMaterialClass(materialRef);

		const bool indexed = (materialRef.flags & MaterialFlag_Indexed) != 0;
		const uint64_t textureKey = MakeTextureKey(materialRef.texture, indexed);
		auto it = textureLookup.find(textureKey);
		if (it == textureLookup.end())
		{
			const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
			textureLookup.emplace(textureKey, textureIndex);
			outMaterials.textures.push_back(BuildTextureUpload(materialRef.texture, indexed));
			material.textureIndex = textureIndex;
		}
		else
		{
			material.textureIndex = it->second;
		}

		outMaterials.materials.push_back(material);
	}

	void BuildPaletteLookup(MaterialBridgeData& outMaterials)
	{
		outMaterials.paletteWidth = 256;
		outMaterials.paletteHeight = MAXPALOOKUPS;
		outMaterials.paletteLookup.resize((size_t)outMaterials.paletteWidth * (size_t)outMaterials.paletteHeight * 4u);

		for (uint32_t paletteIndex = 0; paletteIndex < outMaterials.paletteHeight; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			for (uint32_t colorIndex = 0; colorIndex < outMaterials.paletteWidth; ++colorIndex)
			{
				const uint8_t remappedIndex = table != nullptr ? table[colorIndex] : (uint8_t)colorIndex;
				const PalEntry color = GPalette.BaseColors[remappedIndex];
				const size_t pixelIndex = ((size_t)paletteIndex * outMaterials.paletteWidth + colorIndex) * 4u;
				outMaterials.paletteLookup[pixelIndex + 0] = color.b;
				outMaterials.paletteLookup[pixelIndex + 1] = color.g;
				outMaterials.paletteLookup[pixelIndex + 2] = color.r;
				outMaterials.paletteLookup[pixelIndex + 3] = 255;
			}
		}
	}
}

namespace nri_scene
{
void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials)
{
	outMaterials = {};
	std::unordered_map<uint64_t, uint32_t> textureLookup;

	for (const SurfaceRef& wall : sceneView.opaqueWalls)
	{
		AppendSurfaceMaterial(wall.material, textureLookup, outMaterials);
	}

	for (const SurfaceRef& flat : sceneView.opaqueFlats)
	{
		AppendSurfaceMaterial(flat.material, textureLookup, outMaterials);
	}

	for (const SurfaceRef& sprite : sceneView.opaqueSprites)
	{
		AppendSurfaceMaterial(sprite.material, textureLookup, outMaterials);
	}

	BuildPaletteLookup(outMaterials);
}
}
