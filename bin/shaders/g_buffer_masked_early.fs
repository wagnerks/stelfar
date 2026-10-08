#version 450 core
// g_buffer_masked.fs over the depth g_buffer_masked_depth.fs laid: GeometryPass draws it where that depth is its own,
// with the test before the shading, so a pixel of leaves is shaded once however many lie over it.
#include "sfe_gbuffer_masked.glsl"
layout(early_fragment_tests) in;
