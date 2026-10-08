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
    vec4 at[3];
    for (int i = 0; i < 3; ++i) {
        at[i] = lightSpaceMatrices[layer] * gl_in[i].gl_Position;
    }
    // Wholly on the outer side of one of this face's edges, behind its eye or past its far end: it would be clipped away
    // whole, so it is not sent to this face at all.
    if ((at[0].x < -at[0].w && at[1].x < -at[1].w && at[2].x < -at[2].w)
        || (at[0].x > at[0].w && at[1].x > at[1].w && at[2].x > at[2].w)
        || (at[0].y < -at[0].w && at[1].y < -at[1].w && at[2].y < -at[2].w)
        || (at[0].y > at[0].w && at[1].y > at[1].w && at[2].y > at[2].w)
        || (at[0].z > at[0].w && at[1].z > at[1].w && at[2].z > at[2].w)
        || (at[0].w < 0.0 && at[1].w < 0.0 && at[2].w < 0.0)) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        gl_Position = at[i];
        gl_Layer = layer;
        TexCoords = inUV[i];
        TexCoords1 = inUV1[i];
        vMaterial = inMaterial[i];
        vPiece = inPiece[i];
        EmitVertex();
    }
    EndPrimitive();
}
