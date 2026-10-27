#ifndef SFE_GLSL
#define SFE_GLSL

#ifdef VULKAN
#define SFE_SET(setIdx, bindingIdx) set = setIdx, binding = bindingIdx
#define SFE_PUSH_BLOCK layout(push_constant) uniform
#define SFE_INSTANCE_ID gl_InstanceIndex
#define SFE_VERTEX_ID gl_VertexIndex
#else
#extension GL_ARB_shader_draw_parameters : require
#define SFE_SET(setIdx, bindingIdx) binding = bindingIdx
#define SFE_PUSH_BLOCK layout(std140, binding = 15) uniform
// gl_InstanceID starts at 0 whatever firstInstance is; gl_InstanceIndex on Vulkan includes it.
#define SFE_INSTANCE_ID (gl_InstanceID + gl_BaseInstanceARB)
#define SFE_VERTEX_ID gl_VertexID
#endif

// A hash that gives the same bits on every compiler path: integer arithmetic only. The usual
// fract(sin(x) * 43758.5453) does not -- sin() of a large argument differs in its low bits between the
// OpenGL driver's GLSL compiler and the SPIR-V path, and the multiply turns that into another value.
// Quantises p to 1/256 before hashing; mark the expression that builds p `precise` at the call site.
float sfeHash12(vec2 p) {
    uvec2 v = uvec2(ivec2(floor(p * 256.0)));
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float(v.x >> 8u) / 16777216.0;
}

// A hole a tree's or a plant's leaf card lets the sun through in the sun's shadow map: wherever the 2 cm cell across the
// light (`across`, metres) draws under `leafLight`. The same cell for every leaf along a ray, so the share holds however
// many leaves deep the crown is (each leaf holed by itself let leafLight to the power of the leaves through); fixed in
// the world, so it does not crawl as the view moves the cascades. Bits 32 and 64 of the map mask: a tree's or a plant's
// sway.
bool sfeLeafHole(float mapMask, vec2 across, float leafLight) {
    return (uint(mapMask + 0.5) & 96u) != 0u && sfeHash12(floor(across / 0.02)) < leafLight;
}

// 0..1 for a vertex of a model standing at `origin`, passed on flat so each triangle takes its first (or, on OpenGL,
// last) vertex's: the point on the way to bare (SfeMaterial::bare) at which that triangle of a cut-out surface, a leaf,
// is gone. Where the model stands tells two trees of one model apart, so they do not lose the same leaves.
float sfePieceKey(uint vertex, vec3 origin) {
    uvec3 v = uvec3(vertex, uvec2(ivec2(floor(origin.xz * 4.0))));
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    return float(v.y >> 8u) / 16777216.0;
}

// Jimenez's interleaved gradient noise, fract(52.9829189 * fract(dot(p + 5.588238 * frame, vec2(0.06711056,
// 0.00583715)))), in fixed point. The float form came out different on OpenGL and Vulkan for the same pixel --
// the two shader compilers evaluate it differently even marked precise -- so everything it rotates differed
// between backends. Integer arithmetic has one result: the inner fract keeps 16 bits (the frame shift adds
// frame * 5.588238 * (0.06711056 + 0.00583715) = frame * 26716 / 2^16), and the outer multiply wraps modulo
// 2^32, which is exactly the outer fract.
float sfeInterleavedGradientNoise(vec2 p, uint frame) {
    uvec2 pixel = uvec2(p);
    uint inner = (pixel.x * 4398u + pixel.y * 383u + frame * 26716u) & 0xFFFFu; // fract(dot(p, k)) * 2^16
    uint outer = inner * 3472289u;                                              // fract(52.9829189 * inner / 2^16) * 2^32
    return float(outer >> 8u) / 16777216.0;
}

// A pattern that stands still and still leaves no grain: each texel of a 4x4 block takes its own sixteenth, in an order
// that spreads neighbours apart (Bayer), and a blur over exactly one block (sfe_block_blur.glsl) puts the sixteen back
// together. x the sixteenth itself, (index + 0.5) / 16; y the golden ratio's sequence over the same, in another order.
vec2 sfeBlockNoise(vec2 fragCoord) {
    const float BAYER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 cell = ivec2(fragCoord) & 3;
    float index = BAYER[cell.y * 4 + cell.x];
    return vec2((index + 0.5) / 16.0, fract(0.5 + index * 0.618034));
}

// What is right at the camera fades out towards the near plane rather than being cut off by it: over the first
// SFE_NEAR_FADE of the near distance past it, more of its pixels dropped in a dither the nearer it is. Measured off
// the depth buffer's own value: with the far plane far off, a depth z is near / (1 - z) away, so (distance - near) /
// near is z / (1 - z) -- whatever the near plane is, and with no matrices. Every surface drawn into the view asks it
// first thing, but the ground (g_buffer_terrain.fs).
const float SFE_NEAR_FADE = 0.2;
bool sfeNearFaded(vec4 fragCoord) {
    float past = fragCoord.z / max(1.0 - fragCoord.z, 1e-6);
    return past < SFE_NEAR_FADE * sfeInterleavedGradientNoise(fragCoord.xy, 0u);
}

// vec2(cos(a), sin(a)) without the built-ins, which differ in their low bits between the OpenGL driver's
// GLSL compiler and the SPIR-V path. a is folded to [-pi/4, pi/4] around the nearest quarter turn, where
// Taylor series to the 9th power are within 3e-8, far below a pixel for any disc radius in use.
vec2 sfeCosSin(float a) {
    const float halfPi = 1.5707963267948966;
    precise float q = floor(a / halfPi + 0.5);
    precise float r = a - q * halfPi;
    precise float r2 = r * r;
    precise float s = r * (1.0 - r2 / 6.0 * (1.0 - r2 / 20.0 * (1.0 - r2 / 42.0 * (1.0 - r2 / 72.0))));
    precise float c = 1.0 - r2 / 2.0 * (1.0 - r2 / 12.0 * (1.0 - r2 / 30.0 * (1.0 - r2 / 56.0)));
    int quadrant = int(q) & 3;
    if (quadrant == 1) {
        return vec2(-s, c);
    }
    if (quadrant == 2) {
        return vec2(-c, -s);
    }
    if (quadrant == 3) {
        return vec2(s, -c);
    }
    return vec2(c, s);
}

// The G-buffer's first target, RGBA8: metallic, specular factor, clearcoat, clearcoat roughness. A surface with no
// clearcoat has no use for its roughness: there that channel says how much sunlight it lets through, as leaves and
// blades of grass do (0.04, what everything else writes, is none).
vec4 sfeGBufferMaterial(float metallic, float specFactor, float clearcoat, float clearcoatRough) {
    return clamp(vec4(metallic, specFactor, clearcoat, clearcoatRough), 0.0, 1.0);
}
const float kSfeTranslucentFoliage = 0.7;
// A leaf or a blade of grass, by the G-buffer's material: no coat, and the translucent foliage's mark.
bool sfeIsFoliage(vec4 material) {
    return material.z < 0.5 / 255.0 && abs(material.w - kSfeTranslucentFoliage) < 1.5 / 255.0;
}
// A clearcoat of exactly this is no coat but a glint: the surface is shaded as it is, and where it mirrors the sun into
// the eye it flashes, blinding (a bald crown). Too thin for any coat to use.
const float kSfeGlint = 1.0 / 255.0;
// And this, the ground: shaded as it is, and known for what stains it (the area tint).
const float kSfeGround = 3.0 / 255.0;
bool sfeIsGround(float clearcoat) {
    return abs(clearcoat - kSfeGround) < 0.5 / 255.0;
}

// The world position of what the scene shows at `uv`, from SceneDepthPass's copy of the depth buffer and the
// camera's inverse matrices (the jittered ones the G-buffer was drawn with); zero where nothing was drawn. The
// G-buffer used to keep positions in an RGBA32F target of their own, written for every fragment of every
// overdrawn leaf.
vec3 sfeScenePosition(sampler2D sceneDepth, vec2 uv, mat4 invProjection, mat4 invView) {
    float depth = textureLod(sceneDepth, uv, 0.0).r;
    if (depth >= 1.0) {
        return vec3(0.0);
    }
    vec4 viewPos = invProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return (invView * vec4(viewPos.xyz / viewPos.w, 1.0)).xyz;
}

// sfeScenePosition in view space, for a caller that only wants view space: one matrix product instead of the round
// trip through world space. `sky` is set where nothing was drawn, and the position is then the origin's.
vec3 sfeSceneViewPosition(sampler2D sceneDepth, vec2 uv, mat4 invProjection, out bool sky) {
    float depth = textureLod(sceneDepth, uv, 0.0).r;
    sky = depth >= 1.0;
    if (sky) {
        return vec3(0.0);
    }
    vec4 viewPos = invProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return viewPos.xyz / viewPos.w;
}




// Value noise in [0, 1] on an integer lattice hash, so both backends see the same field.
uint sfeCellHash(ivec2 cell) {
    uvec2 v = uvec2(cell) * uvec2(1664525u, 1013904223u);
    uint h = v.x ^ (v.y * 747796405u) ^ 2891336453u;
    h ^= h >> 16u;
    h *= 2246822519u;
    h ^= h >> 13u;
    h *= 3266489917u;
    h ^= h >> 16u;
    return h;
}

float sfeValueNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 f = p - cell;
    // Quintic: the slope is continuous too, so a normal taken from this noise shows no creases along the cells.
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    ivec2 c = ivec2(cell);
    float a = float(sfeCellHash(c) >> 8u) / 16777216.0;
    float b = float(sfeCellHash(c + ivec2(1, 0)) >> 8u) / 16777216.0;
    float d = float(sfeCellHash(c + ivec2(0, 1)) >> 8u) / 16777216.0;
    float e = float(sfeCellHash(c + ivec2(1, 1)) >> 8u) / 16777216.0;
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

// Octaves of sfeValueNoise, each rotated against the last so the lattice does not show; in [0, 1].
float sfeFbm(vec2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += sfeValueNoise(p) * amp;
        norm += amp;
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + vec2(3.1, 7.7);
        amp *= 0.5;
    }
    return sum / norm;
}

// Rec. 709 luminance, the weighting every pass here already used inline.
float sfeLuma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// How much a colour is a plant's: its hue from a yellow green through green (about 50 to 180 degrees; straw, blond
// hair and skin are below it), and not near grey.
float sfePlantColour(vec3 c) {
    const float hi = max(c.r, max(c.g, c.b));
    const float chroma = hi - min(c.r, min(c.g, c.b));
    if (chroma <= 1e-5) {
        return 0.0;
    }
    float hue = hi == c.r ? (c.g - c.b) / chroma : (hi == c.g ? 2.0 + (c.b - c.r) / chroma : 4.0 + (c.r - c.g) / chroma);
    hue *= 60.0;
    return smoothstep(45.0, 58.0, hue) * (1.0 - smoothstep(170.0, 190.0, hue)) * smoothstep(0.15, 0.3, chroma / hi);
}

// A plant going bare (SfeMaterial::bare) at a texel of a triangle of its cut-out surface whose point on the way
// (sfePieceKey) it has passed: false where that is a leaf, gone; the twigs painted between the leaves stay, dead.
bool sfeBareKeeps(vec3 linearColour) {
    return sfePlantColour(linearColour) < 0.5;
}

// How much the surface scatters light under it, 0 to 1, from the G-buffer's normal (shortened by up to half for it) and
// its metalness (gMaterial.r).
float sfeScatters(vec3 gNormalSample, float metallic) {
    return dot(gNormalSample, gNormalSample) < 1e-6 ? 0.0
        : clamp((1.0 - length(gNormalSample)) * 2.0, 0.0, 1.0) * (1.0 - metallic);
}

float sfeSkinSss(vec3 albedo, float metallic) {
    float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float warm = clamp((albedo.r - albedo.g) * 3.5, 0.0, 1.0);
    warm *= smoothstep(0.03, 0.10, luma);
    warm *= smoothstep(0.0, 0.04, albedo.r - albedo.b);
    return (1.0 - metallic) * warm;
}

// Where a pixel moved since last frame, in UV: history belongs at TexCoords - velocity. Both clip positions
// come from unjittered matrices, so a still surface gives exactly zero.
vec2 sfeVelocity(vec4 clipNow, vec4 clipBefore) {
    if (clipNow.w <= 1e-6 || clipBefore.w <= 1e-6) {
        return vec2(0.0);
    }
    vec2 now = clipNow.xy / clipNow.w;
    vec2 before = clipBefore.xy / clipBefore.w;
    return (now - before) * 0.5;
}

// The velocity target: rg the motion (sfeVelocity, read by taa.fs), b how much a material's occlusion map shuts out the
// ambient light (0 none: the clear and every surface without one), a unused.
vec4 sfeGBufferVelocity(vec4 clipNow, vec4 clipBefore, float occlusion) {
    return vec4(sfeVelocity(clipNow, clipBefore), clamp(occlusion, 0.0, 1.0), 0.0);
}

// The emissive target: rgb the light a surface gives off itself, its material's `emissive` times its emissive texture
// (sRGB, as the albedo's), added once the surface is lit (deferred_shading.fs); a unused. None for a material without.
vec4 sfeGBufferEmissive(vec3 emissive, sampler2D map, vec2 uv) {
    if (max(emissive.r, max(emissive.g, emissive.b)) <= 0.0) {
        return vec4(0.0);
    }
    return vec4(emissive * pow(texture(map, uv).rgb, vec3(2.2)), 0.0);
}

#endif

// One definition of the per-frame matrices block. OpenGL links every shader of a program together and
// rejects a uniform block whose members differ between them, so the body lives here and each shader only
// picks its own set/binding.
#define SFE_SHARED_MATRICES \
    mat4 projection; \
    mat4 view; \
    mat4 PV; \
    mat4 PVNoJitter; \
    mat4 prevPVNoJitter; \
    vec4 wind;       /* xz the direction it blows along, y strength, w seconds; see sfe_wind.glsl */ \
    vec4 windBefore; /* the same, last frame */ \
    mat4 invProjection; /* of the jittered projection */ \
    mat4 invView; \
    vec4 style;      /* x the drawn look, GraphicsSettings::stylise */ \
    vec4 spare0;     /* spare0-4 written 0, read by nobody: an application's frame data is in its user set (UserFrameSet); step 6 takes them out */ \
    vec4 spare1; \
    vec4 spare2; \
    vec4 spare3; \
    vec4 spare4; \
    vec4 patchDeepest; /* x TerrainPatch::deepest: metres the application lowers the ground drawn dense at most */ \
    vec4 marksGrid;   /* TerrainPatch::grid: xy the mesh's first vertex's world x and z, z their spacing, w its base height */ \
    vec4 marksPatch;  /* TerrainPatch::square: xy its least x and z, z its side, w its spacing; a zero side, none */
