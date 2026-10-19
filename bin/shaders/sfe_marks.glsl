#ifndef SFE_MARKS_GLSL
#define SFE_MARKS_GLSL
// The marks in the ground round the camera (GroundMarksFrame): how deep the ground is pressed and how churned up, in
// two windows that wrap round as the camera moves on, a fine one near and a coarse one further out. Included after
// the SharedMatrices block, named `matrices`, whose marks* it reads; its pass binds the map at 34 of set 0.
layout(SFE_SET(0, 34)) uniform sampler2DArray groundMarks;

// How much of each window stands at `xz`: x the fine one, y the coarse one, each faded out towards its edge, where the
// map wraps round to the other side. None with no marks.
vec2 sfeMarksCover(vec2 xz) {
    if (matrices.marksSize.x <= 0.0) {
        return vec2(0.0);
    }
    vec4 inside = (xz.xyxy - matrices.marksWindow) / matrices.marksSize.xxyy;
    vec4 edge = smoothstep(0.03, 0.1, min(inside, 1.0 - inside));
    return vec2(edge.x * edge.y, edge.z * edge.w);
}

// How far off `xz` the marks are read: the law presses straight edges, the ground crumbles and gives unevenly, so its
// marks' edges wander a few centimetres either way.
vec2 sfeMarksWarp(vec2 xz) {
    return (vec2(sfeValueNoise(xz * 9.0), sfeValueNoise(xz * 9.0 + vec2(31.7, 17.3))) - 0.5) * 0.06;
}

// How a mark's floor lies at `xz`, 0..1: the ground slumps into it unevenly, so along a rut its floor sinks and rises
// over a metre or so, deeper where this is higher. Water stands in its low parts (g_buffer_terrain.fs).
float sfeMarksFloor(vec2 xz) {
    return sfeValueNoise(xz * 1.3);
}

// How deep the ground is pressed at `xz`, metres (x), and how churned up, 0..1 (y), filtered over `dx` and `dy` (the
// metres a pixel spans, or as far round as wanted), each window as much as it stands there (`cover`, sfeMarksCover).
// Read where the ground's unevenness puts it (sfeMarksWarp, sfeMarksFloor), the same for all that reads the marks.
vec2 sfeMarks(vec2 xz, vec2 dx, vec2 dy, vec2 cover) {
    xz += sfeMarksWarp(xz);
    vec2 marks = vec2(0.0);
    if (cover.y > 0.001 && cover.x < 0.999) {
        float span = matrices.marksSize.y;
        marks = textureGrad(groundMarks, vec3(xz / span, 1.0), dx / span, dy / span).rg * cover.y;
    }
    if (cover.x > 0.001) {
        float span = matrices.marksSize.x;
        marks = mix(marks, textureGrad(groundMarks, vec3(xz / span, 0.0), dx / span, dy / span).rg, cover.x);
    }
    return vec2(marks.x * matrices.marksSize.z * (0.85 + 0.3 * sfeMarksFloor(xz)), marks.y);
}

// The ground's height the marks leave at `xz`, metres: down as deep as it is pressed, and up round that by marksLook.x
// of the depth pressed aside within marksLook.y metres, as mud is pushed up into rims. The ground slumps into what is
// pressed in, so a mark's walls slope over about as far as they go down, not straight down; churned up, the ground is
// loose mud and flows in further, its walls sloping over twice as far as they go down, rounded.
float sfeMarksHeight(vec2 xz, vec2 dx, vec2 dy, vec2 cover) {
    float reach = matrices.marksLook.y;
    vec2 around = sfeMarks(xz, dx + vec2(reach, 0.0), dy + vec2(0.0, reach), cover);
    float loose = smoothstep(matrices.marksChurn.x, matrices.marksChurn.y, around.y);
    float slump = 0.015 + mix(0.8, 2.0, loose) * around.x;
    float depth = sfeMarks(xz, dx + vec2(slump, 0.0), dy + vec2(0.0, slump), cover).x;
    return matrices.marksLook.x * max(around.x - depth, 0.0) - depth;
}

// Which way the marks run at `xz`, a unit vector either way along them: the way their depth, filtered over `reach`
// metres, bends least, from its curvature, so that it holds on a rut's floor, where the depth has no slope, as on its
// walls; round a print it runs round it. `bend` how much the depth bends across, 1/m.
vec2 sfeMarksAlong(vec2 xz, float reach, vec2 cover, out float bend) {
    vec2 r = vec2(reach, 0.0);
    float e = reach;
    float c = sfeMarks(xz, r, r.yx, cover).x;
    float xx = sfeMarks(xz + vec2(e, 0.0), r, r.yx, cover).x + sfeMarks(xz - vec2(e, 0.0), r, r.yx, cover).x - 2.0 * c;
    float zz = sfeMarks(xz + vec2(0.0, e), r, r.yx, cover).x + sfeMarks(xz - vec2(0.0, e), r, r.yx, cover).x - 2.0 * c;
    float dd = sfeMarks(xz + vec2(e, e), r, r.yx, cover).x + sfeMarks(xz - vec2(e, e), r, r.yx, cover).x - 2.0 * c;
    // The curvature [[a, b], [b, d]]; dd is a + 2b + d along the diagonal.
    float a = xx / (e * e);
    float d = zz / (e * e);
    float b = 0.5 * (dd / (e * e) - a - d);
    float mid = 0.5 * (a + d);
    float spread = sqrt(0.25 * (a - d) * (a - d) + b * b);
    float turn = 0.5 * atan(2.0 * b, a - d);
    vec2 axis = vec2(cos(turn), sin(turn)); // the axis of mid + spread
    vec2 across = abs(mid + spread) >= abs(mid - spread) ? axis : vec2(-axis.y, axis.x);
    bend = max(abs(mid + spread), abs(mid - spread));
    return vec2(-across.y, across.x);
}

// How a plant rooted at `xz` stands in the marks, filtered over the reach they push the ground aside within: x how flat
// they laid it, 0 standing to 1 flat, at the depth grass lies flat at; y how much of it is left where they churned the
// ground up. `along` the way they run there, either way along them, shorter the less they bend any way (a print on its
// own, round which a plant is laid as it leans).
vec2 sfeMarksPlant(vec2 xz, out vec2 along) {
    float reach = matrices.marksLook.y;
    vec2 cover = sfeMarksCover(xz);
    vec2 marks = sfeMarks(xz, vec2(reach, 0.0), vec2(0.0, reach), cover);
    float laid = smoothstep(0.0, matrices.marksSize.w, marks.x);
    along = vec2(0.0);
    if (laid > 0.0) {
        float bend;
        along = sfeMarksAlong(xz, reach, cover, bend) * smoothstep(2.0, 8.0, bend);
    }
    return vec2(laid, 1.0 - smoothstep(matrices.marksChurn.z, matrices.marksChurn.w, marks.y));
}

// A point `offset` from a plant's root laid down by `laid` (sfeMarksPlant) along the marks, `along`, to whichever end of
// them its own `lean` (unit, across the ground) is nearer: the higher up, the further over, flat at the end.
vec3 sfeMarksLay(vec3 offset, float laid, vec2 along, vec2 lean) {
    vec2 way = normalize(mix(lean, along * (dot(lean, along) < 0.0 ? -1.0 : 1.0), length(along)) + vec2(1e-5, 0.0));
    float bent = laid * 1.45;
    float h = max(offset.y, 0.0);
    offset.xz += way * h * sin(bent);
    offset.y -= h * (1.0 - cos(bent));
    return offset;
}

// How much of the marks' depth the ground drawn dense near the camera (marksPatch, g_buffer_ground.vs) takes at `xz`:
// all of it but for its outer cell of the terrain's mesh, over which it eases out to none at its edge, where it meets
// the terrain's own mesh. None outside it, or with none.
float sfeGroundPatchTake(vec2 xz) {
    vec4 square = matrices.marksPatch;
    if (square.z <= 0.0) {
        return 0.0;
    }
    vec2 local = xz - square.xy;
    float edge = min(min(local.x, local.y), min(square.z - local.x, square.z - local.y));
    return smoothstep(0.0, matrices.marksGrid.z, edge);
}

#ifdef SFE_MATERIAL_TABLE_GLSL
// How far the terrain's own mesh is put down where the ground is drawn dense, at least: twice the deepest a mark goes,
// so that its cells round the square's edge, sloping down from it, stay under the marks sfeGroundPatchTake eases out.
const float kSfeGroundUnder = 1.0;

// `world` on a mesh of `material`: put out of sight under the ground drawn dense if it is a vertex of the terrain's
// mesh (bit 128 of its map mask) strictly inside that square; those on its edge stay, so that the mesh round it is
// whole.
vec3 sfeGroundUnderPatch(vec3 world, uint material) {
    vec4 square = matrices.marksPatch;
    if (square.z > 0.0 && (uint(sfeMaterials[material].mapMask + 0.5) & 128u) != 0u) {
        vec2 local = world.xz - square.xy;
        float margin = 0.5 * matrices.marksGrid.z;
        if (all(greaterThan(local, vec2(margin))) && all(lessThan(local, vec2(square.z - margin)))) {
            world.y -= max(kSfeGroundUnder, 2.0 * matrices.marksSize.z);
        }
    }
    return world;
}
#endif

#endif
