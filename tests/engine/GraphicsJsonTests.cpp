#include <sstream>
#include <string>

#include <json/reader.h>
#include <json/value.h>

#include "EngineTestFixture.h"
#include "systemsModule/GraphicsSettingsGui.h"

// The graphics settings as JSON and back (graphicsToJson / graphicsFromJson): what an editor sends with EditorProtocol's
// SetGraphics. Every field by its name, and only GraphicsSettings' own -- not the grass, the vegetation's reach or the
// air life, which are an application's numbers.
namespace SFE::EngineTests {
	namespace {
		Json::Value parsed(const std::string& text) {
			Json::Value root;
			Json::CharReaderBuilder builder;
			std::string errors;
			std::istringstream stream(text);
			EXPECT_TRUE(Json::parseFromStream(builder, stream, &root, &errors)) << errors;
			return root;
		}
	}

	// Settings off their defaults, written and read back into defaults, are written the same: no field is lost or
	// changed on the way. A field the settings lack is passed over, and text that is not JSON changes nothing.
	TEST(GraphicsJson, RoundTrips) {
		using SystemsModule::GraphicsSettings;
		GraphicsSettings set;
		set.ssao = false;
		set.dof = true;
		set.exposure = 0.75f;
		set.warmth = -0.3f;
		set.stylise = 0.5f;
		set.timeOfDay = 13.25f;
		set.sssRadius = 0.004f;
		set.lodPixels = 3.5f;
		const std::string json = SystemsModule::graphicsToJson(set);
		GraphicsSettings back;
		ASSERT_TRUE(SystemsModule::graphicsFromJson(json, back));
		EXPECT_EQ(SystemsModule::graphicsToJson(back), json) << "a field did not come back as it went";
		EXPECT_FALSE(back.ssao);
		EXPECT_TRUE(back.dof);
		EXPECT_EQ(back.exposure, 0.75f);
		EXPECT_EQ(back.warmth, -0.3f);
		EXPECT_EQ(back.stylise, 0.5f);
		EXPECT_EQ(back.timeOfDay, 13.25f);
		EXPECT_EQ(back.sssRadius, 0.004f);
		EXPECT_EQ(back.lodPixels, 3.5f);

		const auto root = parsed(json);
		ASSERT_TRUE(root.isObject());
		EXPECT_EQ(root.size(), 50u);
		for (const char* gone : { "grassDensity", "grassDistance", "lodDetail", "airLife", "airLifeAmount" }) {
			EXPECT_FALSE(root.isMember(gone)) << gone << " is written, though GraphicsSettings has it no longer";
		}

		GraphicsSettings unknown;
		ASSERT_TRUE(SystemsModule::graphicsFromJson(std::string(R"({"exposure": 0.5, "grassDensity": 3, "nobody": 1})"), unknown));
		GraphicsSettings expected;
		expected.exposure = 0.5f;
		EXPECT_EQ(SystemsModule::graphicsToJson(unknown), SystemsModule::graphicsToJson(expected))
			<< "a member the settings lack changed them";

		GraphicsSettings untouched;
		untouched.exposure = 2.f;
		EXPECT_FALSE(SystemsModule::graphicsFromJson(std::string(R"({"exposure": 0.3)"), untouched));
		EXPECT_FALSE(SystemsModule::graphicsFromJson(std::string(), untouched));
		EXPECT_EQ(untouched.exposure, 2.f) << "text that is not JSON changed the settings";
	}
}
