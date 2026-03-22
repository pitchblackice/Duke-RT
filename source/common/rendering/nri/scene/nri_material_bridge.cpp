#include "nri_material_bridge.h"

#include "palette.h"
#include "tiletexture.h"
#include "textures.h"

#include <unordered_map>

EXTERN_CVAR(Int, hw_lightmode)

namespace
{
	using namespace nri_scene;

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
