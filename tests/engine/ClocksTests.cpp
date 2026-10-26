#include <cmath>

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
}
