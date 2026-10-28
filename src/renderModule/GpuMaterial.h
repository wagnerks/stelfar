#pragma once

#include <cstddef>

namespace SFE::Render {
	// Mirrors SfeMaterial in shaders/sfe_material_table.glsl.
	struct GpuMaterial {
		float roughness = 0.5f;
		float metallic = 0.f;
		float mapMask = 0.f;
		float alphaCutoff = 0.f;
		float uvSel = 0.f;
		float specFactor = 1.f;
		float clearcoat = 0.f;
		float clearcoatRough = 0.04f;
		float tint[3]{ 1.f, 1.f, 1.f };
		float opacity = 0.22f;
		float ior = 1.52f;
		float casterOpacity = 1.f;
		float shadowCutoff = 0.f;
		float albedoSlot = 0.f; // glass: index into glassTextures()
		float triplanarUv[4]{};
		float triplanarUv2[4]{};
		float triplanarRepeat[2]{}; // repeats a metre, per layer; 0 for none
		float triplanarRotated = 0.f; // bit 1 the first layer's u and v swapped, bit 2 the second's
		float dissolve = 0.f; // 108: MaterialComponent::dissolve
		float emissive[3]{}; // 112: the light it gives off, times its EMISSIVE texture
		float _pad0 = 0.f; // 124: std430 starts user[] on 16 bytes
		float user[8]{}; // 128: MaterialComponent::user, two vec4
	};
	static_assert(sizeof(GpuMaterial) == 160, "must match the std430 SfeMaterial (sfe_material_table.glsl)");
	static_assert(offsetof(GpuMaterial, user) == 128, "user[] is vec4-aligned in std430");
}
