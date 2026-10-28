#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SFE { struct Material; }
namespace SFE::ComponentsModule { struct HeightfieldComponent; }

namespace AssetsModule {
	class Model;

	// A heightfield's mesh is cut into square chunks of this many cells a side, each a mesh with its own levels of
	// detail.
	inline constexpr int kTerrainChunkCells = 32;
	// The engine's shader for a heightfield's mesh (bin/shaders/g_buffer_terrain.fs): tiling layers of the texture
	// arrays in its material's DIFFUSE (rgb colour, a height) and NORMALS (rg normal, b roughness, a occlusion), the
	// first where the ground is level, the second where it is steep. An application stands in for it with its own file
	// of the same name in its content's shaders/.
	inline constexpr const char* kTerrainFS = "shaders/g_buffer_terrain.fs";

	// The height of a heightfield's mesh at x/z in its local space -- `heights` as HeightfieldComponent holds them,
	// `samples` a side over `size` metres -- on the very triangle drawn there, so whatever is stood on it neither floats
	// nor sinks: each cell split along the diagonal from its +x corner to its +z one. Its gradient comes back through
	// `slopeX` and `slopeZ` when asked for. 0 for fewer than 2 samples or fewer heights than samples squared.
	float heightfieldHeight(const std::vector<float>& heights, int samples, float size, float x, float z,
		float* slopeX = nullptr, float* slopeZ = nullptr);

	// The mesh of `field`, kept as the asset `assetPath` (that asset if it is there already): chunks of
	// kTerrainChunkCells cells, each a mesh with `material` and its own bounds and levels of detail (kept on disk by
	// the field's key: cache/terrain_chunk_lods2_<key ^ kTerrainChunkCells>), put in from the middle out, each in front
	// of those before it, so the model lists them from the outside in, the one round the middle last. Its texCoords
	// carry the local x/z in metres, texCoords1.x the height: what kTerrainFS shades from. Null if the field has fewer
	// than 2 samples or fewer heights than samples squared.
	Model* heightfieldModel(const SFE::ComponentsModule::HeightfieldComponent& field, const SFE::Material& material,
		const std::string& assetPath);
}
