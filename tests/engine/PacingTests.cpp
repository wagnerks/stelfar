#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "core/EngineConfig.h"
#include "core/Pacing.h"

// The frame limit (core/Pacing.h): frames end on a grid of 1 / fps, a late one is not made up, and only a run whose
// step is the wall clock's is held to it, and naps finely.
namespace SFE::FramePacingTests {
	using Clock = FrameSchedule::Clock;
	using std::chrono::milliseconds;

	TEST(FramePacing, HoldsMaxFps) {
		// On a clock of its own: 100 frames of 3 ms of work each at 100 a second end 10 ms apart exactly.
		const auto start = Clock::time_point{} + std::chrono::hours(1);
		FrameSchedule schedule(100, start);
		auto now = start;
		std::vector<Clock::time_point> ends;
		for (int i = 0; i < 100; ++i) {
			now += milliseconds(3);
			now = schedule.waitUntil(now);
			ends.push_back(now);
		}
		EXPECT_EQ(ends.front(), start + milliseconds(10));
		for (size_t i = 1; i < ends.size(); ++i) {
			ASSERT_EQ(ends[i] - ends[i - 1], milliseconds(10)) << "frame " << i;
		}

		// On the real clock: the waits can only overrun, so 20 frames at 100 a second take 190 ms at the least.
		FramePacer pacer;
		const auto began = Clock::now();
		pacer.start(100);
		ASSERT_TRUE(pacer.paced());
		for (int i = 0; i < 20; ++i) {
			pacer.endFrame();
		}
		const auto took = Clock::now() - began;
		EXPECT_GE(took, milliseconds(190));
		EXPECT_LT(took, std::chrono::seconds(10));
	}

	TEST(FramePacing, ASlowFrameIsNotMadeUp) {
		const auto start = Clock::time_point{} + std::chrono::hours(1);
		FrameSchedule schedule(100, start);
		const auto slow = start + milliseconds(50);
		EXPECT_EQ(schedule.waitUntil(slow), slow) << "a late frame waits for nothing";
		auto now = slow;
		for (int i = 1; i <= 5; ++i) {
			now += milliseconds(3);
			const auto until = schedule.waitUntil(now);
			EXPECT_GT(until, now) << "frame " << i << " after the slow one was not waited for";
			EXPECT_EQ(until, slow + i * milliseconds(10)) << "the grid starts again from the slow frame";
			now = until;
		}
	}

	TEST(FramePacing, NoLimitNeverWaits) {
		const auto start = Clock::time_point{} + std::chrono::hours(1);
		FrameSchedule none(0, start);
		EXPECT_FALSE(none.limited());
		EXPECT_EQ(none.waitUntil(start + milliseconds(1)), start + milliseconds(1));
		FramePacer pacer;
		pacer.start(0);
		EXPECT_FALSE(pacer.paced());
	}

	TEST(FramePacing, OnlyRealTimeIsPaced) {
		EngineConfig config;
		EXPECT_EQ(pacedFps(config), config.maxFps);
		config.maxFps = 0;
		EXPECT_EQ(pacedFps(config), 0);
		config.maxFps = 60;
		config.fixedDeltaTime = 0.016f;
		EXPECT_EQ(pacedFps(config), 0) << "a fixed step";
		config.fixedDeltaTime = 0.f;
		config.capture.enabled = true;
		EXPECT_EQ(pacedFps(config), 0) << "a capture";
	}

	// Every run whose step is the wall clock's waits on it -- its frames for the limit, its tick systems for their ticks --
	// and has the OS asked for fine naps whatever its limit: the window's 600, a bot's 60, none at all. A stepped run
	// waits on no clock, and is neither paced nor asks.
	TEST(FramePacing, FineNapsForEveryRealTimeRun) {
		EngineConfig config;
		EXPECT_TRUE(realTime(config)) << "the default, " << config.maxFps << " a second";
		config.maxFps = 60;
		EXPECT_TRUE(realTime(config)) << "60 a second";
		config.maxFps = 0;
		EXPECT_TRUE(realTime(config)) << "no limit: its ticks still wait";
		config.maxFps = 60;
		config.fixedDeltaTime = 0.016f;
		EXPECT_FALSE(realTime(config)) << "a fixed step";
		EXPECT_EQ(pacedFps(config), 0);
		config.fixedDeltaTime = 0.f;
		config.capture.enabled = true;
		EXPECT_FALSE(realTime(config)) << "a capture";
		EXPECT_EQ(pacedFps(config), 0);
	}
}
