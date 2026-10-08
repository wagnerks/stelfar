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

#include "sfe_block_blur.glsl"

// A texel of the half-size AO stands for the full-size pixel at twice its place (ssao.fs).
void main() {
    ivec2 t = ivec2(TexCoords * vec2(textureSize(ssaoInput, 0)));
    FragColor = sfeBlockBlur(ssaoInput, t, 2, 0.01, 0.02, 1.0);
}
