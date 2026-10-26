#version 450 core
#include "sfe.glsl"
// A drop's ball laid into its liquid's field (particle_liquid.vs, ParticleLiquidPass), added to the other drops': along
// the eye's ray here, how near it comes to the drop's middle, r of the ball (in the drop's own frame, where its body is
// a ball of radius 1); how much of the liquid the drop lays here by that, k = (1 - r^2)^n of its middle's; and weighed
// by that, its colour, how far off, which way and how rough its face is here (where the ray strikes its body, or the ray's nearest
// to it past its edge); and how much of the light it stops here, its liquid's density through its body's breadth along
// the ray, as much of it as it lays here; and as much of how it moves across the picture. Where the G-buffer has
// something nearer, nothing.
layout(location = 0) flat in vec4 vLiquid;
layout(location = 1) flat in vec4 vShape;
layout(location = 2) flat in vec3 vMiddle;
layout(location = 3) flat in vec3 vRow0;
layout(location = 4) flat in vec3 vRow1;
layout(location = 5) flat in vec3 vRow2;
layout(location = 6) in vec4 vScreen;
layout(location = 7) flat in vec2 vMoving;

layout(SFE_SET(0, 5), std140) uniform SharedMatrices {
    SFE_SHARED_MATRICES
} matrices;
layout(SFE_SET(0, 4)) uniform sampler2D sceneDepth;

layout(location = 0) out vec4 How;    // rgb colour * k, a k
layout(location = 1) out vec2 Where;  // x distance * k, y the light stopped
layout(location = 2) out vec4 Facing; // xyz which way the face is, in view space, * k; w how rough it is * k
layout(location = 3) out vec2 Moving; // how it moves across the picture (sfeVelocity) * k

void main() {
    vec2 ndc = vScreen.xy / vScreen.w;
    vec4 far = matrices.invProjection * vec4(ndc, 0.0, 1.0);
    vec3 ray = normalize(far.xyz / far.w);
    // The eye and its ray in the drop's own frame.
    mat3 body = transpose(mat3(vRow0, vRow1, vRow2));
    vec3 from = body * -vMiddle;
    vec3 way = body * ray;
    float wayLength2 = dot(way, way);
    float nearest = -dot(from, way) / wayLength2;
    vec3 off = from + way * nearest;
    float r2 = dot(off, off);
    float reach2 = vShape.x * vShape.x;
    if (r2 >= reach2) {
        discard;
    }
    float k = pow(1.0 - r2 / reach2, vShape.y);
    // Where the ray strikes its body; past its edge, the ray's nearest to it.
    float along = r2 < 1.0 ? nearest - sqrt((1.0 - r2) / wayLength2) : nearest;
    if (along <= 0.0) {
        discard;
    }
    vec3 at = from + way * along;
    float face = -(ray * along).z;
    bool sky;
    vec3 behind = sfeSceneViewPosition(sceneDepth, ndc * 0.5 + 0.5, matrices.invProjection, sky);
    if (!sky && face - vShape.z > -behind.z) {
        discard;
    }
    vec3 facing = normalize(mat3(vRow0, vRow1, vRow2) * at);
    How = vec4(vLiquid.rgb * k, k);
    Where = vec2(face * k, vLiquid.a * 2.0 / sqrt(wayLength2) * k);
    Facing = vec4(facing * k, vShape.w * k);
    Moving = vMoving * k;
}
