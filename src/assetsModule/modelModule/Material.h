#pragma once
#include <string>
#include <unordered_map>
#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE {
	enum MaterialType : uint8_t {
		CUSTOM = 0,
		DIFFUSE = 1,
		NORMALS,
		SPECULAR,
		ROUGHNESS,
		METALLIC,
		SUBSURFACE,
		// Textures laid over parts of the surface by where they are rather than by their texture coordinates; see
		// MaterialComponent::triplanar.
		TRIPLANAR_ALBEDO,
		TRIPLANAR_NORMALS,
		TRIPLANAR2_ALBEDO,
		TRIPLANAR2_NORMALS,
		// The glTF occlusionTexture: its red channel darkens only the ambient and indirect light, never the sun or the lamps.
		// Kept last: the material binding loops run DIFFUSE..OCCLUSION.
		OCCLUSION
	};

	struct MaterialTexture {
		Render::TextureHandle texture;
		int uniformSlot = 0;
		MaterialType type = MaterialType::CUSTOM;
		int uvSet = 0;
		// The file it was read from; empty for one made in memory.
		std::string path;
	};

	struct Material {
		MaterialTexture& operator[](MaterialType type) { return materialTextures[type]; }
		MaterialTexture* tryGetTexture(MaterialType type) {
			const auto it = materialTextures.find(type);

			if (it != materialTextures.cend()) {
				return &it->second;
			}

			return nullptr;
		}

		std::unordered_map<MaterialType, MaterialTexture> materialTextures;
		bool transparent = false;
		bool alphaCutout = false;
		bool alphaFromMaps = false;
		bool alphaDither = false;
		bool hasDiffuseAlpha = false;
		bool gltfPbr = false;
		float opacity = 1.f;
		float ior = 1.5f;
		float roughness = 0.5f;
		float metallic = 0.f;
		float alphaCutoff = 0.f;
		float specFactor = 1.f;
		float clearcoat = 0.f;
		float clearcoatRough = 0.04f;
		// glTF baseColorFactor: multiplies the albedo texture, or is the albedo where there is none.
		Math::Vec3 baseColor{ 1.f, 1.f, 1.f };
	};
}
