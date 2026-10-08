#version 450 core
#include "sfe.glsl"
layout(SFE_SET(0, 0)) uniform sampler2D shadowInput;
layout(SFE_SET(0, 1)) uniform sampler2D sceneDepth;
layout(SFE_SET(0, 2)) uniform sampler2D gMaterial;
layout(SFE_SET(0, 3), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;

layout(SFE_SET(0, 4), std140) uniform SunShadowBlurParams {
    float tolerance; // metres off the centre's plane a metre from the eye past which a texel counts for little
} params;

layout(location = 0) in vec2 TexCoords;
layout(location = 0) out float FragColor;

#include "sfe_block_blur.glsl"

// sun_shadow.fs's sixteen turns of the disk, one to each texel of a 4x4 block, averaged back together at full size.
void main() {
    ivec2 t = ivec2(TexCoords * vec2(textureSize(shadowInput, 0)));
    FragColor = sfeBlockBlur(shadowInput, t, 1, params.tolerance, 2.0 * params.tolerance, 0.0);
}
