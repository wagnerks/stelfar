#ifndef SFE_GLASS_GLSL
#define SFE_GLASS_GLSL
// What GlassPass gives the fragment shader of a transparent surface -- glass.fs, or a material's own: what g_buffer.vs
// hands on, the lit scene behind (sceneCopy), the sky's reflections, the sun (params) and the material table. The
// colour is blended over what is behind premultiplied: its alpha already multiplied in. Include after sfe.glsl and
// sfe_material.glsl.
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec2 TexCoords;
layout(location = 2) in mat3 TBN;
layout(location = 5) in vec2 TexCoords1;
layout(location = 6) flat in uint vMaterial;

layout(location = 0) out vec4 FragColor;

layout(SFE_SET(0, 6)) uniform sampler2D sceneCopy;
layout(SFE_SET(0, 13)) uniform samplerCube prefilterMap;
// Every glass albedo in one array; each glass draw holds one instance, so the index is constant per draw.
layout(SFE_SET(0, 16)) uniform sampler2D albedoMaps[8];

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(0, 15), std140) uniform PushConstants {
    vec3 viewPos;
    float _pad2;
    vec2 resolution;
    float _pad3;
    float roughness;
    vec3 lightDir;
    float _pad0;
    vec3 lightColor;
    float _pad1;
    // Spare, so the block keeps its size; GlassPass::Params mirrors it.
    vec4 _pad4;
} params;

#endif
