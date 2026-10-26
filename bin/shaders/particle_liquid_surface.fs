#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
#include "sfe_liquid.glsl"
// The liquid's surface over the picture after TAA (ParticleClearLiquidPass), where its field (particle_liquid.fs) holds
// enough of it and it lets most of the light through (water; what stops it, mud, is in the G-buffer, lit with the
// scene, particle_liquid_gbuffer.fs):
// its colour, how far off it is and which way it faces there each the field's own, weighed by how much of each drop's
// liquid it holds, so a drop on its own is a ball and drops run together are one smooth body. Through it the picture
// behind, bent by its face and dimmed by as much of the light as the liquid stops; what it stops lit as the scene is lit
// (deferred_shading.fs, sfe_pbr.glsl), of the liquid's own colour; over that what it mirrors as the scene does, more at a
// glance (the picture where a smooth face's mirrored ray meets something in it), and the sun glinting. Its edge
// smoothed over a pixel.
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D how;
layout(SFE_SET(0, 1)) uniform sampler2D facing;
layout(SFE_SET(0, 2)) uniform sampler2D where;
layout(SFE_SET(0, 3)) uniform sampler2D picture;
layout(SFE_SET(0, 4)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 6)) uniform sampler2D ambientOcclusion;
layout(SFE_SET(0, 7)) uniform samplerCube irradiance;
layout(SFE_SET(0, 9)) uniform samplerCube prefilterMap;
layout(SFE_SET(0, 10)) uniform sampler2D brdfLUT;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 8)) uniform sampler2DArrayShadow shadowMap;
layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[6];
};
layout(SFE_SET(0, 15), std140) uniform LiquidBlock {
    SFE_SKY_PARAMS
    vec4 view; // z shadow cascades, w the light the scene takes from its irradiance (iblStrength)
    vec4 shade; // as the scene is lit: x how much of the AO it takes, y how far out of the sun a side turned away from
                // it is, z what of the sky's light reaches into a shadow
} liquid;
#include "sfe_sun_visible.glsl"
#include "sfe_reflection.glsl"
#include "sfe_pbr.glsl"

layout(location = 0) out vec4 FragColor;

// How far the picture behind is bent through it, of the picture, for a face turned wholly aside.
const float kBend = 0.03;
// What a liquid's face mirrors head on (water's), and how rough it glints in the sun at the smoothest (the scene's clear
// coat's).
const float kMirror = 0.02;
const float kSharpest = 0.08;

void main() {
    vec4 here = texture(how, TexCoords);
    float held = here.a;
    // How much of the pixel the liquid covers: its edge, where the field passes kSfeLiquidSurface, over a pixel.
    float cover = clamp((held - kSfeLiquidSurface) / max(fwidth(held), 1e-4) + 0.5, 0.0, 1.0);
    vec4 there = texture(where, TexCoords);
    float stopped = there.y;
    float through = exp(-stopped);
    if (cover <= 0.0 || through < kSfeLiquidClear) {
        discard;
    }
    vec3 at = sfeLiquidViewAt(TexCoords, there.x / held, matrices.invProjection);
    vec3 colour = here.rgb / held;
    // Its face, turned as the drops' faces here are, together, and as rough.
    vec4 turned = texture(facing, TexCoords);
    vec3 faceView = dot(turned.xyz, turned.xyz) > 1e-8 ? normalize(turned.xyz) : vec3(0.0, 0.0, 1.0);
    float rough = clamp(turned.w / held, 0.0, 1.0);

    vec3 world = (matrices.invView * vec4(at, 1.0)).xyz;
    vec3 N = normalize(mat3(matrices.invView) * faceView);
    vec3 V = normalize(matrices.invView[3].xyz - world);
    vec3 toSun = liquid.toSun.xyz;
    vec3 sunColor = liquid.sunColor.rgb;
    float NdotV = max(dot(N, V), 0.0);

    // Lit as the scene is: the sun as far as it reaches its face; the light all round from the scene's irradiance and
    // its reflections, the irradiance as much as the AO lets it reach, both dimmed in a shadow as the scene's are (under
    // a car or the trees, much).
    float sun = sfeSunReaching(sfeSunVisible(world, int(liquid.view.z + 0.5)), dot(N, toSun), liquid.shade.y);
    float shaded = mix(liquid.shade.z, 1.0, sun);
    float open = mix(1.0, texture(ambientOcclusion, TexCoords).r, liquid.shade.x);
    // How much of the light its face mirrors (the split sum's, as the scene's), the rest going into it.
    vec2 brdf = texture(brdfLUT, vec2(NdotV, rough)).rg;
    vec3 mirror = fresnelSchlickRoughness(NdotV, vec3(kMirror), rough) * brdf.x + brdf.y;
    // What the liquid stops of the light, of its colour; the rest of it passes, the picture behind bent by its face.
    vec3 lit = colour * (sunColor * max(dot(N, toSun), 0.0) * sun + texture(irradiance, N).rgb * liquid.view.w * open * shaded);
    vec3 behind = texture(picture, TexCoords + faceView.xy * kBend).rgb;
    vec3 body = mix(lit, behind, through);
    // What it mirrors, as rough as its face, below the horizon the horizon (as the sky is drawn there: the scene's
    // reflections hold no ground); a smooth one the picture where its mirrored ray meets something in it.
    vec3 R = reflect(-V, N);
    vec3 skyward = normalize(vec3(R.x, max(R.y, 0.02), R.z));
    vec3 mirrored = textureLod(prefilterMap, skyward, rough * MAX_REFLECTION_LOD).rgb * liquid.view.w * shaded;
    vec2 hitUv;
    if (rough < 1.0 && sfeMarchReflection(world + N * 0.02, R, -1e30, length(at), 1.0 / vec2(textureSize(sceneDepth, 0)), hitUv)) {
        mirrored = mix(mirrored, texture(picture, hitUv).rgb, sfeReflectionOnScreen(hitUv) * (1.0 - rough));
    }
    vec3 glint = cookTorrance(N, V, toSun, vec3(0.0), vec3(kMirror), max(rough, kSharpest), 0.0, sunColor * PI, 1.0) * sun;
    vec3 surface = mix(body, mirrored, mirror) + glint;

    FragColor = vec4(mix(texture(picture, TexCoords).rgb, surface, cover), 1.0);
}
