#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#include "EngineTestFixture.h"
#include "core/Clocks.h"

namespace SFE::EngineTests {
	// Under a fixed step the simulation's clock is the sum of the frames' steps, not the wall clock.
	TEST(Clocks, SimulationGoesByTheFrameSteps) {
		step();
		const double before = Clocks::simulation();
		step(30);
		EXPECT_NEAR(Clocks::simulation() - before, 30.0 * kFrameTime, 1e-6);
	}

	// Held, the animation clock stands at the time asked for, whatever the frames do; released, it goes on from where it
	// would have been, as if it had run all along.
	TEST(Clocks, HeldAnimationClockStandsStillThenGoesOn) {
		step();
		const double before = Engine::instance()->animationClock();
		Clocks::hold(40.0);
		step();
		EXPECT_DOUBLE_EQ(Engine::instance()->animationClock(), 40.0);
		EXPECT_TRUE(Clocks::held());
		EXPECT_TRUE(Clocks::stillPicture()) << "a held frame is a still picture";
		step(9);
		EXPECT_DOUBLE_EQ(Engine::instance()->animationClock(), 40.0) << "the held clock moved";
		Clocks::release();
		step();
		EXPECT_FALSE(Clocks::held());
		EXPECT_NEAR(Engine::instance()->animationClock() - before, 11.0 * kFrameTime, 1e-5) << "it did not go on underneath";
	}

	// What moves by the animation clock's steps stands still while it is held, the jumps onto the hold and off it not
	// counted as time gone by.
	TEST(Clocks, StepIsNothingWhileHeld) {
		step();
		EXPECT_FLOAT_EQ(Clocks::step(), kFrameTime);
		Clocks::hold(1e6);
		step();
		EXPECT_EQ(Clocks::step(), 0.f) << "the jump onto the hold counted";
		step();
		EXPECT_EQ(Clocks::step(), 0.f);
		Clocks::release();
		step();
		EXPECT_FLOAT_EQ(Clocks::step(), kFrameTime) << "the jump off the hold counted";
	}

	// The pictures made over frames start again on the first frame of a hold, and only on it.
	TEST(Clocks, PicturesStartAgainOnTheFirstHeldFrame) {
		step();
		EXPECT_FALSE(Clocks::restarting());
		Clocks::hold(40.0);
		step();
		EXPECT_TRUE(Clocks::restarting()) << "not on the first held frame";
		step();
		EXPECT_FALSE(Clocks::restarting()) << "again on the next";
		Clocks::release();
		step();
		EXPECT_FALSE(Clocks::restarting()) << "on a release";
	}

	// Frames stepping the world, a budget of work never runs out: what a frame does is what is due, however long it takes.
	TEST(Clocks, BudgetNeverRunsOutWhenFramesStep) {
		step();
		ASSERT_TRUE(Clocks::stepped());
		const Clocks::Budget budget(0.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		EXPECT_TRUE(budget.left()) << "a stepped frame's budget ran out on the wall clock";
		EXPECT_EQ(budget.until(), std::chrono::steady_clock::time_point::max());
	}

	// A frame begun as one of a world running in real time (not run: that would move the frames' clock on by the wall's
	// for the tests after), taken back on every way out of the test by frames stepping it again.
	namespace {
		void beginRealTimeFrame() {
			Clocks::beginFrame(-1.0, 0.0);
		}
	}

	// In real time it is the wall clock's milliseconds, as before.
	TEST(Clocks, BudgetIsMillisecondsInRealTime) {
		const Finally stepAgain([] { step(); });
		beginRealTimeFrame();
		ASSERT_FALSE(Clocks::stepped());
		EXPECT_FALSE(Clocks::Budget(0.0).left()) << "no time is time left";
		const Clocks::Budget budget(5.0);
		EXPECT_TRUE(Clocks::Budget(60000.0).left());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		EXPECT_FALSE(budget.left()) << "5 ms lasted 10";
	}

	// Frames stepping the world, work in flight is waited for where it is looked for, so its result lands on that frame
	// however long it takes; in real time it is taken only once it is done.
	TEST(Clocks, WorkInFlightIsWaitedForWhenFramesStep) {
		step();
		ASSERT_TRUE(Clocks::stepped());
		auto slow = std::async(std::launch::async, [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return 7; });
		EXPECT_TRUE(Clocks::ready(slow)) << "not waited for";
		EXPECT_EQ(slow.get(), 7);

		const Finally stepAgain([] { step(); });
		beginRealTimeFrame();
		std::promise<void> done;
		const auto job = done.get_future().share();
		EXPECT_FALSE(Clocks::ready(job)) << "in real time, taken before it was done";
		done.set_value();
		EXPECT_TRUE(Clocks::ready(job));
	}

	// Work that waits on the main thread for something (what goes to the device, as a model's load does) is not waited
	// for in a deadlock: the main thread does that meanwhile.
	TEST(Clocks, WaitingForWorkServesTheMainThread) {
		step();
		ASSERT_TRUE(Clocks::stepped());
		ASSERT_TRUE(Engine::isMainThread());
		bool served = false;
		auto load = std::async(std::launch::async, [&served] {
			ThreadPool::instance()->addTask<WorkerType::SYNC>([&served] { served = true; }).wait();
		});
		EXPECT_TRUE(Clocks::ready(load));
		EXPECT_TRUE(served);
	}
}
