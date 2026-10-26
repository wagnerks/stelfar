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
		OCCLUSION,
		// The glTF emissiveTexture: what the surface gives off by itself, times Material::emissive.
		// Kept last: the material binding loops run DIFFUSE..EMISSIVE.
		EMISSIVE
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
		// The material's name in the file it was loaded from: what a game finds a part of a model by (a lamp's glass).
		std::string name;
		// A fragment shader of its own (a content path), empty for the engine's own.
		std::string fragmentShader;
		bool castsShadow = true;
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
		// glTF emissiveFactor times its emissive strength: the light the surface gives off, in the lighting's units, times
		// the emissive texture where there is one. None by default.
		Math::Vec3 emissive{ 0.f, 0.f, 0.f };
	};
}
