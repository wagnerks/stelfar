#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
#include "sfe_water.glsl"
// The eye under the water: everything seen through the water the view crosses, up to the surface where it looks up
// through it; the bottom lit dimmer the deeper it lies and netted with caustics; the picture wavering a little.
layout(location = 0) in vec2 TexCoords;
layout(location = 0) out vec4 FragColor;

layout(SFE_SET(0, 15), std140) uniform WaterBlock {
    SFE_WATER_PARAMS
} water;

layout(SFE_SET(0, 6)) uniform sampler2D sceneColor;
layout(SFE_SET(0, 7)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 20)) uniform sampler2D skyLut;

const float PI = 3.14159265;
const float kUnderClear = 0.25;
// The water's glow around a swimmer, over what sfeWaterBody gives the surface seen from above.
const float kUnderGlow = 4.0;

// What an application makes of its water (sfe_user_water.glsl; the engine's defaults leave it as it is).
#include "sfe_user_water.glsl"

void main() {
    float level = water.surface.y;
    float seconds = water.sunColor.w;
    vec3 toSun = water.toSun.xyz;
    vec3 sun = water.sunColor.rgb;
    vec3 eye = water.cameraPos.xyz;

    // The wavering: slow, and a few pixels at most.
    vec2 uv = gl_FragCoord.xy * water.look.yz;
    uv += vec2(sin(uv.y * 23.0 + seconds * 1.9), cos(uv.x * 19.0 + seconds * 1.6)) * 0.0025;
    uv = clamp(uv, water.look.yz, 1.0 - water.look.yz);

    vec4 viewPos = matrices.invProjection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize((matrices.invView * vec4(viewPos.xyz / viewPos.w, 0.0)).xyz);
    vec3 scene = texture(sceneColor, uv).rgb;
    vec3 at = sfeScenePosition(sceneDepth, uv, matrices.invProjection, matrices.invView);
    bool nothing = dot(at, at) < 1e-6;
    float distanceTo = nothing ? 1e4 : distance(at, eye);
    // Up to the surface, where the view leaves the water (what is past it the water pass drew as seen from below).
    float toSurface = dir.y > 1e-4 ? (level - eye.y) / dir.y : 1e4;
    float path = min(distanceTo, toSurface);

    // Clearer than the stained lake looks from above, where the light crosses it twice: a swimmer sees some metres.
    // The water round the eye as an application leaves it (sfe_user_water.glsl): its albedo and its clarity.
    SfeWaterPlace place = sfeWaterPlace(water.look.x);
    sfeUserWater(eye.xz, place);
    vec3 extinction = sfeWaterExtinction(place.clarity) * kUnderClear;
    if (!nothing && distanceTo < toSurface) {
        // What lies in the water was lit as if in the air: the sunlight reaching it has crossed the water above it
        // (half as dimming as the view through it: light from above comes in scattered from all over the surface).
        float under = max(level - at.y, 0.0);
        scene *= exp(-extinction * under * 0.5) * sfeCausticLight(at, level, toSun, seconds, 7.0);
    }
    vec3 skyLight = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * water.cameraPos.w * PI;
    // The deeper the eye, the darker the water around it.
    vec3 body = sfeWaterBody(sun, toSun, skyLight, place.albedo) * kUnderGlow * exp(-extinction * max(level - eye.y, 0.0) * 0.5);
    vec3 transmitted = exp(-extinction * path);
    FragColor = vec4(scene * transmitted + body * (1.0 - transmitted), 1.0);
}
