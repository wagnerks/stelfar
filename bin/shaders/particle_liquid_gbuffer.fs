#version 450 core
#include "sfe.glsl"
#include "sfe_liquid.glsl"
// The liquid into the G-buffer (ParticleLiquidPass), to be lit with the rest of the scene -- its shadows, its sky and
// lamps, its AO, its reflections, its fog: where its field (particle_liquid.fs) holds enough of it for its surface and
// it stops most of the light through it (mud; what is seen through, water, is drawn over the picture after TAA,
// particle_liquid_surface.fs). Its colour, how rough its face is, which way it faces, how far off it is (its depth) and
// how it moves across the picture there each the field's own, weighed by how much of each drop's liquid it holds.
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D how;
layout(SFE_SET(0, 1)) uniform sampler2D facing;
layout(SFE_SET(0, 2)) uniform sampler2D where;
layout(SFE_SET(0, 3)) uniform sampler2D moving;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(location = 0) out vec4 gMaterial;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gVelocity;
layout(location = 4) out vec4 gEmissive;

void main() {
    vec4 here = texture(how, TexCoords);
    float held = here.a;
    vec4 there = texture(where, TexCoords);
    if (held < kSfeLiquidSurface || exp(-there.y) >= kSfeLiquidClear) {
        discard;
    }
    vec3 at = sfeLiquidViewAt(TexCoords, there.x / held, matrices.invProjection);
    vec4 clip = matrices.projection * vec4(at, 1.0);
    float depth = clip.z / clip.w * 0.5 + 0.5;
    gl_FragDepth = depth;
    vec4 turned = texture(facing, TexCoords);
    vec3 faceView = dot(turned.xyz, turned.xyz) > 1e-8 ? normalize(turned.xyz) : vec3(0.0, 0.0, 1.0);
    gMaterial = sfeGBufferMaterial(0.0, 0.5, 0.0, 0.04);
    gNormal = vec4(normalize(mat3(matrices.invView) * faceView), depth * clip.w);
    gAlbedoSpec = vec4(here.rgb / held, clamp(turned.w / held, 0.0, 1.0));
    // As sfeGBufferVelocity has it: the motion (sfeVelocity, the drops'), no occlusion map.
    gVelocity = vec4(texture(moving, TexCoords).xy / held, 0.0, 0.0);
    gEmissive = vec4(0.0);
}
