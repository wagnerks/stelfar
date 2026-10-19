#version 450 core
#include "sfe.glsl"
// After gpu_cull.cs: one workgroup per draw group packs the group's commands with instances to the start of its range,
// in their order (the G-buffer's nearest first stays so), and writes how many to its count, which the draw takes its
// count from. Every batch of every level of every mesh has a command, most with nothing in them, and an empty
// command sent to the GPU costs about what a small draw does: tens of thousands of them a frame.
layout(local_size_x = 256) in;

struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout(SFE_SET(0, 30), std430) coherent buffer Commands {
    DrawCommand commands[];
};
layout(SFE_SET(0, 34), std430) writeonly buffer Counts {
    uint counts[];
};
// ranges: per group its first batch and how many; the commands of this phase start commandOffset after them, and its
// counts countOffset into Counts.
layout(SFE_SET(0, 35), std430) readonly buffer Groups {
    uint groupCount;
    uint commandOffset;
    uint countOffset;
    uint pad;
    uvec2 ranges[];
} groups;

shared uint sums[256];

void main() {
    uint g = gl_WorkGroupID.x;
    uint t = gl_LocalInvocationID.x;
    uint first = groups.ranges[g].x + groups.commandOffset;
    uint n = groups.ranges[g].y;
    uint kept = 0u;
    for (uint base = 0u; base < n; base += 256u) {
        uint i = base + t;
        DrawCommand command = DrawCommand(0u, 0u, 0u, 0, 0u);
        bool live = false;
        if (i < n) {
            command = commands[first + i];
            live = command.instanceCount > 0u;
        }
        // Each one's place among the live ones of this stretch: written at most where it was read, and never past the
        // stretch, so what a later stretch reads is untouched.
        sums[t] = live ? 1u : 0u;
        memoryBarrierBuffer();
        barrier();
        for (uint step = 1u; step < 256u; step <<= 1u) {
            uint add = t >= step ? sums[t - step] : 0u;
            barrier();
            sums[t] += add;
            barrier();
        }
        if (live) {
            commands[first + kept + sums[t] - 1u] = command;
        }
        kept += sums[255];
        barrier();
    }
    if (t == 0u) {
        counts[groups.countOffset + g] = kept;
    }
}
