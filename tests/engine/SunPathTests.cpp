#include <cmath>

#include <gtest/gtest.h>

#include "componentsModule/SkyComponent.h"

namespace SFE::EngineTests {
	namespace {
		// The sun's path the scenes give it: 54.6 degrees north, the sun 23.4 north of the equator, highest at half past
		// 12. Where it stands through the day is worked out below in double precision; the component works in float, a
		// hundred times closer than this.
		constexpr float kDegrees = 1e-3f;

		ComponentsModule::SkyComponent sunAt(const float hours) {
			ComponentsModule::SkyComponent sky;
			sky.setTimeOfDay(hours);
			return sky;
		}

		struct SunAt {
			float hours;
			float elevation;
			float azimuth;
		};
		constexpr SunAt kDay[] = {
			{ 0.f, -11.73371f, 352.97239f },
			{ 4.f, 0.0049f, 46.72766f },
			{ 6.f, 14.7341f, 70.19331f },
			{ 9.f, 40.34338f, 107.19717f },
			{ 12.5f, 58.8f, 180.f },
			{ 16.f, 40.34338f, 252.80283f },
			{ 19.f, 14.7341f, 289.80669f },
			{ 21.f, 0.0049f, 313.27234f },
			{ 22.f, -5.62692f, 325.84741f },
			{ 23.5f, -10.94091f, 345.99953f },
		};
	}

	// Highest at half past 12, due south, as high as the latitude and the declination put it: 90 - 54.6 + 23.4.
	TEST(SunPath, HighestAtHalfPastTwelveDueSouth) {
		const auto noon = sunAt(12.5f);
		EXPECT_NEAR(noon.sunElevation, 58.8f, kDegrees);
		EXPECT_NEAR(noon.sunAzimuth, 180.f, kDegrees);
		EXPECT_LT(sunAt(12.4f).sunElevation, noon.sunElevation);
		EXPECT_LT(sunAt(12.6f).sunElevation, noon.sunElevation);
	}

	// Up in the north-east just before 4, down in the north-west just after 21.
	TEST(SunPath, UpInTheNorthEastAtFourDownInTheNorthWestAtTwentyOne) {
		EXPECT_LT(sunAt(3.9f).sunElevation, 0.f);
		EXPECT_GT(sunAt(4.1f).sunElevation, 0.f);
		EXPECT_GT(sunAt(20.9f).sunElevation, 0.f);
		EXPECT_LT(sunAt(21.1f).sunElevation, 0.f);
		const auto rise = sunAt(4.f);
		EXPECT_GT(rise.sunAzimuth, 0.f);
		EXPECT_LT(rise.sunAzimuth, 90.f);
		const auto set = sunAt(21.f);
		EXPECT_GT(set.sunAzimuth, 270.f);
		EXPECT_LT(set.sunAzimuth, 360.f);
	}

	// Through the day, and the short night not far under the horizon.
	TEST(SunPath, TheDayAsItGoes) {
		for (const auto& at : kDay) {
			const auto sky = sunAt(at.hours);
			EXPECT_NEAR(sky.sunElevation, at.elevation, kDegrees) << at.hours << " o'clock";
			EXPECT_NEAR(sky.sunAzimuth, at.azimuth, kDegrees) << at.hours << " o'clock";
		}
	}

	// timeOfDay reads back the hour the sun was set to, the day coming round at 24.
	TEST(SunPath, TimeOfDayReadsTheHourBack) {
		for (const auto& at : kDay) {
			EXPECT_NEAR(std::remainder(sunAt(at.hours).timeOfDay() - at.hours, 24.f), 0.f, 1e-3f) << at.hours << " o'clock";
		}
	}
}
