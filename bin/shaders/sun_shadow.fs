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
    const vec4 material = texture(gMaterial, TexCoords);
    FragColor = ShadowCascadedCalculation(FragPos, Normal, sfeIsFoliage(material), sfeScatters(nSample, material.r));
}
