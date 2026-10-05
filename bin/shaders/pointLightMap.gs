#version 450 core
#include "sfe.glsl"
layout(triangles, invocations = 6) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 1) in vec2 inUV[];
layout(location = 2) in vec2 inUV1[];
layout(location = 3) flat in uint inMaterial[];
layout(location = 4) flat in float inPiece[];
layout(location = 1) out vec2 TexCoords;
layout(location = 2) out vec2 TexCoords1;
layout(location = 3) flat out uint vMaterial;
layout(location = 4) flat out float vPiece;

layout(SFE_SET(0, 11), std140) uniform LightSpaceMatrices {
    mat4 lightSpaceMatrices[72];
};

// The light whose faces are drawn, one draw a light (PointLightPass): each invocation one of its six.
SFE_PUSH_BLOCK Push {
    int light;
} pc;

void main() {
    int layer = pc.light * 6 + gl_InvocationID;
    for (int i = 0; i < 3; ++i) {
        gl_Position = lightSpaceMatrices[layer] * gl_in[i].gl_Position;
        gl_Layer = layer;
        TexCoords = inUV[i];
        TexCoords1 = inUV1[i];
        vMaterial = inMaterial[i];
        vPiece = inPiece[i];
        EmitVertex();
    }
    EndPrimitive();
}
