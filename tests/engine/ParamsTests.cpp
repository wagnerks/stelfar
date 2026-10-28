#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "EngineTestFixture.h"
#include "componentsModule/ShelterComponent.h"
#include "core/Params.h"
#include "renderModule/DebugView.h"
#include "renderModule/renderPasses/SunShadowPass.h"
#include "systemsModule/systems/AudioSystem.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::EngineTests {
	namespace {
		// A root of its own for one test, outside every real one, holding these numbers if any (an engine's root behind it,
		// empty) in a file `name`, which it names the application's numbers file (Params::setFile); set as FileSystem's
		// roots, and the roots and the file named put back as they were when the test ends. The registry is the one the
		// whole run shares: each test keeps to groups of its own, "Test Params ...".
		class TempRoot {
		public:
			explicit TempRoot(const std::string& numbers = {}, std::string name = "numbers.json") : mWasRoot(FileSystem::root()),
				mWasEngine(FileSystem::engineRoot()), mWasFile(Params::file()), mName(std::move(name)),
				mDir(std::filesystem::temp_directory_path()
					/ ("sfe_params_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
				ThreadPool::instance()->waitIdle();
				std::filesystem::create_directories(mDir / "content");
				std::filesystem::create_directories(mDir / "engine");
				FileSystem::setRoot((mDir / "content").string());
				FileSystem::setEngineRoot((mDir / "engine").string());
				Params::setFile(mName);
				if (!numbers.empty()) {
					std::ofstream(file(), std::ios::binary | std::ios::trunc) << numbers;
				}
			}

			~TempRoot() {
				ThreadPool::instance()->waitIdle();
				Params::setFile(mWasFile);
				FileSystem::setRoot(mWasRoot.string());
				FileSystem::setEngineRoot(mWasEngine.string());
				std::error_code ec;
				std::filesystem::remove_all(mDir, ec);
			}

			TempRoot(const TempRoot&) = delete;
			TempRoot& operator=(const TempRoot&) = delete;

			std::filesystem::path content() const { return mDir / "content"; }
			std::filesystem::path file() const { return content() / mName; }
			// The file as save left it.
			Json::Value saved() const { return FileSystem::readJson(file().string()); }

		private:
			std::filesystem::path mWasRoot;
			std::filesystem::path mWasEngine;
			std::string mWasFile;
			std::string mName;
			std::filesystem::path mDir;
		};

		// A file's bytes as they are; empty for none.
		std::string bytesOf(const std::filesystem::path& path) {
			std::ifstream in(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		Params::Seen seenOf(const std::string& group, const std::string& name) {
			Params::Seen seen{};
			EXPECT_TRUE(Params::see(group, name, seen)) << group << " / " << name << " is not registered";
			return seen;
		}
	}

	// One group and name with one initial is one number, wherever it is registered from; another initial, or another
	// type, is another number, named apart.
	TEST(Params, OneNameAndInitialIsOneNumber) {
		float& first = Params::value("Test Params same", "n", 2.f);
		float& again = Params::value("Test Params same", "n", 2.f);
		float& other = Params::value("Test Params same", "n", 3.f);
		int& whole = Params::value("Test Params same", "n", 2);
		EXPECT_EQ(&first, &again);
		EXPECT_NE(&first, &other);
		EXPECT_EQ(Params::findFloat("Test Params same", "n"), &first);
		EXPECT_EQ(Params::findFloat("Test Params same", "n #2"), &other);
		EXPECT_EQ(seenOf("Test Params same", "n #3").type, Params::Type::Int);
		EXPECT_EQ(seenOf("Test Params same", "n #3").value, 2.0);
		EXPECT_NE(static_cast<void*>(&whole), static_cast<void*>(&first)) << "an int took a float's number";
	}

	// A second bind of a bound name is ignored: the first storage stays the number's.
	TEST(Params, ASecondBindIsIgnored) {
		static float first = 1.f;
		static float second = 1.f;
		Params::bind("Test Params bind", "n", first);
		Params::bind("Test Params bind", "n", second);
		EXPECT_EQ(Params::findFloat("Test Params bind", "n"), &first);
		Params::unbind(&first, &first + 1);
		Params::unbind(&second, &second + 1);
	}

	// The file's numbers become the type the code registered: an int rounded, a bool true for anything but nothing; what
	// is not a number, true or false, or is past what the type holds, is no value, and leaves the code's. A number
	// registered after the file was read takes the file's value as it comes.
	TEST(Params, TheFileIsReadAsTheCodeHasEachNumber) {
		const TempRoot root(R"({"Test Params types": {"int": 30.0, "late": 4.6, "one": 1, "true": true, "float": 5, "text": "no", "huge": 1e20}})");
		int& whole = Params::value("Test Params types", "int", 7);
		bool& one = Params::value("Test Params types", "one", false);
		bool& yes = Params::value("Test Params types", "true", false);
		float& real = Params::value("Test Params types", "float", 1.f);
		float& text = Params::value("Test Params types", "text", 2.f);
		int& huge = Params::value("Test Params types", "huge", 3);
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(whole, 30);
		EXPECT_TRUE(one);
		EXPECT_TRUE(yes);
		EXPECT_EQ(real, 5.f);
		EXPECT_EQ(text, 2.f) << "a string was read as a number";
		EXPECT_EQ(huge, 3) << "an int took a value past any int";
		EXPECT_EQ(Params::value("Test Params types", "late", 0), 5) << "registered after the read, it did not get the file's";
	}

	// No file, or one that does not parse, changes nothing: load says so, and every number is as it was.
	TEST(Params, NoFileOrABadOneChangesNothing) {
		float& number = Params::value("Test Params no file", "n", 1.f);
		number = 2.f;
		{
			const TempRoot root;
			EXPECT_FALSE(Params::load());
			EXPECT_EQ(number, 2.f);
		}
		{
			const TempRoot root(R"({"Test Params no file": {"n": 3)");
			EXPECT_FALSE(Params::load());
			EXPECT_EQ(number, 2.f) << "a file that does not parse was read in part";
		}
		number = 1.f;
	}

	// A number of this run only is never read from the file, and a reload leaves it as it is.
	TEST(Params, ATransientNumberIsNotTheFilesToSet) {
		const TempRoot root(R"({"Test Params transient": {"t": 5}})");
		float& number = Params::value("Test Params transient", "t", 1.f, { .transient = true });
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(number, 1.f);
		number = 3.f;
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(number, 3.f);
		number = 1.f;
	}

	// What differs from the code is merged into the file as it stands: a key nothing registered keeps its value, one back
	// at the code's goes, a number of this run only is never written, and one held for the session is written as it is
	// besides -- the value from before the session. Ints are written as ints and bools as bools.
	TEST(Params, SaveMergesIntoTheFile) {
		const TempRoot root(R"({"Test Params save": {"nobody": 9, "same": 4}, "Test Params save elsewhere": {"x": 1}})");
		float& changed = Params::value("Test Params save", "changed", 1.f);
		float& same = Params::value("Test Params save", "same", 1.f);
		float& thisRun = Params::value("Test Params save", "this run", 1.f, { .transient = true });
		float& held = Params::value("Test Params save", "held", 1.f);
		int& count = Params::value("Test Params save", "count", 1);
		bool& on = Params::value("Test Params save", "on", false);
		ASSERT_TRUE(Params::load());
		ASSERT_EQ(same, 4.f);
		changed = 2.f;
		same = 1.f;
		thisRun = 5.f;
		held = 3.f;
		ASSERT_TRUE(Params::setSession("Test Params save", "held", 7.0, Params::Session::UntilReload));
		count = 4;
		on = true;
		Params::save();
		const auto file = root.saved();
		const auto& group = file["Test Params save"];
		EXPECT_EQ(group["nobody"].asInt(), 9) << "a key nothing registered was dropped";
		EXPECT_EQ(file["Test Params save elsewhere"]["x"].asInt(), 1) << "a group nothing registered was dropped";
		EXPECT_EQ(group["changed"].asFloat(), 2.f);
		EXPECT_FALSE(group.isMember("same")) << "a number back at the code's value was kept";
		EXPECT_FALSE(group.isMember("this run")) << "a number of this run only was saved";
		EXPECT_EQ(group["held"].asFloat(), 3.f) << "the session's value was saved over the number's own";
		EXPECT_TRUE(group["count"].isInt());
		EXPECT_EQ(group["count"].asInt(), 4);
		EXPECT_TRUE(group["on"].isBool());
		EXPECT_TRUE(group["on"].asBool());
		EXPECT_EQ(held, 7.f) << "saving ended the session";
		EXPECT_TRUE(seenOf("Test Params save", "held").session);
	}

	// A value from the command line holds through a reload; one from the bridge goes with it, back to the file's, and from
	// under it the command line's comes back.
	TEST(Params, ARunValueOutlastsAReloadAndABridgeValueDoesNot) {
		const TempRoot root(R"({"Test Params session": {"bridge": 4}})");
		float& run = Params::value("Test Params session", "run", 1.f);
		float& bridge = Params::value("Test Params session", "bridge", 1.f);
		float& both = Params::value("Test Params session", "both", 1.f);
		ASSERT_TRUE(Params::setSession("Test Params session", "run", 2.0, Params::Session::Run));
		ASSERT_TRUE(Params::setSession("Test Params session", "bridge", 3.0, Params::Session::UntilReload));
		ASSERT_TRUE(Params::setSession("Test Params session", "both", 5.0, Params::Session::Run));
		ASSERT_TRUE(Params::setSession("Test Params session", "both", 6.0, Params::Session::UntilReload));
		EXPECT_EQ(bridge, 3.f);
		EXPECT_EQ(both, 6.f);
		EXPECT_TRUE(seenOf("Test Params session", "bridge").session);
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(run, 2.f) << "the command line's value went with the reload";
		EXPECT_TRUE(seenOf("Test Params session", "run").session);
		EXPECT_EQ(bridge, 4.f) << "the bridge's value outlasted the reload";
		EXPECT_FALSE(seenOf("Test Params session", "bridge").session);
		EXPECT_EQ(both, 5.f) << "the command line's value did not come back from under the bridge's";
		EXPECT_TRUE(seenOf("Test Params session", "both").session);
	}

	// Anything else that writes a number held for the session -- a drag in a panel, the code itself -- ends the session,
	// and what it wrote is the number's own, the one saved and kept through a reload.
	TEST(Params, AWriteEndsTheSession) {
		const TempRoot root("{}");
		float& number = Params::value("Test Params written", "n", 1.f);
		ASSERT_TRUE(Params::setSession("Test Params written", "n", 4.0, Params::Session::UntilReload));
		number = 6.f;
		const auto seen = seenOf("Test Params written", "n");
		EXPECT_FALSE(seen.session);
		EXPECT_EQ(seen.value, 6.0);
		Params::save();
		EXPECT_EQ(root.saved()["Test Params written"]["n"].asFloat(), 6.f) << "the written value was not saved";
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(number, 6.f) << "a reload took the written value back as if it were the session's";
	}

	// A value set before anything registered its name waits for the registration, listed as waiting until then.
	TEST(Params, ASetBeforeRegistrationWaitsForIt) {
		ASSERT_TRUE(Params::setSession("Test Params waiting", "later", 8.0, Params::Session::Run));
		Params::Seen seen{};
		EXPECT_FALSE(Params::see("Test Params waiting", "later", seen)) << "seen before it was registered";
		EXPECT_NE(Params::waiting().find("Test Params waiting / later"), std::string::npos);
		EXPECT_EQ(Params::value("Test Params waiting", "later", 1), 8);
		EXPECT_TRUE(seenOf("Test Params waiting", "later").session);
		EXPECT_EQ(Params::waiting().find("Test Params waiting / later"), std::string::npos) << "still waiting once registered";
	}

	// Only a value the number's type cannot be is refused: one past what it holds, or no number at all. An int is rounded,
	// and a bool is true for anything but nothing.
	TEST(Params, ASessionValueIsOneTheTypeCanBe) {
		int& whole = Params::value("Test Params fit", "int", 1);
		float& real = Params::value("Test Params fit", "float", 1.f);
		bool& flag = Params::value("Test Params fit", "bool", false);
		EXPECT_FALSE(Params::setSession("Test Params fit", "int", 1e20, Params::Session::UntilReload));
		EXPECT_FALSE(Params::setSession("Test Params fit", "float", 1e300, Params::Session::UntilReload));
		EXPECT_FALSE(Params::setSession("Test Params fit", "float", std::nan(""), Params::Session::UntilReload));
		EXPECT_EQ(whole, 1);
		EXPECT_EQ(real, 1.f);
		EXPECT_TRUE(Params::setSession("Test Params fit", "int", 2.6, Params::Session::UntilReload));
		EXPECT_EQ(whole, 3);
		EXPECT_TRUE(Params::setSession("Test Params fit", "bool", 0.5, Params::Session::UntilReload));
		EXPECT_TRUE(flag);
		whole = 1;
		flag = false;
	}

	// Storage that goes away leaves its number with the registry, at the value it had; the next bind of the name gives it
	// the new storage, with that value written in.
	TEST(Params, AnUnboundNumberKeepsItsValueForTheNextBind) {
		static float first = 1.f;
		static float second = 1.f;
		first = 1.f;
		second = 1.f;
		Params::bind("Test Params unbind", "n", first);
		first = 4.f;
		Params::unbind(&first, &first + 1);
		first = 100.f;
		EXPECT_EQ(seenOf("Test Params unbind", "n").value, 4.0) << "the number followed storage that had gone";
		Params::bind("Test Params unbind", "n", second);
		EXPECT_EQ(second, 4.f);
		EXPECT_EQ(Params::findFloat("Test Params unbind", "n"), &second);
		Params::unbind(&second, &second + 1);
	}

	// A settings object's "reset defaults": every number bound inside it back to the code's value, and nothing outside it;
	// one held for the session keeps the session's, and what it has besides goes back to the code's.
	TEST(Params, ResetPutsARangeBackAsTheCodeHasIt) {
		const TempRoot root("{}");
		struct Settings {
			float a = 1.f;
			int b = 2;
			bool c = true;
		};
		static Settings settings;
		static float outside = 1.f;
		settings = {};
		outside = 1.f;
		Params::bind("Test Params reset", "a", settings.a);
		Params::bind("Test Params reset", "b", settings.b);
		Params::bind("Test Params reset", "c", settings.c);
		Params::bind("Test Params reset", "outside", outside);
		settings.a = 5.f;
		settings.b = 7;
		settings.c = false;
		outside = 3.f;
		ASSERT_TRUE(Params::setSession("Test Params reset", "b", 9.0, Params::Session::UntilReload));
		Params::reset(&settings, &settings + 1);
		EXPECT_EQ(settings.a, 1.f);
		EXPECT_TRUE(settings.c);
		EXPECT_EQ(settings.b, 9) << "the session's value was reset";
		EXPECT_EQ(outside, 3.f) << "a number outside the range was reset";
		Params::save();
		const auto group = root.saved()["Test Params reset"];
		EXPECT_FALSE(group.isMember("b")) << "what the number has besides the session was not reset";
		EXPECT_EQ(group["outside"].asFloat(), 3.f);
		Params::unbind(&settings, &settings + 1);
		Params::unbind(&outside, &outside + 1);
	}

	// findFloat finds a float, and nothing for another type or no number.
	TEST(Params, FindFloatFindsOnlyAFloat) {
		float& real = Params::value("Test Params find", "float", 1.f);
		Params::value("Test Params find", "int", 1);
		EXPECT_EQ(Params::findFloat("Test Params find", "float"), &real);
		EXPECT_EQ(Params::findFloat("Test Params find", "int"), nullptr);
		EXPECT_EQ(Params::findFloat("Test Params find", "none"), nullptr);
	}

	// The engine's own numbers, as its .cpp files register them: under the keys tweaks.json has had them by, bound to what
	// reads them and starting at the code's value; the two main.cpp's --no-taa and --debug-term set, of their type; and
	// what is this run's only, not the file's to set.
	TEST(Params, TheEngineRegistersItsNumbersUnderTheirKeys) {
		using ComponentsModule::ShelterComponent;
		using Render::RenderPasses::SunShadowPass;
		auto* render = ECSHandler::systemManager().getSystem<SystemsModule::RenderSystem>();
		auto* audio = ECSHandler::systemManager().getSystem<SystemsModule::AudioSystem>();
		ASSERT_TRUE(render);
		ASSERT_TRUE(audio);
		auto& gfx = render->getRenderData().graphics;
		// The files read below take every number to the code's value or the file's: each is put back as it was, whatever
		// root the run has.
		const auto wasGfx = gfx;
		const auto wasShadows = SunShadowPass::settings;
		const float wasEye = ShelterComponent::eyeAdapts;
		const auto wasAudio = audio->settings;
		const int wasTerm = Render::debugView();
		const Finally restore([&] {
			gfx = wasGfx;
			SunShadowPass::settings = wasShadows;
			ShelterComponent::eyeAdapts = wasEye;
			audio->settings = wasAudio;
			Render::debugView() = wasTerm;
		});
		gfx.timeOfDay = 7.f;
		gfx.windStrength = 0.25f;
		Render::debugView() = 3;
		{
			const TempRoot root("{}");
			ASSERT_TRUE(Params::load());
		}

		const SystemsModule::GraphicsSettings code{};
		const SystemsModule::AudioSystem::Settings audioCode{};
		struct Number {
			const char* group;
			const char* name;
			const float* at;
			float initial;
		};
		const Number numbers[] = {
			// The keys the game bound the graphics settings by, in tweaks.json as it is.
			{ "AO", "radius", &gfx.ssaoRadius, code.ssaoRadius },
			{ "AO", "thickness", &gfx.ssaoThickness, code.ssaoThickness },
			{ "AO", "intensity", &gfx.ssaoIntensity, code.ssaoIntensity },
			{ "AO", "sun share", &gfx.ssaoSun, code.ssaoSun },
			{ "AO", "grass floor", &gfx.ssaoGrassFloor, code.ssaoGrassFloor },
			{ "Shadows", "sun width", &gfx.shadowSunWidth, code.shadowSunWidth },
			{ "Shadows", "penumbra most", &gfx.shadowPenumbraMost, code.shadowPenumbraMost },
			{ "Shadows", "slope bias", &gfx.shadowSlopeBias, code.shadowSlopeBias },
			{ "Shadows", "slope bias most", &gfx.shadowSlopeBiasMost, code.shadowSlopeBiasMost },
			{ "Shadows", "leaf light", &gfx.shadowLeafLight, code.shadowLeafLight },
			{ "Shadows", "refresh rate", &gfx.shadowRefreshRate, code.shadowRefreshRate },
			{ "Light", "sky", &gfx.iblStrength, code.iblStrength },
			{ "Light", "shadow fill", &gfx.shadowAmbient, code.shadowAmbient },
			{ "Skin", "sheen", &gfx.sheenStrength, code.sheenStrength },
			{ "Skin", "rim", &gfx.rimStrength, code.rimStrength },
			{ "Skin", "scatter wrap", &gfx.sssWrap, code.sssWrap },
			{ "Skin", "scatter", &gfx.sssStrength, code.sssStrength },
			{ "Skin", "scatter radius", &gfx.sssRadius, code.sssRadius },
			{ "Render", "lod pixels", &gfx.lodPixels, code.lodPixels },
			{ "Terrain", "coarser past", &gfx.terrainLodDistance, code.terrainLodDistance },
			{ "Foliage", "depth prepass", &gfx.foliagePrepass, code.foliagePrepass },
			// And the rest the game bound, kept where they were.
			{ "Shadows", "samples", &SunShadowPass::settings.samples, 8.f },
			{ "Shadows", "blur tolerance", &SunShadowPass::settings.tolerance, 0.01f },
			{ "Shelter", "eye adapts", &ShelterComponent::eyeAdapts, 0.5f },
			{ "Audio", "hrtf", &audio->settings.hrtf, audioCode.hrtf },
			{ "Audio", "reverb wet", &audio->settings.reverb, audioCode.reverb },
			{ "Audio", "air absorption", &audio->settings.airAbsorption, audioCode.airAbsorption },
		};
		for (const auto& [group, name, at, initial] : numbers) {
			EXPECT_EQ(Params::findFloat(group, name), at) << group << " / " << name << " is not the number read";
			EXPECT_EQ(*at, initial) << group << " / " << name << " does not start at the code's value";
		}
		// Kept by the registry, read where a sound starts.
		ASSERT_TRUE(Params::findFloat("Audio", "reverb room size"));
		EXPECT_EQ(*Params::findFloat("Audio", "reverb room size"), 0.4f);
		ASSERT_TRUE(Params::findFloat("Audio", "reverb damping"));
		EXPECT_EQ(*Params::findFloat("Audio", "reverb damping"), 0.5f);

		// --no-taa's: a bool, on as the code has it.
		EXPECT_TRUE(code.taa);
		EXPECT_EQ(seenOf("Graphics", "taa").type, Params::Type::Bool);
		EXPECT_EQ(seenOf("Graphics", "taa").value, 1.0);
		// --debug-term's: an int, the view the lighting draws (Render::debugView).
		EXPECT_EQ(seenOf("Render", "debug view").type, Params::Type::Int);
		EXPECT_EQ(seenOf("Render", "debug view").value, 3.0) << "the debug view is not the engine's, or a reload reset it";
		EXPECT_EQ(Params::findFloat("Graphics", "time of day"), &gfx.timeOfDay);
		EXPECT_EQ(Params::findFloat("Graphics", "wind"), &gfx.windStrength);
		EXPECT_EQ(gfx.timeOfDay, 7.f) << "a reload reset a number of this run only";
		EXPECT_EQ(gfx.windStrength, 0.25f) << "a reload reset a number of this run only";

		// A file sets the number that is its to set, and none of those that are this run's only.
		{
			const TempRoot root(R"({"Graphics": {"taa": false, "time of day": 12, "wind": 0.9}, "Render": {"debug view": 5}})");
			ASSERT_TRUE(Params::load());
		}
		EXPECT_FALSE(gfx.taa) << "the file's taa was not read into the settings";
		EXPECT_EQ(gfx.timeOfDay, 7.f) << "the time of day was read from the file";
		EXPECT_EQ(gfx.windStrength, 0.25f) << "the wind was read from the file";
		EXPECT_EQ(Render::debugView(), 3) << "the debug view was read from the file";
	}

	// A settings object's "reset defaults" goes back to where each number stands: the file's value as last read where it
	// has one, the code's where it has none or for a number of this run only, and nothing outside the range; one held for
	// the session keeps the session's, and what it has besides goes back so -- here the code's, the file having none.
	TEST(Params, ResetPutsARangeBackAsTheFileHasIt) {
		const TempRoot root(R"({"Test Params reset file": {"a": 4, "c": false}})");
		struct Settings {
			float a = 1.f;
			int b = 2;
			bool c = true;
			float t = 6.f;
		};
		static Settings settings;
		static float outside = 1.f;
		settings = {};
		outside = 1.f;
		Params::bind("Test Params reset file", "a", settings.a);
		Params::bind("Test Params reset file", "b", settings.b);
		Params::bind("Test Params reset file", "c", settings.c);
		Params::bind("Test Params reset file", "t", settings.t, { .transient = true });
		Params::bind("Test Params reset file", "outside", outside);
		const Finally unbind([] {
			Params::unbind(&settings, &settings + 1);
			Params::unbind(&outside, &outside + 1);
		});
		ASSERT_TRUE(Params::load());
		ASSERT_EQ(settings.a, 4.f);
		ASSERT_FALSE(settings.c);
		settings.a = 5.f;
		settings.b = 7;
		settings.c = true;
		settings.t = 2.f;
		outside = 3.f;
		ASSERT_TRUE(Params::setSession("Test Params reset file", "b", 9.0, Params::Session::UntilReload));
		Params::reset(&settings, &settings + 1);
		EXPECT_EQ(settings.a, 4.f) << "a reset went to the code's value, not the file's";
		EXPECT_FALSE(settings.c) << "a reset went to the code's value, not the file's";
		EXPECT_EQ(settings.t, 6.f) << "a number of this run only did not go back to the code's";
		EXPECT_EQ(settings.b, 9) << "the session's value was reset";
		EXPECT_EQ(outside, 3.f) << "a number outside the range was reset";
		Params::save();
		const auto group = root.saved()["Test Params reset file"];
		EXPECT_FALSE(group.isMember("b")) << "what the number has besides the session did not go back to the code's";
		EXPECT_EQ(group["a"].asFloat(), 4.f);
		EXPECT_EQ(group["outside"].asFloat(), 3.f);
	}

	// What a save wrote is where the number stands from then on, as a load of the file would have it: a reset after it
	// goes back to the value saved, not to the one the file had before.
	TEST(Params, SaveIsWhereTheNumbersStand) {
		const TempRoot root(R"({"Test Params saved": {"n": 2}})");
		static float number = 0.f;
		number = 0.f;
		Params::bind("Test Params saved", "n", number);
		const Finally unbind([] { Params::unbind(&number, &number + 1); });
		ASSERT_TRUE(Params::load());
		ASSERT_EQ(number, 2.f);
		number = 3.f;
		Params::save();
		ASSERT_EQ(root.saved()["Test Params saved"]["n"].asFloat(), 3.f);
		EXPECT_TRUE(Params::atStanding("Test Params saved", "n")) << "the value just saved is not where the number stands";
		number = 5.f;
		EXPECT_FALSE(Params::atStanding("Test Params saved", "n"));
		Params::reset(&number, &number + 1);
		EXPECT_EQ(number, 3.f) << "a reset went back past what was saved";
	}

	// A number neutral in the code whose look is the file's (an application's grade): read, reset and saved, it stays the
	// file's. Only set back to the code's value and saved does it leave the file, and a load then gives the code's.
	TEST(Params, ANeutralCodeKeepsTheFilesLook) {
		const TempRoot root(R"({"Test Params look": {"grade": 1.0}})");
		static float grade = 0.f;
		grade = 0.f;
		Params::bind("Test Params look", "grade", grade);
		const Finally unbind([] { Params::unbind(&grade, &grade + 1); });
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(grade, 1.f);
		Params::reset(&grade, &grade + 1);
		EXPECT_EQ(grade, 1.f) << "a reset took the file's look away";
		Params::save();
		EXPECT_EQ(root.saved()["Test Params look"]["grade"].asFloat(), 1.f) << "a save took the file's look away";
		grade = 0.f;
		Params::save();
		EXPECT_FALSE(root.saved().isMember("Test Params look")) << "a value back at the code's was kept in the file";
		grade = 1.f;
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(grade, 0.f) << "the file did not give back what was saved";
	}

	// The numbers file is the one the application named, whatever else the root holds: a tweaks.json beside it is neither
	// read nor written.
	TEST(Params, TheFileIsTheApplications) {
		const TempRoot root(R"({"Test Params whose": {"n": 2}})");
		const std::string beside = R"({"Test Params whose": {"n": 7}})";
		std::ofstream(root.content() / "tweaks.json", std::ios::binary | std::ios::trunc) << beside;
		float& number = Params::value("Test Params whose", "n", 1.f);
		const Finally restore([&] { number = 1.f; });
		ASSERT_EQ(Params::file(), "numbers.json");
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(number, 2.f) << "the numbers were not read from the file named";
		number = 4.f;
		Params::save();
		EXPECT_EQ(root.saved()["Test Params whose"]["n"].asFloat(), 4.f) << "the numbers were not saved into the file named";
		EXPECT_EQ(bytesOf(root.content() / "tweaks.json"), beside) << "a file nobody named was written";
	}

	// No file named -- an application that names none -- and nothing is read or written, whatever the root holds: load
	// says so and leaves every number as it was, save leaves every file as it was and makes none.
	TEST(Params, NoFileNamedReadsAndWritesNothing) {
		const std::string numbers = R"({"Test Params unnamed": {"n": 2}})";
		const TempRoot root(numbers);
		float& number = Params::value("Test Params unnamed", "n", 1.f);
		const Finally restore([&] { number = 1.f; });
		Params::setFile("");
		EXPECT_EQ(Params::file(), "");
		number = 3.f;
		EXPECT_FALSE(Params::load());
		EXPECT_EQ(number, 3.f) << "a load with no file named changed a number";
		Params::save();
		EXPECT_EQ(bytesOf(root.file()), numbers) << "a file was written though none was named";
		size_t files = 0;
		for (const auto& entry : std::filesystem::directory_iterator(root.content())) {
			files += entry.is_regular_file() ? 1 : 0;
		}
		EXPECT_EQ(files, 1u) << "a save with no file named made one";
	}

	// Where a number stands is compared in its own type: a float the file has as 1.05 stands at the float 1.05f, never at
	// the double, so right after the load it is there and the Tweaks window offers no reset that would do nothing.
	TEST(Params, AFloatAsTheFileHasItIsAtItsStanding) {
		const TempRoot root(R"({"Test Params standing": {"f": 1.05, "i": 3}})");
		struct Numbers {
			float f = 0.f;
			int i = 0;
		};
		static Numbers numbers;
		numbers = {};
		Params::bind("Test Params standing", "f", numbers.f);
		Params::bind("Test Params standing", "i", numbers.i);
		const Finally unbind([] { Params::unbind(&numbers, &numbers + 1); });
		ASSERT_TRUE(Params::load());
		EXPECT_EQ(numbers.f, 1.05f);
		EXPECT_EQ(numbers.i, 3);
		EXPECT_TRUE(Params::atStanding("Test Params standing", "f")) << "the file's float was compared as a double";
		EXPECT_TRUE(Params::atStanding("Test Params standing", "i"));
		numbers.f = 2.f;
		EXPECT_FALSE(Params::atStanding("Test Params standing", "f"));
		Params::reset(&numbers, &numbers + 1);
		EXPECT_EQ(numbers.f, 1.05f);
		EXPECT_TRUE(Params::atStanding("Test Params standing", "f"));
		EXPECT_FALSE(Params::atStanding("Test Params standing", "nobody")) << "a name nobody registered stands somewhere";
	}
}
