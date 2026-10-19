#pragma once

#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include "mathModule/Forward.h"

namespace SFE::SystemsModule::TerrainCollider {
	// The terrain's collider (PhysicsSystem): its heights (AssetsModule::terrainHeights, n x n samples `step` apart
	// from `corner`, the world place of the first, row by row along z, over corner.y) as a Jolt height field on the
	// very triangles the terrain mesh is drawn with. The mesh splits each cell along the diagonal from its +x corner to
	// its +z one (terrainSurfaceHeight), Jolt the other way, so the field is laid turned a quarter round about y. Where
	// something needs finer ground than the terrain's cells, a square of them is cut out of it and a finer field of its
	// own laid there (fine).
	JPH::ShapeRefC shape(const std::vector<float>& heights, int n, float step, const Math::Vec3& corner);

	// Where the terrain of a shape made by `shape` lies.
	struct Grid {
		int n = 0;
		float step = 0.f;
		Math::Vec3 corner;
	};
	// The field of a shape made by `shape`, cut into in place; null for any other shape.
	JPH::HeightFieldShape* field(const JPH::Shape& shape);
	Grid gridOf(const JPH::Shape& shape);
	// Whether `shape` is the ground: the terrain's (shape) or a finer field laid on it (fine).
	bool isGround(const JPH::Shape& shape);

	// The square `cells` across from cell (x, z) (as the terrain's samples are counted, x along x and z along z) cut out
	// of `field` for good: its samples, its edge's too, made holes, so every triangle with a corner in it goes -- all of
	// the square's and some of the ring of cells round it. Not while anything queries the field.
	void cut(JPH::HeightFieldShape& field, const Grid& grid, int x, int z, int cells, JPH::TempAllocator& allocator);

	// The finer field laid where such a square is cut out: over the square and the ring of cells round it (as much of
	// it as the cut takes, and the rest where it is the terrain's own), `per` samples to a cell from cell (x - 1, z - 1):
	// `heights` are samples(cells, per)^2 world heights row by row along z. Null if they are not as many. Any thread.
	// It can be given ground down to `sink` lower than the lowest of them later (setHeights), no lower.
	int samples(int cells, int per);
	JPH::ShapeRefC fine(const Grid& grid, int x, int z, int cells, int per, const std::vector<float>& heights, float sink);
	// Part of a field made by `fine` given anew: `heights`, `width` x `height` samples of it from its sample (u, v), row by
	// row. Where in the world the ground moved (none if nowhere), from the lowest it was or is to the highest. Not while
	// anything queries the field.
	JPH::AABox setHeights(const JPH::Shape& fine, int u, int v, int width, int height, const std::vector<float>& heights,
		JPH::TempAllocator& allocator);
}
