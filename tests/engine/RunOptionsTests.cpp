#include <chrono>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "EngineTestFixture.h"
#include "core/Pacing.h"
#include "core/Params.h"
#include "renderModule/DebugView.h"
#include "runtime/RunOptions.h"

// The engine's command line (RunOptions): the flags it takes and how, read with nothing global changed; what it leaves
// for the application, in order; and what apply() sets, on test-only names and roots put back after.
namespace SFE::EngineTests {
	namespace {
		using Render::GraphicsBackend;

		// A command line as main gets it: the program's name, then the arguments. Kept alive while the options parsed from
		// it point into it (RunOptions::rest).
		class CommandLine {
		public:
			CommandLine(std::initializer_list<std::string> arguments) : mTexts{ "game.exe" } {
				mTexts.insert(mTexts.end(), arguments);
				for (auto& text : mTexts) {
					mPointers.push_back(text.data());
				}
			}

			RunOptions parse(const GraphicsBackend backend = GraphicsBackend::Vulkan) {
				return RunOptions::parse(static_cast<int>(mPointers.size()), mPointers.data(), backend);
			}

		private:
			std::vector<std::string> mTexts;
			std::vector<char*> mPointers;
		};

		std::vector<std::string> restOf(const RunOptions& options) {
			std::vector<std::string> rest;
			for (size_t i = 1; i < options.rest.size(); ++i) {
				rest.emplace_back(options.rest[i] ? options.rest[i] : "(null)");
			}
			return rest;
		}

		// An environment of these variables only.
		RunOptions::Env envOf(std::map<std::string, std::string> variables) {
			return [variables = std::move(variables)](const char* name) -> const char* {
				const auto found = variables.find(name);
				return found == variables.end() ? nullptr : found->second.c_str();
			};
		}

		// FileSystem's roots as they were before the test, put back when it ends.
		class KeptRoots {
		public:
			KeptRoots() : mWasRoot(FileSystem::root()), mWasEngine(FileSystem::engineRoot()) {
				ThreadPool::instance()->waitIdle();
			}
			~KeptRoots() {
				ThreadPool::instance()->waitIdle();
				FileSystem::setRoot(mWasRoot.string());
				FileSystem::setEngineRoot(mWasEngine.string());
			}
			KeptRoots(const KeptRoots&) = delete;
			KeptRoots& operator=(const KeptRoots&) = delete;

		private:
			std::filesystem::path mWasRoot;
			std::filesystem::path mWasEngine;
		};
	}

	TEST(RunOptions, EngineFlagsFillTheConfig) {
		CommandLine line{ "--fixed-dt", "0.016", "--capture", "shot.png", "--capture-frame", "0", "--capture-passes", "passes" };
		const auto options = line.parse();
		const auto config = options.engineConfig();
		EXPECT_FLOAT_EQ(config.fixedDeltaTime, 0.016f);
		EXPECT_EQ(config.maxFps, EngineConfig{}.maxFps);
		EXPECT_TRUE(config.capture.enabled);
		EXPECT_EQ(config.capture.frame, 1) << "a capture lands on frame 1 at the earliest";
		EXPECT_EQ(config.capture.passDumpDir, FileSystem::resolve("passes").string()) << "a relative pass folder under the roots";
		EXPECT_TRUE(config.numbersFile.empty()) << "the numbers file is the application's";

		const auto absolute = (std::filesystem::temp_directory_path() / "sfe_passes").string();
		CommandLine kept{ "--capture", "shot.png", "--capture-passes", absolute };
		EXPECT_EQ(kept.parse().engineConfig().capture.passDumpDir, absolute) << "an absolute pass folder kept as given";

		CommandLine noCapture{ "--capture-passes", "passes", "--capture-frame", "0" };
		const auto plain = noCapture.parse().engineConfig();
		EXPECT_FALSE(plain.capture.enabled);
		EXPECT_TRUE(plain.capture.passDumpDir.empty()) << "passes dumped with no capture";
		EXPECT_EQ(plain.capture.frame, 0) << "the frame is held to 1 only for a capture";
	}

	TEST(RunOptions, EqualsFormsAsBefore) {
		CommandLine line{ "--capture=a.png", "--capture-frame=7", "--capture-passes=p", "--debug-term=4", "--content-dir=c" };
		const auto options = line.parse();
		EXPECT_EQ(options.capture.path, "a.png");
		EXPECT_EQ(options.capture.frame, 7);
		EXPECT_EQ(options.passDumpDir, "p");
		ASSERT_EQ(options.numbers.size(), 1u);
		EXPECT_EQ(options.numbers[0].value, 4.0);
		EXPECT_EQ(options.contentDir, "c");
		EXPECT_TRUE(restOf(options).empty());
	}

	TEST(RunOptions, GameFlagsAreLeftInOrder) {
		CommandLine line{ "--at", "1,2,3", "--backend", "vulkan", "--aim", "100", "30,5", "--fresh", "--backend=gl", "--frames", "9",
			"--look", "180,-5" };
		const auto options = line.parse();
		EXPECT_EQ(restOf(options), (std::vector<std::string>{ "--at", "1,2,3", "--aim", "100", "30,5", "--fresh", "--look", "180,-5" }));
		EXPECT_STREQ(options.rest[0], "game.exe") << "the program's name first, as in argv";
		EXPECT_EQ(options.frameLimit, 9);
	}

	TEST(RunOptions, AMissingValueIsIgnored) {
		CommandLine line{ "--fixed-dt", "0.5", "--frames" };
		const auto options = line.parse();
		EXPECT_FLOAT_EQ(options.fixedDeltaTime, 0.5f);
		EXPECT_EQ(options.frameLimit, 0);

		CommandLine capture{ "--capture" };
		EXPECT_FALSE(capture.parse().capture.enabled());
	}

	TEST(RunOptions, HostTakesAPortOnlyWhenADigitFollows) {
		CommandLine withPort{ "--host", "7000" };
		const auto hosted = withPort.parse();
		EXPECT_EQ(hosted.network.role, Network::Role::Host);
		EXPECT_EQ(hosted.network.port, 7000);

		CommandLine without{ "--host", "--fresh" };
		const auto plain = without.parse();
		EXPECT_EQ(plain.network.role, Network::Role::Host);
		EXPECT_EQ(plain.network.port, Network::Settings{}.port);
		EXPECT_EQ(restOf(plain), std::vector<std::string>{ "--fresh" });
	}

	TEST(RunOptions, ConnectSplitsAddress) {
		CommandLine withPort{ "--connect", "10.0.0.2:7001" };
		const auto joined = withPort.parse();
		EXPECT_EQ(joined.network.role, Network::Role::Client);
		EXPECT_EQ(joined.network.address, "10.0.0.2");
		EXPECT_EQ(joined.network.port, 7001);

		CommandLine without{ "--connect", "localhost" };
		const auto plain = without.parse();
		EXPECT_EQ(plain.network.address, "localhost");
		EXPECT_EQ(plain.network.port, Network::Settings{}.port);
	}

	TEST(RunOptions, NetworkConditionsAndPlayers) {
		CommandLine line{ "--net-lag", "120", "--net-jitter", "15", "--net-loss", "2.5", "--players", "0" };
		const auto options = line.parse();
		EXPECT_FLOAT_EQ(options.network.conditions.lagMs, 120.f);
		EXPECT_FLOAT_EQ(options.network.conditions.jitterMs, 15.f);
		EXPECT_FLOAT_EQ(options.network.conditions.lossPercent, 2.5f);
		EXPECT_EQ(options.network.maxPlayers, 1u) << "one player at the least";
		EXPECT_EQ(options.network.role, Network::Role::Offline);
	}

	TEST(RunOptions, VisibilityGoesByOrder) {
		CommandLine none{};
		const auto plain = none.parse();
		EXPECT_TRUE(plain.visible);
		EXPECT_FALSE(plain.serve);
		EXPECT_EQ(plain.servePort, EditorProtocol::kDefaultPort);

		CommandLine shown{ "--serve", "--visible", "--serve-port", "7760" };
		const auto served = shown.parse();
		EXPECT_TRUE(served.serve);
		EXPECT_TRUE(served.visible);
		EXPECT_TRUE(served.explicitlyVisible);
		EXPECT_EQ(served.servePort, 7760);

		CommandLine hidden{ "--visible", "--serve" };
		EXPECT_FALSE(hidden.parse().visible) << "--serve after --visible hides";

		CommandLine headless{ "--headless" };
		EXPECT_FALSE(headless.parse().visible);

		CommandLine capture{ "--capture", "x.png" };
		EXPECT_FALSE(capture.parse().visible) << "a capture hides";
		CommandLine shownCapture{ "--capture", "x.png", "--visible" };
		EXPECT_TRUE(shownCapture.parse().visible) << "a capture asked to be seen";
	}

	TEST(RunOptions, SessionNumbersInFlagOrder) {
		CommandLine line{ "--debug-term", "3", "--no-taa", "--debug-term=5" };
		const auto options = line.parse();
		ASSERT_EQ(options.numbers.size(), 3u);
		EXPECT_EQ(options.numbers[0].group, Render::kDebugViewGroup);
		EXPECT_EQ(options.numbers[0].name, Render::kDebugViewName);
		EXPECT_EQ(options.numbers[0].value, 3.0);
		EXPECT_EQ(options.numbers[1].group, "Graphics");
		EXPECT_EQ(options.numbers[1].name, "taa");
		EXPECT_EQ(options.numbers[1].value, 0.0);
		EXPECT_EQ(options.numbers[2].value, 5.0);
	}

	TEST(RunOptions, NothingDrawnRunsAt60) {
		CommandLine line{};
		EXPECT_EQ(line.parse(GraphicsBackend::None).maxFps, RunOptions::kNothingDrawnFps);
		EXPECT_EQ(line.parse(GraphicsBackend::Vulkan).maxFps, EngineConfig{}.maxFps);
		EXPECT_EQ(line.parse(GraphicsBackend::OpenGL).maxFps, EngineConfig{}.maxFps);
		EXPECT_EQ(pacedFps(line.parse(GraphicsBackend::None).engineConfig()), 60) << "a bot's frames in real time";

		CommandLine stepped{ "--fixed-dt", "0.016" };
		EXPECT_EQ(pacedFps(stepped.parse(GraphicsBackend::None).engineConfig()), 0) << "stepped frames are not paced";
	}

	TEST(RunOptions, ContentRootFlagBeatsEnv) {
		const auto env = envOf({ { "STELFORGE_CONTENT", "from_env" } });
		CommandLine given{ "--content-dir", "from_flag" };
		EXPECT_EQ(given.parse().contentRoot(env), std::filesystem::path("from_flag"));
		CommandLine none{};
		EXPECT_EQ(none.parse().contentRoot(env), std::filesystem::path("from_env"));
	}

	// The roots set, the engine's from what the application makes of the content root unless the environment names it,
	// and the numbers held for the run on names of this test's own (the registry is the one the run shares).
	TEST(RunOptions, ApplySetsTheRootsAndHoldsTheNumbers) {
		const KeptRoots kept;
		const auto dir = std::filesystem::temp_directory_path()
			/ ("sfe_run_options_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		RunOptions options;
		options.contentDir = (dir / "game" / "content").string();
		options.numbers = { { "Test RunOptions", "held", 7.0 }, { "Test RunOptions", "held", 9.0 } };
		options.apply([](const std::filesystem::path& content) { return (content / ".." / "engine" / "bin").lexically_normal(); },
			envOf({}));
		EXPECT_EQ(FileSystem::root(), (dir / "game" / "content").lexically_normal());
		EXPECT_EQ(FileSystem::engineRoot(), (dir / "game" / "engine" / "bin").lexically_normal());
		EXPECT_NE(Params::waiting().find("Test RunOptions / held"), std::string::npos) << "held before anything registered it";
		EXPECT_EQ(Params::value("Test RunOptions", "held", 1), 9) << "the last one given";
		EXPECT_EQ(Params::waiting().find("Test RunOptions / held"), std::string::npos);

		RunOptions named;
		named.contentDir = (dir / "other").string();
		named.apply([](const std::filesystem::path&) { return std::filesystem::path("unused"); },
			envOf({ { "STELFORGE_ENGINE_CONTENT", (dir / "engine_env").string() } }));
		EXPECT_EQ(FileSystem::engineRoot(), (dir / "engine_env").lexically_normal()) << "the environment's engine root first";
	}

	TEST(RunOptions, CaptureSchedule) {
		CaptureSchedule one;
		EXPECT_FALSE(one.enabled());
		EXPECT_FALSE(one.due(240, 0)) << "no capture without a path";
		one.path = "shot.png";
		EXPECT_FALSE(one.due(239, 0));
		ASSERT_TRUE(one.due(240, 0));
		EXPECT_EQ(*one.due(240, 0), "shot.png");
		EXPECT_TRUE(one.last(1));

		CaptureSchedule every{ "shot_%d.png", 10, 5, 3 };
		EXPECT_EQ(every.due(10, 0).value_or(""), "shot_0.png");
		EXPECT_FALSE(every.last(1));
		EXPECT_FALSE(every.due(14, 1));
		EXPECT_EQ(every.due(15, 1).value_or(""), "shot_1.png");
		EXPECT_FALSE(every.last(2));
		EXPECT_EQ(every.due(20, 2).value_or(""), "shot_2.png");
		EXPECT_TRUE(every.last(3));

		CaptureSchedule counted{ "shot.png", 10, 0, 3 };
		EXPECT_TRUE(counted.last(1)) << "a count with no interval is one capture";
	}
}
