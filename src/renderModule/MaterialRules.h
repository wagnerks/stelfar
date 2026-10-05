#pragma once

#include <functional>
#include <string>

#include "componentsModule/MaterialComponent.h"

// How a material's fields become shader inputs. One place for the rules, used by the GPU material
// table and the draw lists.
namespace SFE::Render::MaterialRules {
	inline float cutoutCutoff(const float alphaCutoff, const bool alphaFromMaps, const std::string& fragmentShader) {
		if (alphaCutoff > 0.f) {
			return alphaCutoff;
		}
		if (alphaFromMaps) {
			return 0.18f;
		}
		if (fragmentShader == ComponentsModule::DefaultGBufferMaskedFS) {
			return 0.4f;
		}
		return 0.f;
	}

	// The fragment shader a transparent material is drawn with: its own, or glass when it keeps a G-buffer default.
	inline std::string glassShader(const ComponentsModule::MaterialComponent& material) {
		const auto& own = material.fragmentShader;
		const bool gBuffer = own.empty() || own == ComponentsModule::DefaultGBufferFS || own == ComponentsModule::DefaultGBufferMaskedFS
			|| own == ComponentsModule::DefaultGBufferSolidFS;
		return gBuffer ? ComponentsModule::DefaultGlassFS : own;
	}

	inline TextureHandle diffuseTexture(const ComponentsModule::Materials& materials) {
		for (uint8_t i = 0; i < materials.materialsCount; ++i) {
			if (materials.material[i].slot == SFE::DIFFUSE) {
				return materials.material[i].texture;
			}
		}
		return {};
	}

	inline TextureHandle maskTexture(const ComponentsModule::Materials& materials) {
		TextureHandle spec{};
		TextureHandle rough{};
		for (uint8_t i = 0; i < materials.materialsCount; ++i) {
			const auto& slot = materials.material[i];
			if (slot.slot == SFE::ROUGHNESS) {
				rough = slot.texture;
			}
			else if (slot.slot == SFE::SPECULAR) {
				spec = slot.texture;
			}
		}
		if (rough) {
			return rough;
		}
		if (spec) {
			return spec;
		}
		return diffuseTexture(materials);
	}

	inline float gBufferMapMask(const ComponentsModule::Materials& materials, const bool alphaFromMaps) {
		float mask = 0.f;
		for (uint8_t i = 0; i < materials.materialsCount; ++i) {
			const int slot = materials.material[i].slot;
			if (slot == SFE::ROUGHNESS) {
				mask += 1.f;
			}
			else if (slot == SFE::METALLIC) {
				mask += 2.f;
			}
			else if (slot == SFE::SPECULAR) {
				mask += 4.f;
			}
		}
		if (alphaFromMaps) {
			mask += 8.f;
		}
		// The albedo comes from a texture, so a skin tone in it may be taken for skin. A flat material colour never
		// is: an orange mannequin is not a person.
		if (diffuseTexture(materials)) {
			mask += 16.f;
		}
		return mask;
	}

	inline float gBufferUvSel(const ComponentsModule::Materials& materials) {
		float sel = 0.f;
		for (uint8_t i = 0; i < materials.materialsCount; ++i) {
			if (materials.material[i].uvSet < 1) {
				continue;
			}
			switch (materials.material[i].slot) {
			case SFE::DIFFUSE: sel += 1.f; break;
			case SFE::NORMALS: sel += 2.f; break;
			case SFE::SPECULAR: sel += 4.f; break;
			case SFE::ROUGHNESS: sel += 8.f; break;
			case SFE::METALLIC: sel += 16.f; break;
			case SFE::SUBSURFACE: sel += 32.f; break;
			case SFE::OCCLUSION: sel += 64.f; break;
			default: break;
			}
		}
		return sel;
	}

	// Which textures a draw binds, independent of the scalar parameters.
	inline uint64_t textureKey(const ComponentsModule::Materials& materials) {
		uint64_t h = 1469598103934665603ull;
		const auto mix = [&](const uint64_t v) {
			h ^= v;
			h *= 1099511628211ull;
		};
		mix(materials.materialsCount);
		for (uint8_t i = 0; i < materials.materialsCount; ++i) {
			const auto& slot = materials.material[i];
			mix((static_cast<uint64_t>(slot.slot) << 32) | slot.texture.id);
		}
		return h;
	}

	inline uint64_t materialHash(const ComponentsModule::MaterialComponent& material) {
		uint64_t h = 1469598103934665603ull;
		const auto mix = [&](const uint64_t v) {
			h ^= v;
			h *= 1099511628211ull;
		};
		mix(material.transparent ? 1ull : 0ull);
		mix(material.alphaFromMaps ? 1ull : 0ull);
		mix(material.alphaDither ? 1ull : 0ull);
		mix(static_cast<uint64_t>(material.opacity * 4096.f));
		mix(static_cast<uint64_t>(material.ior * 4096.f));
		mix(static_cast<uint64_t>(material.roughness * 4096.f));
		mix(static_cast<uint64_t>(material.metallic * 4096.f));
		mix(static_cast<uint64_t>(material.alphaCutoff * 4096.f));
		mix(static_cast<uint64_t>(material.specFactor * 4096.f));
		mix(static_cast<uint64_t>(material.clearcoat * 4096.f));
		mix(static_cast<uint64_t>(material.clearcoatRough * 4096.f));
		mix(static_cast<uint64_t>(material.tint.x * 4096.f));
		mix(static_cast<uint64_t>(material.tint.y * 4096.f));
		mix(static_cast<uint64_t>(material.tint.z * 4096.f));
		mix(static_cast<uint64_t>(material.sway));
		mix(static_cast<uint64_t>(static_cast<int64_t>(material.wetBelow * 4096.f)));
		mix(static_cast<uint64_t>(material.bare * 4096.f));
		for (const auto& layer : material.triplanar) {
			for (const float bound : layer.uv) {
				mix(static_cast<uint64_t>(bound * 4096.f));
			}
			mix(static_cast<uint64_t>(layer.metres * 4096.f));
			mix(layer.rotated ? 1ull : 0ull);
		}
		mix(material.materials.materialsCount);
		for (uint8_t i = 0; i < material.materials.materialsCount; ++i) {
			const auto& slot = material.materials.material[i];
			mix((static_cast<uint64_t>(slot.slot) << 32) | slot.texture.id);
			mix(static_cast<uint64_t>(slot.uvSet));
		}
		if (material.vertexShader != ComponentsModule::DefaultGBufferVS) {
			mix(std::hash<std::string>{}(material.vertexShader));
		}
		if (material.fragmentShader != ComponentsModule::DefaultGBufferFS) {
			mix(std::hash<std::string>{}(material.fragmentShader));
		}
		return h;
	}
}
