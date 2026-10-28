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

	// Each says it ran, in the order the frame ran them.
	std::vector<std::string> ran;
	struct RootA : ecss::System {
		void update(float) override { ran.push_back("A"); }
	};
	struct RootB : ecss::System {
		void update(float) override { ran.push_back("B"); }
	};
	struct RootX : ecss::System {
		void update(float) override { ran.push_back("X"); }
	};
	struct RootY : ecss::System {
		void update(float) override { ran.push_back("Y"); }
	};

	// How many were made.
	int neverMade = 0;
	struct Never : ecss::System {
		Never() { ++neverMade; }
		void update(float) override {}
	};

	struct Updating : ecss::System {
		void update(float) override {}
	};
	struct NoUpdate : ecss::System {};
	struct Rooted : ecss::System {
		void update(float) override {}
	};
	struct Ticked : ecss::System {
		void update(float) override {}
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
	systems.createSystem<Ticker>();
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
	systems.createSystem<LateTicker>();
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

// An application's own go before the engine's: right before the anchor, in the order asked, after what was there.
TEST(SystemManager, RootsGoBeforeTheirAnchorInTheirOrder) {
	ran.clear();
	ecss::SystemManager systems;
	systems.createSystem<RootA>();
	systems.createSystem<RootB>();
	systems.createSystem<RootX>();
	systems.createSystem<RootY>();
	systems.addRootSystems<RootA, RootB>();
	systems.addRootSystemsBefore<RootB, RootX, RootY>();
	systems.update(kStep);
	EXPECT_EQ(ran, (std::vector<std::string>{ "A", "X", "Y", "B" }));
}

// Before an anchor that is not on the main thread -- here never made -- nothing is scheduled: what was to go there is
// never updated, and is said as made but not scheduled.
TEST(SystemManager, NoAnchorSchedulesNothing) {
	ran.clear();
	ecss::SystemManager systems;
	auto* x = systems.createSystem<RootX>();
	systems.addRootSystemsBefore<RootB, RootX>();
	systems.update(kStep);
	EXPECT_TRUE(ran.empty()) << "a system went before an anchor that is not there";
	EXPECT_EQ(systems.unscheduled(), (std::vector<ecss::System*>{ x }));
}

// Scheduling takes what createSystem made and makes nothing itself: one never made is left out, and stays unmade.
TEST(SystemManager, SchedulingNeverMakesASystem) {
	neverMade = 0;
	ecss::SystemManager systems;
	systems.addRootSystems<Never>();
	systems.addTickSystems<Never>(60);
	systems.update(kStep);
	EXPECT_EQ(neverMade, 0) << "scheduling made a system";
	EXPECT_EQ(systems.getSystem<Never>(), nullptr);
}

// Scheduled again, on the main thread or as a tick system, it is still updated once a frame.
TEST(SystemManager, ASystemIsScheduledOnce) {
	ran.clear();
	ecss::SystemManager systems;
	systems.createSystem<RootA>();
	systems.addRootSystems<RootA>();
	systems.addRootSystems<RootA>();
	systems.addTickSystems<RootA>(60);
	systems.update(kStep);
	EXPECT_EQ(ran, (std::vector<std::string>{ "A" }));
}

// A system with an update of its own that is neither on the main thread nor ticked would never run: it is the one named.
// One without an update (a task worker) is not.
TEST(SystemManager, SaysWhatWasMadeButNotScheduled) {
	ecss::SystemManager systems;
	auto* updating = systems.createSystem<Updating>();
	systems.createSystem<NoUpdate>();
	systems.createSystem<Rooted>();
	systems.createSystem<Ticked>();
	systems.addRootSystems<Rooted>();
	systems.setStepTickSystems(true);
	systems.addTickSystems<Ticked>(60);
	EXPECT_EQ(systems.unscheduled(), (std::vector<ecss::System*>{ updating }));
}
