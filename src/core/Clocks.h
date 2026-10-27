#pragma once

#include <chrono>
#include <future>

namespace SFE::Clocks {
	// The two clocks a frame goes by. The wall clock (Engine::clockSeconds) is left to what deals with the world outside
	// the game: the network, sound, timeouts, timings.
	//
	// The simulation's: the time the world is at. When frames step the world (a fixed step, or a capture) it is the sum
	// of their steps, so frame N is the same world on every run; when the world runs in real time, the wall clock.
	// Physics stamps its steps with it, and what it moves is drawn at a moment of it. It is never held.
	double simulation();
	// Whether frames step the world this frame (a fixed step, or a capture). Then how long work takes never decides what
	// a frame does: a Budget never runs out, and work in flight is waited for where its result is looked for (ready).
	bool stepped();

	// A frame's share of work spread over frames: at most `ms` milliseconds of the wall clock from when it is made, while
	// the world runs in real time. When frames step it, it never runs out -- what bounds the work then is how much of it
	// is due and the work's own counts (so many a frame, so many iterations), the same on every run.
	class Budget {
	public:
		explicit Budget(double ms);
		// Whether there is time left for more.
		bool left() const;
		// When it runs out, for work that hands the deadline on: never (time_point::max), when frames step the world.
		std::chrono::steady_clock::time_point until() const { return mUntil; }

	private:
		std::chrono::steady_clock::time_point mUntil;
	};

	// On the main thread, what work on other threads waits on it for (ThreadPool's SYNC tasks: what goes to the device);
	// elsewhere nothing.
	void serve();
	// Whether the work behind `job` (begun: valid) is done, to take its result this frame. In real time, once it is done;
	// when frames step the world, it is waited for, so the result lands on the first frame that looks for it after the
	// work began, the same on every run -- the main thread meanwhile doing what the work waits on it for (serve).
	template <class Future>
	bool ready(const Future& job) {
		if (!stepped()) {
			return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		}
		while (job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
			serve();
		}
		return true;
	}

	// The animation clock (Engine::animationClock): what only moves on screen goes by it -- sway, flicker, clouds,
	// water, smoke, skinned clips. It follows the frames unless held.
	// Held at `seconds` from the next frame begun until released: everything drawn by it stands still, alike on every
	// run, and on that frame the pictures made over several frames (TAA, exposure, far shadows, particles) start again.
	// Released, it goes on from where it would have been.
	void hold(double seconds);
	void release();
	bool held();
	// How far the animation clock went on this frame: 0 while it is held, the jump onto a hold and off it not counted.
	// What moves by steps of it (particles, sparks let go) stands still while it is held.
	float step();

	// A picture meant to come out the same on every run: it leaves out what differs from one run to the next (timings,
	// panels) and waits for work in flight. A frame capture, or any frame while the animation clock is held.
	bool stillPicture();
	// Whether the pictures made over several frames start again this frame: the first frame of a hold.
	bool restarting();

	// The engine's, once a frame before anything of it runs: the simulation's time when frames step it (else negative),
	// and how far the animation clock goes on this frame. What the animation clock reads this frame.
	double beginFrame(double steppedTime, double advance);
}
