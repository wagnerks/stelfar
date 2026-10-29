#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <json/value.h>

#include "EngineTestFixture.h"
#include "core/InputHandler.h"
#include "core/KeyNames.h"
#include "core/Params.h"
#include "renderModule/DebugView.h"
#include "runtimeBridge/RuntimeBridge.h"

// The engine's own commands as a script sends them, through the bridge's runCommand: every answer carries the command's
// name in front ("tweak: ..."), and scripts parse what comes after it.
namespace SFE::EngineTests {
	namespace {
		std::string run(const std::string& command, const Json::Value& args = Json::Value()) {
			return RuntimeBridge::RuntimeBridge::instance()->runCommand(command, args);
		}

		Json::Value tweak(const std::string& group, const std::string& name) {
			Json::Value args;
			args["group"] = group;
			args["name"] = name;
			return args;
		}

		template <typename T>
		Json::Value tweak(const std::string& group, const std::string& name, T value) {
			auto args = tweak(group, name);
			args["value"] = value;
			return args;
		}

		// A root of its own for one test, outside every real one, holding these numbers if any in a file `name`, which it
		// names the application's numbers file (Params::setFile); the roots and the file named put back as they were when
		// the test ends.
		class TempRoot {
		public:
			explicit TempRoot(const std::string& numbers = {}, std::string name = "numbers.json") : mWasRoot(FileSystem::root()),
				mWasEngine(FileSystem::engineRoot()), mWasFile(Params::file()),
				mDir(std::filesystem::temp_directory_path()
					/ ("sfe_bridge_commands_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
				ThreadPool::instance()->waitIdle();
				std::filesystem::create_directories(mDir / "content");
				std::filesystem::create_directories(mDir / "engine");
				FileSystem::setRoot((mDir / "content").string());
				FileSystem::setEngineRoot((mDir / "engine").string());
				Params::setFile(name);
				if (!numbers.empty()) {
					std::ofstream(mDir / "content" / name, std::ios::binary | std::ios::trunc) << numbers;
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

		private:
			std::filesystem::path mWasRoot;
			std::filesystem::path mWasEngine;
			std::string mWasFile;
			std::filesystem::path mDir;
		};

		// What a listener of the input hears: the keys, the mouse's buttons and its wheel.
		class Listener : public CoreModule::InputObserver {
		public:
			Listener() {
				onKeyEvent = [this](CoreModule::InputKey key, CoreModule::InputEventType type) {
					heard.emplace_back(key, type);
				};
				onMouseBtnEvent = [this](Math::DVec2, CoreModule::MouseButton button, CoreModule::InputEventType type) {
					clicked.emplace_back(button, type);
				};
				onScrollEvent = [this](Math::DVec2 offset) {
					turned.emplace_back(offset.x, offset.y);
				};
			}
			std::vector<std::pair<CoreModule::InputKey, CoreModule::InputEventType>> heard;
			std::vector<std::pair<CoreModule::MouseButton, CoreModule::InputEventType>> clicked;
			std::vector<std::pair<double, double>> turned;
		};
	}

	// A float is read and set as it always was, to six decimals: scripts take the number after the last '='. A value set
	// so holds for the session.
	TEST(BridgeCommands, TweakReadsAndSetsAFloat) {
		float& number = Params::value("Test Bridge", "float", 0.25f);
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "float")), "tweak: Test Bridge / float = 0.250000");
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "float", 1.5)), "tweak: Test Bridge / float = 1.500000");
		EXPECT_EQ(number, 1.5f);
		Params::Seen seen{};
		ASSERT_TRUE(Params::see("Test Bridge", "float", seen));
		EXPECT_TRUE(seen.session) << "a value from the bridge is not held for the session";
		number = 0.25f;
	}

	// An int is answered whole, a value given it rounded; a bool as 1 or 0, set by true and false or by a number.
	TEST(BridgeCommands, TweakAnswersAnIntWholeAndABoolAsOneOrNought) {
		int& whole = Params::value("Test Bridge", "int", 3);
		bool& flag = Params::value("Test Bridge", "bool", true);
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "int")), "tweak: Test Bridge / int = 3");
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "int", 7.6)), "tweak: Test Bridge / int = 8");
		EXPECT_EQ(whole, 8);
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "bool")), "tweak: Test Bridge / bool = 1");
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "bool", false)), "tweak: Test Bridge / bool = 0");
		EXPECT_FALSE(flag);
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "bool", 1)), "tweak: Test Bridge / bool = 1");
		EXPECT_TRUE(flag);
		whole = 3;
	}

	// A name nothing registered is said so, with no '=' for a script to take a number from; a value that is no number is
	// refused the same way; no arguments at all (a panel's button) says what to give.
	TEST(BridgeCommands, TweakSaysWhatItCannotDo) {
		Params::value("Test Bridge", "kept", 2.f);
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "none")), "tweak: no tweak Test Bridge / none");
		EXPECT_EQ(run("tweak", tweak("Test Bridge", "kept", "high")), "tweak: not a value for Test Bridge / kept");
		EXPECT_EQ(run("tweak"), "tweak: give {group, name[, value]} or {reload}");
	}

	// A reload without a file says so and changes nothing, and so does one with no file named, whatever the root holds;
	// with one, it reads it and takes back what the bridge set. The file is named in the answer: the game's says
	// "tweaks.json" there.
	TEST(BridgeCommands, TweakReloadTakesBackWhatTheBridgeSet) {
		Json::Value reload;
		reload["reload"] = true;
		float& number = Params::value("Test Bridge", "reloaded", 1.f);
		ASSERT_EQ(run("tweak", tweak("Test Bridge", "reloaded", 2)), "tweak: Test Bridge / reloaded = 2.000000");
		{
			// Under a root of no file, whatever root the run has.
			const TempRoot root;
			EXPECT_EQ(run("tweak", reload), "tweak: no numbers.json to read");
		}
		EXPECT_EQ(number, 2.f) << "a reload with no file to read changed a number";
		{
			const TempRoot root(R"({"Test Bridge": {"reloaded": 3}})");
			Params::setFile("");
			EXPECT_EQ(run("tweak", reload), "tweak: no numbers file named");
		}
		EXPECT_EQ(number, 2.f) << "a reload with no file named changed a number";
		{
			const TempRoot root(R"({"Test Bridge": {"reloaded": 3}})");
			EXPECT_EQ(run("tweak", reload), "tweak: tweaks read again from numbers.json");
		}
		EXPECT_EQ(number, 3.f);
		Params::Seen seen{};
		ASSERT_TRUE(Params::see("Test Bridge", "reloaded", seen));
		EXPECT_FALSE(seen.session) << "the bridge's value outlasted the reload";
		number = 1.f;
	}

	// debugTerm sets the debug view the lighting draws, the registry's number Render / debug view, and answers as it did.
	TEST(BridgeCommands, DebugTermSetsTheDebugView) {
		Json::Value args;
		args["term"] = 4;
		EXPECT_EQ(run("debugTerm", args), "debugTerm: debug term 4");
		EXPECT_EQ(Render::debugView(), 4);
		Params::Seen seen{};
		ASSERT_TRUE(Params::see("Render", "debug view", seen));
		EXPECT_EQ(seen.type, Params::Type::Int);
		EXPECT_EQ(seen.value, 4.0);
		EXPECT_EQ(run("debugTerm"), "debugTerm: debug term 4");
		args["term"] = 0;
		EXPECT_EQ(run("debugTerm", args), "debugTerm: debug term 0");
		EXPECT_EQ(Render::debugView(), 0);
	}

	// The names press knows: today's, every letter and digit, and the keys a script may want besides; a mouse button or a
	// name in small letters is no key.
	TEST(BridgeCommands, PressKnowsTheKeysByName) {
		using CoreModule::InputKey;
		using CoreModule::keyNamed;
		const std::pair<const char*, InputKey> named[] = {
			{ "W", InputKey::KEY_W }, { "A", InputKey::KEY_A }, { "S", InputKey::KEY_S }, { "D", InputKey::KEY_D },
			{ "SHIFT", InputKey::KEY_LEFT_SHIFT }, { "SPACE", InputKey::KEY_SPACE }, { "CTRL", InputKey::KEY_LEFT_CONTROL },
			{ "C", InputKey::KEY_C }, { "UP", InputKey::KEY_UP }, { "DOWN", InputKey::KEY_DOWN }, { "LEFT", InputKey::KEY_LEFT },
			{ "RIGHT", InputKey::KEY_RIGHT }, { "PAGEUP", InputKey::KEY_PAGE_UP }, { "PAGEDOWN", InputKey::KEY_PAGE_DOWN },
			{ "R", InputKey::KEY_R }, { "E", InputKey::KEY_E }, { "G", InputKey::KEY_G }, { "X", InputKey::KEY_X },
			{ "1", InputKey::KEY_1 }, { "2", InputKey::KEY_2 }, { "3", InputKey::KEY_3 }, { "4", InputKey::KEY_4 },
			{ "Z", InputKey::KEY_Z }, { "0", InputKey::KEY_0 }, { "9", InputKey::KEY_9 }, { "ALT", InputKey::KEY_LEFT_ALT },
			{ "TAB", InputKey::KEY_TAB }, { "ENTER", InputKey::KEY_ENTER }, { "ESCAPE", InputKey::KEY_ESCAPE },
			{ "BACKSPACE", InputKey::KEY_BACKSPACE }, { "HOME", InputKey::KEY_HOME }, { "END", InputKey::KEY_END },
			{ "F1", InputKey::KEY_F1 }, { "F10", InputKey::KEY_F10 }, { "F12", InputKey::KEY_F12 },
		};
		for (const auto& [name, key] : named) {
			EXPECT_EQ(keyNamed(name), key) << name;
		}
		for (const char* none : { "", "w", "LMB", "RMB", "F0", "F13", "F01", "F1X", "SHIFTY" }) {
			EXPECT_EQ(keyNamed(none), InputKey::UNKNOWN) << none;
		}
		// The mouse's buttons by their own names, which are no key's.
		using CoreModule::buttonNamed;
		using CoreModule::MouseButton;
		const std::pair<const char*, MouseButton> buttons[] = {
			{ "LMB", MouseButton::MOUSE_BUTTON_LEFT }, { "RMB", MouseButton::MOUSE_BUTTON_RIGHT },
			{ "MMB", MouseButton::MOUSE_BUTTON_MIDDLE },
		};
		for (const auto& [name, button] : buttons) {
			EXPECT_TRUE(buttonNamed(name) == button) << name;
		}
		for (const char* none : { "", "lmb", "W", "LMBX", "MB" }) {
			EXPECT_FALSE(buttonNamed(none).has_value()) << none;
		}
	}

	// A key, a mouse button or the wheel sent by the command reaches every listener of the input, as the window's does:
	// held and let go, turned either way. A name of no key or button says which there are, and nothing is heard.
	TEST(BridgeCommands, PressReachesEveryListener) {
		using CoreModule::InputEventType;
		using CoreModule::InputKey;
		using CoreModule::MouseButton;
		Listener first;
		Listener second;
		Json::Value args;
		args["key"] = "W";
		EXPECT_EQ(run("press", args), "press: W held");
		args["down"] = false;
		EXPECT_EQ(run("press", args), "press: W let go");
		for (const std::string button : { "LMB", "RMB", "MMB" }) {
			args["key"] = button;
			args["down"] = true;
			EXPECT_EQ(run("press", args), "press: " + button + " held");
			args["down"] = false;
			EXPECT_EQ(run("press", args), "press: " + button + " let go");
		}
		// Turned back as far, so the free camera the fixture makes, which zooms by the wheel, is left as it was.
		Json::Value wheel;
		wheel["wheel"] = 2;
		EXPECT_EQ(run("press", wheel), "press: wheel turned 2");
		wheel["wheel"] = -2;
		EXPECT_EQ(run("press", wheel), "press: wheel turned -2");

		const std::vector<std::pair<InputKey, InputEventType>> keys = {
			{ InputKey::KEY_W, InputEventType::PRESS }, { InputKey::KEY_W, InputEventType::RELEASE },
		};
		const std::vector<std::pair<MouseButton, InputEventType>> buttons = {
			{ MouseButton::MOUSE_BUTTON_LEFT, InputEventType::PRESS }, { MouseButton::MOUSE_BUTTON_LEFT, InputEventType::RELEASE },
			{ MouseButton::MOUSE_BUTTON_RIGHT, InputEventType::PRESS }, { MouseButton::MOUSE_BUTTON_RIGHT, InputEventType::RELEASE },
			{ MouseButton::MOUSE_BUTTON_MIDDLE, InputEventType::PRESS }, { MouseButton::MOUSE_BUTTON_MIDDLE, InputEventType::RELEASE },
		};
		const std::vector<std::pair<double, double>> turned = { { 0.0, 2.0 }, { 0.0, -2.0 } };
		for (const auto* listener : { &first, &second }) {
			EXPECT_EQ(listener->heard, keys);
			EXPECT_EQ(listener->clicked, buttons);
			EXPECT_EQ(listener->turned, turned);
		}

		args["key"] = "X1";
		args["down"] = true;
		EXPECT_EQ(run("press", args).rfind("press: give {\"key\": A-Z, 0-9", 0), 0u);
		wheel["wheel"] = "far";
		EXPECT_EQ(run("press", wheel).rfind("press: give {\"key\": A-Z, 0-9", 0), 0u);
		for (const auto* listener : { &first, &second }) {
			EXPECT_EQ(listener->heard.size(), 2u) << "a name of no key was pressed";
			EXPECT_EQ(listener->clicked.size(), 6u) << "a name of no button was pressed";
			EXPECT_EQ(listener->turned.size(), 2u) << "a wheel of no notches was turned";
		}
	}

	// A command added under a name already taken stands in for the earlier one, and addCommand says it replaced one.
	TEST(BridgeCommands, ACommandAddedAgainReplacesTheEarlier) {
		auto* bridge = RuntimeBridge::RuntimeBridge::instance();
		// A name of its own each time the test runs: no command is ever taken out.
		static int runs = 0;
		const auto name = "test bridge twice " + std::to_string(++runs);
		EXPECT_FALSE(bridge->addCommand(name, "the first", [](const Json::Value&) { return std::string("first"); }));
		EXPECT_TRUE(bridge->addCommand(name, "the second", [](const Json::Value&) { return std::string("second"); }));
		EXPECT_EQ(run(name), name + ": second");
		EXPECT_EQ(bridge->commands().at(name).help, "the second");
	}
}
