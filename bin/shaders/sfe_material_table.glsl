#ifndef SFE_MATERIAL_TABLE_GLSL
#define SFE_MATERIAL_TABLE_GLSL
// The material table alone, with nothing only a fragment shader may use: vertex shaders include this.

// Material parameters, one entry per distinct material. Mirrors SFE::Render::GpuMaterial; the pass
// rules (dither cutoff, caster opacity, glass defaults) are resolved on the CPU when an entry is made.
struct SfeMaterial {
    float matRoughness;
    float matMetallic;
    float mapMask;
    float alphaCutoff;     // g-buffer: > 1 dithers
    float uvSel;
    float specFactor;
    float clearcoat;
    float clearcoatRough;
    vec3 tint;
    float opacity;         // glass density
    float ior;             // glass
    float casterOpacity;   // shadow casters
    float shadowCutoff;    // shadow casters: < 0 takes the mask from r/g
    float albedoSlot;      // glass: index into the glass albedo array
    vec4 triplanarUv;      // u min, v min, u max, v max; see MaterialComponent::triplanar
    vec4 triplanarUv2;
    float triplanarRepeat; // repeats a metre, 0 for none
    float triplanarRepeat2;
    float triplanarRotated; // bit 1 the first layer's u and v swapped, bit 2 the second's
    float wetBelow;         // metres over the model's origin it is soaked below; see MaterialComponent::wetBelow
    float bare;             // 0..1, how far a cut-out surface has gone bare; see MaterialComponent::bare
    float emissiveR;        // the light it gives off, times its emissive texture; floats, as a vec3 would move it
    float emissiveG;
    float emissiveB;
    vec4 mudWheels[8];      // the wheels that throw mud up over it: hub, radius; see MaterialComponent::mudWheels
    vec4 mudTreads[2];      // how caked each one's tread is, 0..1, four to a vec4
    vec4 mudMap;            // its mud map's low corner in the model, its first layer + 1 (0 none); see MudMapsFrame
};

layout(SFE_SET(0, 7), std430) readonly buffer SfeMaterials {
    SfeMaterial sfeMaterials[];
};

#endif
