#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "assetsModule/modelModule/Material.h"
#include "componentsModule/WindComponent.h"
#include "ecss/Types.h"
#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::ComponentsModule {
	constexpr int MaxMaterialTexturesCount = 16;
	static_assert(MaxMaterialTexturesCount >= SFE::USER3, "a slot each, DIFFUSE..USER3");
	inline constexpr const char* DefaultGBufferVS = "shaders/g_buffer.vs";
	inline constexpr const char* DefaultGBufferFS = "shaders/g_buffer.fs";
	inline constexpr const char* DefaultGBufferMaskedFS = "shaders/g_buffer_masked.fs";
	// DefaultGBufferFS without its cut-out path, for a material with nothing to discard; GpuScene picks it.
	inline constexpr const char* DefaultGBufferSolidFS = "shaders/g_buffer_solid.fs";
	// A transparent material's, unless it names one of its own (GlassPass).
	inline constexpr const char* DefaultGlassFS = "shaders/glass.fs";
	// How a material is drawn into the G-buffer (MaterialRules::drawClass): Solid, Cutout where its shader discards by its
	// alpha (the cut-out turn, after the solid surfaces), Ground the ground (last, and under the dense patch).
	enum class DrawClass : uint8_t { Solid, Cutout, Ground };

	struct MaterialData {
		int slot = 0;
		Render::TextureHandle texture;
		int uvSet = 0;
	};

	struct Materials {
		void addMaterial(MaterialData data) {
			if (materialsCount < MaxMaterialTexturesCount) {
				material[materialsCount++] = std::move(data);
			}
		}
		void clearMaterials() {
			materialsCount = 0;
		}

		MaterialData material[MaxMaterialTexturesCount];
		uint8_t materialsCount = 0;
	};

	struct MaterialComponent {
		Materials materials;
		// The model's material it was made from (SFE::Material::name), empty for one made otherwise.
		std::string name;
		std::string vertexShader = DefaultGBufferVS;
		std::string fragmentShader = DefaultGBufferFS;
		// Drawn over the lit scene, blended (GlassPass): as glass, or by a fragment shader of its own when it names one
		// (with sfe_glass.glsl's inputs, not the G-buffer's).
		bool transparent = false;
		// Transparent, and seen as one surface: its nearest only, none of it through the rest of it -- a hand's fingers
		// not through its palm (GlassPass lays its depth first).
		bool solidGlass = false;
		bool alphaFromMaps = false;
		bool alphaDither = false;
		float opacity = 1.f;
		float ior = 1.5f;
		float roughness = 0.5f;
		float metallic = 0.f;
		float alphaCutoff = 0.f;
		float specFactor = 1.f;
		float clearcoat = 0.f;
		float clearcoatRough = 0.04f;
		// Multiplies the albedo; glass takes it as the colour it filters light through.
		Math::Vec3 tint{ 1.f, 1.f, 1.f };
		// The light it gives off by itself (a lamp's glass, a lit screen), times its EMISSIVE texture: added to the lit
		// colour, never shadowed. None by default.
		Math::Vec3 emissive{ 0.f, 0.f, 0.f };
		Sway sway = Sway::None;
		// Not drawn at all: a part of a model switched off, such as a hairstyle nobody wears.
		bool hidden = false;
		// Not drawn, but its shadow is: a head the first person camera is inside.
		bool shadowOnly = false;
		// Drawn, but casting no shadow: a flame.
		bool castsShadow = true;
		// A ground seen from above (DrawClass::Ground): drawn after everything else, its back faces culled, its own mesh put
		// out of sight under the dense patch near the camera (TerrainPatch) and a level coarser past
		// GraphicsSettings::terrainLodDistance. Set by whoever makes the ground's material.
		bool ground = false;
		// The application's own numbers for its shaders, eight a material (SfeMaterial::user): the engine passes them on and
		// never reads them; what each lane means is the shader's that draws the material.
		std::array<Math::Vec4, 2> user{};
		// How much of its cut-out surface is gone, 0 none to 1 all: each triangle (a leaf) goes at its own point on the way
		// (sfePieceKey), and what of a gone piece stays, and in what colour, is the application's (sfe_user_dissolve.glsl).
		float dissolve = 0.f;
		// Where the texture coordinates fall inside `uv` (u min, v min, u max, v max), the surface takes the
		// TRIPLANAR_ALBEDO and TRIPLANAR_NORMALS textures instead (the second: TRIPLANAR2_*), projected along the
		// model's axes, a repeat every `metres`: bark on a trunk, or stone and moss on a rock, whose coordinates only
		// pick colours out of a palette. `metres` 0 for none.
		struct Triplanar {
			float uv[4]{};
			float metres = 0.f;
			// The texture's u and v swapped: bark whose grain should run along a log lying down instead of up a trunk.
			bool rotated = false;
		};
		static constexpr size_t kTriplanarLayers = 2;
		Triplanar triplanar[kTriplanarLayers];
	};

	inline MaterialComponent fromAssetMaterial(const SFE::Material& src) {
		MaterialComponent out;
		out.name = src.name;
		out.transparent = src.transparent;
		out.alphaFromMaps = src.alphaFromMaps;
		out.alphaDither = src.alphaDither;
		out.opacity = src.opacity;
		out.ior = src.ior;
		out.roughness = src.roughness;
		out.metallic = src.metallic;
		out.alphaCutoff = src.alphaCutoff;
		out.specFactor = src.specFactor;
		out.clearcoat = src.clearcoat;
		out.clearcoatRough = src.clearcoatRough;
		out.tint = src.baseColor;
		out.emissive = src.emissive;
		if (src.alphaCutout) {
			out.fragmentShader = DefaultGBufferMaskedFS;
		}
		if (!src.fragmentShader.empty()) {
			out.fragmentShader = src.fragmentShader;
		}
		out.castsShadow = src.castsShadow;
		for (const auto& [type, tex] : src.materialTextures) {
			if (!tex.texture) {
				continue;
			}
			out.materials.addMaterial({ tex.uniformSlot, tex.texture, tex.uvSet });
		}
		return out;
	}
}

template<> struct ecss::AllowNonTrivial<SFE::ComponentsModule::MaterialComponent> : std::true_type {};

using SFE::ComponentsModule::MaterialComponent;
