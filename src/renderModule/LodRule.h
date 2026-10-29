#pragma once

#include <algorithm>
#include <cstdint>

// The scene's rule of which level of a mesh's chain is drawn (gpu_cull.cs), for a pass that picks the levels of its own
// instances on the CPU: the very text of shaders/sfe_lod.glsl compiled as C++, so the two cannot part.
// sfeLodAllowed(lodPixels, bias, radius, pixelsPerUnit, distance, texelsPerMetre) is the share of a mesh's extent a
// level may stray by, sfeLodLevel(errors of levels 1 on, lodCount, allowed) the coarsest level within it.
namespace SFE::Render::LodRule {
#pragma push_macro("max")
#undef max
	using uint = std::uint32_t;
	using std::max;
#define SFE_LOD_FN inline
#include "../../bin/shaders/sfe_lod.glsl"
#undef SFE_LOD_FN
#pragma pop_macro("max")
}
