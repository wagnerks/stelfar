#pragma once

#include <algorithm>
#include <cmath>

#include "mathModule/Forward.h"
#include "mathModule/Quaternion.h"

namespace SFE::ComponentsModule {
	// An outdoor sky: the atmosphere drawn behind the scene and baked into the ambient light, and the sun that lights
	// and shadows it. A scene without one keeps the studio environment and a black background.
	struct SkyComponent {
		// Degrees. Elevation above the horizon; azimuth turns the sun around the vertical axis.
		float sunElevation = 14.f;
		float sunAzimuth = 35.f;
		// 0 a clear sky, 1 overcast.
		float cloudCoverage = 0.45f;
		// Mist lying on the water: its density at the water line per metre, and the metres up over which it thins to
		// 1/e of that. 0 for none.
		float mist = 0.f;
		float mistHeight = 4.f;
		// Haze over any distance, per metre. 0 for none.
		float haze = 0.f;

		// The sun's path through a midsummer day kLatitude degrees north, the sun kDeclination north of the equator, on
		// a clock that has it highest at kNoon: up in the north-east at 4, highest (59 degrees) in the south at half
		// past 12, down in the north-west at 21, and not far under the horizon through the short night. Azimuth from the
		// north by the east.
		static constexpr float kLatitude = 54.6f;
		static constexpr float kDeclination = 23.4f;
		static constexpr float kNoon = 12.5f;
		void setTimeOfDay(const float hours) {
			constexpr float rad = 3.14159265f / 180.f;
			const float hour = (hours - kNoon) * 15.f * rad;
			const float latitude = kLatitude * rad;
			const float declination = kDeclination * rad;
			const float up = std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hour);
			sunElevation = std::asin(std::clamp(up, -1.f, 1.f)) / rad;
			sunAzimuth = std::atan2(std::sin(hour), std::cos(hour) * std::sin(latitude) - std::tan(declination) * std::cos(latitude)) / rad + 180.f;
		}
		// The hour of the day this sun stands at: its hour angle, from where it is in the sky.
		float timeOfDay() const {
			constexpr float rad = 3.14159265f / 180.f;
			const float elevation = sunElevation * rad;
			const float azimuth = sunAzimuth * rad;
			const float latitude = kLatitude * rad;
			const float hour = std::atan2(-std::cos(elevation) * std::sin(azimuth),
				std::sin(elevation) * std::cos(latitude) - std::cos(elevation) * std::cos(azimuth) * std::sin(latitude));
			return std::fmod(kNoon + hour / rad / 15.f + 24.f, 24.f);
		}

		// The rotation the sun's transform takes: its forward is the direction the light travels.
		Math::Vec3 sunRotation() const { return { -sunElevation, sunAzimuth, 0.f }; }

		// The moon stands opposite the sun: it rises as the sun sets, which is a full moon and the brightest night
		// there is. Its light is the sun's, so its transform and direction are the sun's turned round.
		Math::Vec3 moonRotation() const { return { sunElevation, sunAzimuth + 180.f, 0.f }; }

		// Towards the sun, the way the transform with sunRotation() computes its forward.
		Math::Vec3 toSun() const {
			const auto m = Math::Quaternion<float>::fromEulerDegrees(sunRotation()).toMat4();
			return Math::normalize(Math::Vec3{ m[2][0], m[2][1], m[2][2] });
		}
	};
}

using SFE::ComponentsModule::SkyComponent;
