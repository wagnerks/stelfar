#include <cmath>
#include <optional>
#include <vector>

#include <json/value.h>

#include "EngineTestFixture.h"
#include "componentsModule/SkyComponent.h"
#include "propertiesModule/SceneComponents.h"
#include "renderModule/IblBaker.h"

namespace SFE::EngineTests {
	namespace {
		// The sun's path the scenes give it: 54.6 degrees north, the sun 23.4 north of the equator, highest at half past
		// 12. Where it stands through the day is worked out below in double precision; the component works in float, a
		// hundred times closer than this.
		constexpr float kDegrees = 1e-3f;

		ComponentsModule::SkyComponent sunAt(const float hours) {
			ComponentsModule::SkyComponent sky;
			sky.latitude = 54.6f;
			sky.declination = 23.4f;
			sky.noon = 12.5f;
			sky.setTimeOfDay(hours);
			return sky;
		}

		// How bright, all channels together, the irradiance cube of a bake is in the middle of `face` (+x -x +y -y +z -z):
		// its four texels round the face's centre.
		float irradianceAtTheMiddle(const Render::IblTexels& texels, const int face) {
			const auto side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(texels.irradiance.size()) / 24.0)));
			float sum = 0.f;
			for (const int y : { side / 2 - 1, side / 2 }) {
				for (const int x : { side / 2 - 1, side / 2 }) {
					const size_t at = ((static_cast<size_t>(face) * side + y) * side + x) * 4;
					sum += texels.irradiance[at] + texels.irradiance[at + 1] + texels.irradiance[at + 2];
				}
			}
			return sum;
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

	// Without a path of the scene's, the equinox on the equator: up due east at 6, overhead at 12, down due west at 18.
	TEST(SunPath, ByDefaultTheEquinoxOnTheEquator) {
		const auto at = [](const float hours) {
			ComponentsModule::SkyComponent sky;
			sky.setTimeOfDay(hours);
			return sky;
		};
		EXPECT_NEAR(at(12.f).sunElevation, 90.f, kDegrees);
		EXPECT_NEAR(at(6.f).sunElevation, 0.f, kDegrees);
		EXPECT_NEAR(at(6.f).sunAzimuth, 90.f, kDegrees);
		EXPECT_NEAR(at(18.f).sunElevation, 0.f, kDegrees);
		EXPECT_NEAR(at(18.f).sunAzimuth, 270.f, kDegrees);
		EXPECT_NEAR(at(9.f).timeOfDay(), 9.f, 1e-3f);
		const ComponentsModule::SkyComponent sky;
		EXPECT_EQ(sky.groundAlbedo.x, 0.2f);
		EXPECT_EQ(sky.groundAlbedo.y, 0.2f);
		EXPECT_EQ(sky.groundAlbedo.z, 0.2f);
	}

	// A scene carries the sun's path and the ground's colour under the horizon, read as the very floats its decimals
	// are and written back under the same names; a scene without them leaves the defaults.
	TEST(SunPath, ASceneCarriesItsPath) {
		using Serializer = PropertiesModule::ComponentSerializer<ComponentsModule::SkyComponent>;
		auto& registry = ECSHandler::registry();
		const auto entity = registry.takeEntity();
		const Finally takeOut([&] {
			ECSHandler::destroyEntity(entity);
			step();
		});
		Json::Value data;
		data["latitude"] = 54.6;
		data["declination"] = 23.4;
		data["noon"] = 12.5;
		for (const double v : { 0.1, 0.12, 0.07 }) {
			data["ground_albedo"].append(v);
		}
		Serializer::load(entity, data);
		{
			const auto sky = registry.pinComponent<const ComponentsModule::SkyComponent>(entity);
			ASSERT_TRUE(sky);
			EXPECT_EQ(sky->latitude, 54.6f);
			EXPECT_EQ(sky->declination, 23.4f);
			EXPECT_EQ(sky->noon, 12.5f);
			EXPECT_EQ(sky->groundAlbedo.x, 0.1f);
			EXPECT_EQ(sky->groundAlbedo.y, 0.12f);
			EXPECT_EQ(sky->groundAlbedo.z, 0.07f);
			Json::Value saved;
			Serializer::save(*sky, saved);
			EXPECT_EQ(saved["latitude"].asFloat(), 54.6f);
			EXPECT_EQ(saved["declination"].asFloat(), 23.4f);
			EXPECT_EQ(saved["noon"].asFloat(), 12.5f);
			ASSERT_TRUE(saved["ground_albedo"].isArray() && saved["ground_albedo"].size() == 3u);
			EXPECT_EQ(saved["ground_albedo"][1].asFloat(), 0.12f);
		}
		Serializer::load(entity, Json::Value(Json::objectValue));
		const auto sky = registry.pinComponent<const ComponentsModule::SkyComponent>(entity);
		ASSERT_TRUE(sky);
		const ComponentsModule::SkyComponent neutral;
		EXPECT_EQ(sky->latitude, neutral.latitude);
		EXPECT_EQ(sky->declination, neutral.declination);
		EXPECT_EQ(sky->noon, neutral.noon);
		EXPECT_EQ(sky->groundAlbedo.x, neutral.groundAlbedo.x);
		EXPECT_EQ(sky->groundAlbedo.y, neutral.groundAlbedo.y);
		EXPECT_EQ(sky->groundAlbedo.z, neutral.groundAlbedo.z);
	}

	// The sky's light from below the horizon is the ground's colour lit by the sun and the sky (IblBaker): a brighter
	// ground lights what faces down more, and leaves what faces up as it was.
	TEST(SunPath, TheGroundUnderTheHorizonIsTheScenes) {
		ComponentsModule::SkyComponent dark;
		dark.sunElevation = 30.f;
		dark.groundAlbedo = { 0.f, 0.f, 0.f };
		auto bright = dark;
		bright.groundAlbedo = { 0.5f, 0.5f, 0.5f };
		const auto underDark = Render::computeIbl(dark, 1.f);
		const auto underBright = Render::computeIbl(bright, 1.f);
		constexpr int kUp = 2;
		constexpr int kDown = 3;
		EXPECT_GT(irradianceAtTheMiddle(underBright, kDown), irradianceAtTheMiddle(underDark, kDown));
		EXPECT_EQ(irradianceAtTheMiddle(underBright, kUp), irradianceAtTheMiddle(underDark, kUp));
	}
}
