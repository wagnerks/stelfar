#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ecss/Types.h"

namespace SFE::ComponentsModule {
	// Ground of heights: `samples` x `samples` heights `size` metres across, centred on the entity's origin, row by
	// row along z from -size/2, each row along x from -size/2, size / (samples - 1) apart, in the entity's local space;
	// it stands unturned and unscaled. The physics collides with it (a TerrainCollider field, PhysicsSystem), with or
	// without a model; it is drawn only by a model made of it (AssetsModule::heightfieldModel); its height between
	// samples is the triangle drawn there (AssetsModule::heightfieldHeight). Not read from a scene: whatever makes the
	// heights attaches it.
	struct HeightfieldComponent {
		int samples = 0;
		float size = 0.f;
		std::shared_ptr<const std::vector<float>> heights;
		// What the heights are made from: the same key for the same heights. What is built of them goes by it -- its
		// mesh's levels of detail on disk (heightfieldModel).
		uint64_t key = 0;
	};
}

template<> struct ecss::AllowNonTrivial<SFE::ComponentsModule::HeightfieldComponent> : std::true_type {};
