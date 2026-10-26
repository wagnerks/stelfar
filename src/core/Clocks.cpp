#include "Clocks.h"

#include <atomic>

#include "core/Engine.h"

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
