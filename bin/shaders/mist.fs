#version 450 core
#include "sfe.glsl"
#include "sfe_sky.glsl"
// Mist on the water and haze in the distance, over the finished image. Along the view ray up to whatever the pixel
// shows -- the sky counts as far away, and a ray into the lake stops at its surface -- the mist's density falls off
// exponentially with height above the water, so its thickness is exact rather than marched. The mist drifts in
// patches with the wind; both are lit by the sky around the horizon and by the sun scattered forwards. An application
// may add a medium of its own along the ray, lit in its own colour (sfe_user_mist.glsl).
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 FragColor; // rgb the light added, a what is left of the image behind

layout(SFE_SET(0, 7)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 20)) uniform sampler2D skyLut;

layout(SFE_SET(0, 15), std140) uniform MistBlock {
    SFE_SKY_PARAMS
    vec4 mist; // x density at the water line per metre, y metres up over which it thins to 1/e, z haze per metre, w water level
    vec4 view; // x the view distance: the far plane
    vec4 tintArea;    // the area tint map's place, see sfeAreaTint
    vec4 fumes;       // x the map's water value they rise from, y metres up over which they thin to 1/e
    vec4 fumesColour; // rgb what the light in them is multiplied by, a density per metre at the water line at worst
} fog;

const float PI = 3.14159265;
// How far the sky counts as being.
const float kSkyDistance = 4000.0;
// Metres up over which the haze thins to 1/e.
const float kHazeHeight = 150.0;

// The thickness of a layer whose density falls off as exp(-height / scale), along a ray between two heights over its
// base: the integral in closed form, written so neither end can overflow.
float sfeLayerDepth(float startHeight, float endHeight, float dirY, float rayLength, float scale) {
    if (abs(dirY) > 1e-3) {
        return scale / dirY * (exp(-startHeight / scale) - exp(-endHeight / scale));
    }
    return exp(-startHeight / scale) * rayLength;
}

// A medium of the application's own along the view ray, besides the mist and the haze: the file says what it is given
// and what it returns. It comes after everything above, which it may read.
#include "sfe_user_mist.glsl"

void main() {
    vec3 camera = fog.cameraPos.xyz;
    vec3 surface = sfeScenePosition(sceneDepth, vNdc * 0.5 + 0.5, matrices.invProjection, matrices.invView);
    vec3 dir;
    float rayLength;
    bool isSky = dot(surface, surface) <= 1e-6;
    if (!isSky) {
        vec3 toSurface = surface - camera;
        rayLength = length(toSurface);
        dir = toSurface / max(rayLength, 1e-4);
    }
    else {
        vec4 far = fog.invViewProj * vec4(vNdc, 0.5, 1.0);
        dir = normalize(far.xyz / far.w - camera);
        rayLength = kSkyDistance;
    }
    float level = fog.mist.w;
    // The lake bottom seen through the water: the fog ends at the surface.
    if (dir.y < -1e-4) {
        rayLength = min(rayLength, max(camera.y - level, 0.0) / -dir.y);
    }
    rayLength = min(rayLength, kSkyDistance);

    float startHeight = max(camera.y - level, 0.0);
    float endHeight = max(startHeight + dir.y * rayLength, 0.0);
    float mistDepth = sfeLayerDepth(startHeight, endHeight, dir.y, rayLength, fog.mist.y);
    // Thicker and thinner patches, drifting with the wind, taken low down along the ray where the mist is.
    // Only where there is mist along the ray for them to thin or thicken: above it, or with no mist, they multiply zero.
    // The haze is air near the ground as well, only a much deeper layer of it: across the lake the far shore greys
    // and pales into the sky, while a ray up into the sky passes through little of it.
    float opticalDepth = fog.mist.z * sfeLayerDepth(startHeight, endHeight, dir.y, rayLength, kHazeHeight);
    if (isSky) {
        // The sky's own scattering is in the sky itself; the haze only thickens the band near the horizon, or a ray
        // up through a layer this deep would wash the whole sky grey.
        opticalDepth *= smoothstep(0.32, 0.0, dir.y);
    }
    if (fog.mist.x != 0.0 && mistDepth > 0.0) {
        vec2 drift = fog.wind.xz * fog.sunColor.w * (0.3 + 0.9 * fog.wind.y);
        vec2 patchAt = camera.xz + dir.xz * min(rayLength, 600.0) - drift;
        float patches = mix(0.3, 1.7, sfeFbm(patchAt * 0.008, 3));
        opticalDepth = fog.mist.x * patches * mistDepth + opticalDepth;
    }
    // The application's own medium along the ray: its depth adds to the rest, and its part of the whole is how much of
    // the light scattered along the ray is in its colour and its share of the sun's glow.
    vec3 userColour;
    float userSun;
    float userDepth = sfeUserMist(camera, dir, rayLength, startHeight, level, userColour, userSun);
    float userShare = userDepth / max(opticalDepth + userDepth, 1e-5);
    opticalDepth += userDepth;
    float transmittance = exp(-opticalDepth);
    // The last stretch before the far plane fades into the haze entirely, so the scene has no edge where it ends.
    if (!isSky) {
        float viewDistance = fog.view.x;
        transmittance *= 1.0 - smoothstep(viewDistance * 0.75, viewDistance * 0.98, distance(surface, camera));
    }

    // The light in it: the sky just above the horizon in this direction, and the sun glowing through it.
    vec3 toSun = fog.toSun.xyz;
    float brightness = fog.cameraPos.w;
    vec3 horizon = sfeSkyRadiance(skyLut, vec3(dir.x, 0.08, dir.z)) * brightness;
    vec3 overhead = sfeSkyRadiance(skyLut, vec3(0.0, 1.0, 0.0)) * brightness;
    float g = 0.55;
    float mu = dot(dir, toSun);
    float phase = (1.0 - g * g) / (4.0 * PI * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    vec3 skyPart = mix(horizon, overhead, 0.35) * 1.1;
    vec3 sunPart = fog.sunColor.rgb * phase * 0.35 * max(toSun.y + 0.1, 0.0);
    // Of it, the application's medium's share is lit with its own share of the sun's glow, in its own colour.
    vec3 inScatter = mix(skyPart + sunPart, (skyPart + sunPart * userSun) * userColour, userShare);

    FragColor = vec4(inScatter * (1.0 - transmittance), transmittance);
}
