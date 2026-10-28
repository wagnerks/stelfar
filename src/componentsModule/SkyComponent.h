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

		// The sun's path through a day (setTimeOfDay, timeOfDay): `latitude` degrees north, the sun `declination` degrees
		// north of the equator, on a clock that has it highest at `noon`. By default the equinox on the equator: up due
		// east at 6, overhead at 12, down due west at 18. Azimuth from the north by the east.
		float latitude = 0.f;
		float declination = 0.f;
		float noon = 12.f;
		// The ground under the horizon as the sky's light sees it (IblBaker): its albedo, lit by the sun and the sky. By
		// default a grey.
		Math::Vec3 groundAlbedo{ 0.2f, 0.2f, 0.2f };
		void setTimeOfDay(const float hours) {
			constexpr float rad = 3.14159265f / 180.f;
			const float hour = (hours - noon) * 15.f * rad;
			const float lat = latitude * rad;
			const float dec = declination * rad;
			const float up = std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(hour);
			sunElevation = std::asin(std::clamp(up, -1.f, 1.f)) / rad;
			sunAzimuth = std::atan2(std::sin(hour), std::cos(hour) * std::sin(lat) - std::tan(dec) * std::cos(lat)) / rad + 180.f;
		}
		// The hour of the day this sun stands at: its hour angle, from where it is in the sky.
		float timeOfDay() const {
			constexpr float rad = 3.14159265f / 180.f;
			const float elevation = sunElevation * rad;
			const float azimuth = sunAzimuth * rad;
			const float lat = latitude * rad;
			const float hour = std::atan2(-std::cos(elevation) * std::sin(azimuth),
				std::sin(elevation) * std::cos(lat) - std::cos(elevation) * std::cos(azimuth) * std::sin(lat));
			return std::fmod(noon + hour / rad / 15.f + 24.f, 24.f);
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
