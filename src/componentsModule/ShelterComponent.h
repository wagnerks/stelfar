#pragma once

#include <algorithm>
#include <cmath>

#include "mathModule/Forward.h"

namespace SFE::ComponentsModule {
	// A roof over a place -- a tent, a shed: under it the sky's light comes in only through the canvas and the door, so
	// what is inside is lit far less by the sky than out in the open (deferred_shading.fs), and the eye inside makes up
	// only part of that dark (sfe_tonemap.glsl). The place stands on the entity's origin: walls `size.x` apart and
	// `size.z` long, a roof sloping from the ridge `size.y` up down to the eaves over its width, as a ridge tent's.
	struct ShelterComponent {
		Math::Vec3 size{ 2.f, 2.f, 2.f }; // metres: x width, y the ridge's height, z length
		float eaves = 2.f;                // metres up where the roof meets the walls
		float sky = 0.25f;                // the share of the sky's light that comes in

		// How much of a shelter's dark the eye makes up, in stops: 1 all of it, as auto exposure does out in the open.
		static inline float eyeAdapts = 0.5f;
		// Metres from the walls and the roof over which the inside comes in, here and in deferred_shading.fs.
		static constexpr float kSoft = 0.35f;

		// How far in `local` (the entity's own space, metres) is: 0 outside, 1 kSoft or more from the walls and the roof.
		float inside(const Math::Vec3& local) const {
			const float halfWide = (std::max)(size.x * 0.5f, 1e-3f);
			const float across = std::abs(local.x);
			const float roof = eaves + (size.y - eaves) * (1.f - across / halfWide);
			const float depth = (std::min)({ halfWide - across, size.z * 0.5f - std::abs(local.z), roof - local.y });
			const float t = std::clamp(depth / kSoft, 0.f, 1.f);
			return t * t * (3.f - 2.f * t);
		}
	};
}

using SFE::ComponentsModule::ShelterComponent;
