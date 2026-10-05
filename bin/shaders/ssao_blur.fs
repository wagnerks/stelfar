#version 450 core
#include "sfe.glsl"
layout(SFE_SET(0, 0)) uniform sampler2D ssaoInput;
layout(SFE_SET(0, 1)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 4)) uniform sampler2D gMaterial;
layout(SFE_SET(0, 3), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(0, 2), std140) uniform SSAOBlurParams {
    float sigmaS;
    float sigmaL;
    float facS;
    float facL;
} params;

layout(location = 0) in vec2 TexCoords;
layout(location = 0) out float FragColor;

// A texel of the half-size AO stands for the full-size pixel at twice its place (ssao.fs): its position from that
// pixel's depth exactly, so a texel by an edge is weighed by the surface it was worked out for.
vec3 texelView(ivec2 t, vec2 texelFull, out bool sky) {
    return sfeSceneViewPosition(sceneDepth, (vec2(t * 2) + 0.5) * texelFull, matrices.invProjection, sky);
}

// A leaf or a blade (ssao.fs): the two are not blurred into each other, or a tuft's grey spreads on the ground round it.
float foliageAt(ivec2 t) {
    return sfeIsFoliage(texelFetch(gMaterial, t * 2, 0)) ? 1.0 : 0.0;
}

void main() {
    ivec2 size = textureSize(ssaoInput, 0);
    vec2 texelFull = 1.0 / vec2(textureSize(sceneDepth, 0));
    ivec2 t = ivec2(TexCoords * vec2(size));
    bool sky;
    vec3 p0 = texelView(t, texelFull, sky);
    float center = texelFetch(ssaoInput, t, 0).r;
    if (sky) {
        FragColor = 1.0;
        return;
    }

    // A tap is weighed by how far it lies off the centre's plane (from the depth round it), not by how far it is: on
    // the ground seen low neighbouring texels are metres apart along it, and a plain distance left the blur only the
    // texels beside, across the view, so the noise stood in streaks. Off the plane by more than a few centimetres a
    // metre of distance (an edge, what stands behind) it counts for nothing.
    bool skyX;
    bool skyY;
    vec3 dpx = texelView(min(t + ivec2(1, 0), size - 1), texelFull, skyX) - p0;
    vec3 dpy = texelView(min(t + ivec2(0, 1), size - 1), texelFull, skyY) - p0;
    bool skyX0;
    bool skyY0;
    vec3 dmx = texelView(max(t - ivec2(1, 0), ivec2(0)), texelFull, skyX0) - p0;
    vec3 dmy = texelView(max(t - ivec2(0, 1), ivec2(0)), texelFull, skyY0) - p0;
    vec3 dx = skyX || (!skyX0 && abs(dmx.z) < abs(dpx.z)) ? -dmx : dpx;
    vec3 dy = skyY || (!skyY0 && abs(dmy.z) < abs(dpy.z)) ? -dmy : dpy;
    vec3 plane = cross(dx, dy);
    float plane2 = dot(plane, plane);
    // Degenerate only for the angle between the two steps, not their size: a millimetre a pixel near the eye made the
    // cross product smaller than any fixed bound, and the ground there turned to face the eye in a hard line.
    plane = plane2 > 1e-8 * dot(dx, dx) * dot(dy, dy) ? plane * inversesqrt(plane2) : vec3(0.0, 0.0, 1.0);
    float tolerance = 0.01 * -p0.z + 0.02;
    float leaf = foliageAt(t);
    float sum = 0.0;
    float wsum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            ivec2 tt = clamp(t + ivec2(x, y), ivec2(0), size - 1);
            bool tapSky;
            vec3 dp = texelView(tt, texelFull, tapSky) - p0;
            if (tapSky) {
                continue;
            }
            float off = dot(dp, plane) / tolerance;
            // Half weight on the outer ring: the two texels four apart are the same sixteenth of ssao.fs's 4x4 block,
            // so every sixteenth counts once, centred.
            float w = (abs(x) == 2 ? 0.5 : 1.0) * (abs(y) == 2 ? 0.5 : 1.0) * exp(-off * off);
            w *= foliageAt(tt) == leaf ? 1.0 : 0.02;
            sum += texelFetch(ssaoInput, tt, 0).r * w;
            wsum += w;
        }
    }
    FragColor = wsum > 1e-6 ? sum / wsum : center;
}
