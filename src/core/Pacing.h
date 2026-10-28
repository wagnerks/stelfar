#pragma once

#include <chrono>

// How the engine waits: for a tick of a tick system (SystemManager) and for the end of a frame (Engine::update), with one
// waiter. Included by Engine.cpp, SystemManager.cpp and their tests only.
namespace SFE {
	struct EngineConfig;

	// Waits until a deadline as closely as the platform allows, without asking anything of the platform: it sleeps in
	// short naps while more time is left than a nap has been seen to overrun, and yields for the rest. The overrun is
	// measured as it goes, so where the OS wakes a thread on time almost all of the wait is sleep, and where it wakes it
	// in coarse slices (Windows, ~15.6 ms by default; askFineNaps) the last slice is spent yielding.
	class DeadlineWaiter {
	public:
		using Clock = std::chrono::steady_clock;
		// What a nap is taken to cost before one has been seen: no nap is begun with less than this left.
		static constexpr double kFirstNapEstimate = 0.002;

		void waitUntil(Clock::time_point deadline);

	private:
		// A running mean and variance of how long a 1 ms nap really takes; the estimate keeps two deviations of margin,
		// so a nap rarely carries the thread past its deadline.
		void observe(double seconds);

		double mMean = kFirstNapEstimate;
		double mVariance = 0.0;
		double mNapEstimate = kFirstNapEstimate;
	};

	// When frames end, at most `fps` a second: on a grid of 1 / fps from where it starts. A frame that ends before its
	// slot waits for the slot's end; one that ends late waits for nothing, and the grid starts again from it. A frame is
	// not owed, as a tick is (SystemManager), so a slow one is not made up by a burst of quick ones after it. Pure: the
	// moments are given, nothing is read from a clock.
	class FrameSchedule {
	public:
		using Clock = std::chrono::steady_clock;

		// No limit: no frame waits.
		FrameSchedule() = default;
		// 0 or less: no limit.
		FrameSchedule(int fps, Clock::time_point start);

		// The frame that ends at `now`: the moment it waits for (`now` itself when it waits for none); the next slot set.
		Clock::time_point waitUntil(Clock::time_point now);
		bool limited() const;
		Clock::duration period() const;

	private:
		Clock::duration mPeriod = Clock::duration::zero();
		Clock::time_point mNext{};
	};

	// The frame limit: a FrameSchedule's waits, waited out by a DeadlineWaiter.
	class FramePacer {
	public:
		// The grid from now; 0 or less: frames are not paced.
		void start(int fps);
		bool paced() const;
		// At the end of a frame: the rest of its slot waited out.
		void endFrame();

	private:
		FrameSchedule mSchedule;
		DeadlineWaiter mWaiter;
	};

	// The frames a second a run is held to: EngineConfig::maxFps while the step is the wall clock's; none (0) under a
	// fixed step or a capture, whose frames go as fast as they can.
	int pacedFps(const EngineConfig& config);
	// Whether a frame of 1 / fps seconds has room to nap in: longer than a nap is first taken to cost. False for none.
	bool napsFit(int fps);
	// The OS's timer at 1 ms for the rest of the process, so naps are a millisecond and not a slice (timeBeginPeriod on
	// Windows, given back at exit; nothing elsewhere). Asked once however often it is called.
	void askFineNaps();
}
