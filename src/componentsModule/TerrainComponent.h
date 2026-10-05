#pragma once

#include <vector>

#include "ecss/Types.h"
#include "mathModule/Forward.h"

namespace SFE::ComponentsModule {
	// How worn a footpath or patch is where the scene does not say: 0 grass walked on now and then, 1 trodden bare.
	constexpr float kDefaultWear = 0.5f;

	// A footpath: a smooth curve through x/z points in the terrain's own space, `width` metres across, worn `wear`.
	struct TerrainPath {
		std::vector<Math::Vec2> points;
		float width = 1.2f;
		float wear = kDefaultWear;
	};

	// Ground built from a height function around a lake basin. The mesh is generated from these values when the
	// scene loads; local y = 0 is the water line, so a WaterComponent at the same height fills the basin.
	struct TerrainComponent {
		float size = 420.f;       // metres along each side
		int resolution = 321;     // vertices along each side
		// The lake's mean extent across (along x) and along (along z); its shore wanders in and out of that oval.
		float lakeWidth = 140.f;
		float lakeLength = 200.f;
		float lakeDepth = 7.f;
		float hillHeight = 16.f;
		int seed = 7;
		// A dirt road's course, x/z in the terrain's own space: a smooth curve through these points, levelled into
		// the ground, drawn as a track and kept clear of vegetation. Empty for none.
		std::vector<Math::Vec2> road;
		// Round clearings: x/z of the middle and the radius. Levelled to the height the middle had and kept clear of
		// vegetation: a meadow walked short.
		std::vector<Math::Vec3> clearings;
		// Footpaths, and round patches of trampled ground (x/z of the middle, the radius and how worn, as a path's
		// wear). Painted into the ground, the grass on them thin and short and the earth through it, bare where they are
		// worn most; kept clear of vegetation, but not levelled: they lie on the ground as it is.
		std::vector<TerrainPath> paths;
		std::vector<Math::Vec4> patches;
	};
}

template<> struct ecss::AllowNonTrivial<SFE::ComponentsModule::TerrainComponent> : std::true_type {};

using SFE::ComponentsModule::TerrainComponent;
