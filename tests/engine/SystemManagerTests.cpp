#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "systemsModule/SystemManager.h"

// Systems go the other way they came: one made on another (a character's arm in the loose world) is deleted first, while
// what it was made on still stands -- and one made inside another's constructor outlives it.
namespace {
	std::vector<std::string> gone;

	struct World : ecss::System {
		~World() override { gone.push_back("world"); }
	};
	struct Inner : ecss::System {
		~Inner() override { gone.push_back("inner"); }
	};
	struct Character : ecss::System {
		explicit Character(ecss::SystemManager& systems) {
			world = systems.getSystem<World>();
			systems.createSystem<Inner>();
		}
		~Character() override {
			// What it put in the world is taken out of a world that is still there.
			gone.push_back(world && gone.empty() ? "character, the world there" : "character, the world gone");
		}
		World* world = nullptr;
	};

	// How many ticks it was given, and whether one came on a thread other than the one it was made on.
	struct Ticker : ecss::System {
		void update(float) override {
			ticks.fetch_add(1);
			if (std::this_thread::get_id() != made) {
				offThread.store(true);
			}
		}
		const std::thread::id made = std::this_thread::get_id();
		std::atomic<int> ticks{ 0 };
		std::atomic<bool> offThread{ false };
	};
	struct LateTicker : Ticker {};

	std::vector<std::string> debugged;
	struct MadeFirst : ecss::System {
		void debugUpdate(float) override { debugged.push_back("made first"); }
	};
	struct MadeSecond : ecss::System {
		void debugUpdate(float) override { debugged.push_back("made second"); }
	};

	constexpr float kStep = 1.f / 60.f;
}

TEST(SystemManager, DeletesASystemBeforeWhatItWasMadeOn) {
	gone.clear();
	{
		ecss::SystemManager systems;
		systems.createSystem<World>();
		systems.createSystem<Character>(systems);
	}
	EXPECT_EQ(gone, (std::vector<std::string>{ "character, the world there", "inner", "world" }));
}

// A tick system added before the frames are said to step the tick systems -- a game's is, made before the engine sets
// the mode -- is stepped by the frames as the rest are, on the main thread: one tick a frame at 60 of both a second.
// It used to go to a wall-clock thread of its own.
TEST(SystemManager, TickSystemAddedBeforeStepModeIsStepped) {
	ecss::SystemManager systems;
	systems.addTickSystems<Ticker>(60);
	systems.setStepTickSystems(true);
	const auto* ticker = systems.getSystem<Ticker>();
	ASSERT_NE(ticker, nullptr);
	for (int frame = 0; frame < 3; ++frame) {
		systems.update(kStep);
	}
	EXPECT_EQ(ticker->ticks.load(), 3) << "not stepped by the frames";
	EXPECT_FALSE(ticker->offThread.load()) << "ticked on a thread of its own";
}

// Once the first update has settled how tick systems run, one added after runs the same way.
TEST(SystemManager, TickSystemAddedAfterTheFirstUpdateKeepsTheMode) {
	ecss::SystemManager systems;
	systems.setStepTickSystems(true);
	systems.update(kStep);
	systems.addTickSystems<LateTicker>(60);
	systems.update(kStep);
	systems.update(kStep);
	const auto* late = systems.getSystem<LateTicker>();
	ASSERT_NE(late, nullptr);
	EXPECT_EQ(late->ticks.load(), 2);
	EXPECT_FALSE(late->offThread.load()) << "ticked on a thread of its own";
}

// Debug updates run in the order the systems were made, though the second was asked for -- and so given its type id,
// its slot in the manager -- before the first was made.
TEST(SystemManager, DebugUpdatesRunInCreationOrder) {
	debugged.clear();
	ecss::SystemManager systems;
	EXPECT_EQ(systems.getSystem<MadeSecond>(), nullptr);
	systems.createSystem<MadeFirst>();
	systems.createSystem<MadeSecond>();
	systems.update(kStep);
	EXPECT_EQ(debugged, (std::vector<std::string>{ "made first", "made second" }));
}
