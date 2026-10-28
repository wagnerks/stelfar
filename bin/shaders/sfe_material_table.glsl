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
    float dissolve;         // 0..1, how much of a cut-out surface is gone: its pieces whose vPiece is under it;
                            // see MaterialComponent::dissolve
    float emissiveR;        // the light it gives off, times its emissive texture; floats, as a vec3 would move it
    float emissiveG;
    float emissiveB;
    vec4 user[2];           // the application's own, eight a material (MaterialComponent::user); never read here
};

layout(SFE_SET(0, 7), std430) readonly buffer SfeMaterials {
    SfeMaterial sfeMaterials[];
};

#endif
