// The drops' liquid's field (ParticleLiquidPass, particle_liquid.vs, .fs) as its surfaces read it: the opaque into the
// G-buffer to be lit with the scene (particle_liquid_gbuffer.fs), the clear over the picture after TAA
// (particle_liquid_surface.fs).

// What of a drop's middle's the field holds at the edge of its body: the liquid's surface, where it holds as much.
const float kSfeLiquidSurface = 0.5;
// As much of the light through it as the liquid lets through there (exp of what it stops), at least this it is seen
// through (water), drawn over the picture; less, a face the scene lights as its own (mud), in the G-buffer.
const float kSfeLiquidClear = 0.5;

// Where along the view at `uv` a point `distance` metres off is, in view space, by the (jittered) projection's inverse.
vec3 sfeLiquidViewAt(vec2 uv, float distance, mat4 invProjection) {
    vec4 ray = invProjection * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec3 dir = ray.xyz / ray.w;
    return dir * (distance / max(-dir.z, 1e-4));
}
