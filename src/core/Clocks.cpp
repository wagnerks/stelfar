#include "Clocks.h"

#include <atomic>

#include "core/Engine.h"
#include "multithreading/ThreadPool.h"

namespace SFE::Clocks {
	namespace {
		std::atomic<double> gStepped{ -1.0 };
		// Where the animation clock would be without holds: it goes on underneath one.
		double gRunning = 0.0;
		std::atomic<bool> gHeld{ false };
		std::atomic<double> gHeldAt{ 0.0 };
		std::atomic<bool> gRestartDue{ false };
		bool gRestarting = false;
		float gStep = 0.f;
	}

	double simulation() {
		const double stepped = gStepped.load(std::memory_order_relaxed);
		return stepped >= 0.0 ? stepped : Engine::clockSeconds();
	}

	bool stepped() {
		return gStepped.load(std::memory_order_relaxed) >= 0.0;
	}

	Budget::Budget(double ms)
		: mUntil(stepped() ? std::chrono::steady_clock::time_point::max()
			: std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(ms))) {
	}

	bool Budget::left() const {
		return mUntil == std::chrono::steady_clock::time_point::max() || std::chrono::steady_clock::now() < mUntil;
	}

	void serve() {
		if (Engine::isMainThread()) {
			ThreadPool::instance()->syncUpdate();
		}
	}

	void hold(double seconds) {
		gHeldAt.store(seconds);
		gHeld.store(true);
		gRestartDue.store(true);
	}

	void release() {
		gHeld.store(false);
	}

	bool held() {
		return gHeld.load();
	}

	float step() {
		return gStep;
	}

	bool stillPicture() {
		return held() || Engine::instance()->capturingFrame();
	}

	bool restarting() {
		return gRestarting;
	}

	double beginFrame(double steppedTime, double advance) {
		gStepped.store(steppedTime, std::memory_order_relaxed);
		gRestarting = gRestartDue.exchange(false);
		gRunning += advance;
		const bool still = held();
		gStep = still ? 0.f : static_cast<float>(advance);
		return still ? gHeldAt.load() : gRunning;
	}
}
