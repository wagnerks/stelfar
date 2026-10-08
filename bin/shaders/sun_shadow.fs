#version 450 core
#include "sfe.glsl"
layout(location = 0) out float FragColor;
layout(location = 0) in vec2 TexCoords;

layout(SFE_SET(0, 0)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 1)) uniform sampler2D gNormal;
layout(SFE_SET(0, 2)) uniform sampler2D gMaterial;
layout(SFE_SET(0, 3), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

// The sun's shadow (sfe_sun_shadow.glsl) over what is drawn, for the lighting to read (SunShadowPass). Its disk is turned
// by the pixel's place in a 4x4 block, a sixteenth each, the same every frame, and sun_shadow_blur.fs averages exactly
// one block back together: sixteen turns to every pixel. A noise that stood still turned it before, and left a fine
// weave over the ground under the trees that TAA cannot take out; turned each frame, the shadows shimmered.
#define SFE_SUN_SHADOW_DEPTH SFE_SET(0, 4)
#define SFE_SUN_SHADOW_BLOCK SFE_SET(0, 5)
#define SFE_SUN_SHADOW_MATRICES SFE_SET(0, 6)
#define SFE_SUN_SHADOW_TURN sfeBlockNoise(gl_FragCoord.xy)
#include "sfe_sun_shadow.glsl"

// The surface's two steps across the screen, in the world: to the next pixel right and up, each from the nearer of its
// two neighbours so an edge does not bend them; in view space first, where the numbers are small, then turned.
void surfaceSteps(vec3 viewPos, out vec3 tx, out vec3 ty) {
    vec2 px = 1.0 / vec2(textureSize(sceneDepth, 0));
    bool sky;
    vec3 x0 = sfeSceneViewPosition(sceneDepth, TexCoords - vec2(px.x, 0.0), matrices.invProjection, sky) - viewPos;
    vec3 x1 = sfeSceneViewPosition(sceneDepth, TexCoords + vec2(px.x, 0.0), matrices.invProjection, sky) - viewPos;
    vec3 y0 = sfeSceneViewPosition(sceneDepth, TexCoords - vec2(0.0, px.y), matrices.invProjection, sky) - viewPos;
    vec3 y1 = sfeSceneViewPosition(sceneDepth, TexCoords + vec2(0.0, px.y), matrices.invProjection, sky) - viewPos;
    mat3 toWorld = mat3(matrices.invView);
    tx = toWorld * (abs(x0.z) < abs(x1.z) ? -x0 : x1);
    ty = toWorld * (abs(y0.z) < abs(y1.z) ? -y0 : y1);
}

void main() {
    const vec3 nSample = texture(gNormal, TexCoords).rgb;
    // Nothing drawn here (the sky): lit, as the blur takes it.
    if (dot(nSample, nSample) < 0.01) {
        FragColor = 0.0;
        return;
    }
    const vec3 FragPos = sfeScenePosition(sceneDepth, TexCoords, matrices.invProjection, matrices.invView);
    const float nLen = length(nSample);
    const vec3 Normal = nLen > 1e-5 ? nSample / nLen : vec3(0.0, 1.0, 0.0);
    // Everywhere, turned from the sun or not: a normal map's bumps turn single pixels of the ground away from a low sun,
    // and the blur takes them in with the rest.
    // A blade's or a leaf's plane from the depth round it is whatever stands beside it, the card itself a few
    // millimetres wide: drawn out over the filter it lit the grass at a leg's foot, the shadow beginning off the leg.
    vec3 tx = vec3(0.0);
    vec3 ty = vec3(0.0);
    if (!sfeIsFoliage(texture(gMaterial, TexCoords))) {
        surfaceSteps((matrices.view * vec4(FragPos, 1.0)).xyz, tx, ty);
    }
    FragColor = ShadowCascadedCalculation(FragPos, Normal, tx, ty);
}
