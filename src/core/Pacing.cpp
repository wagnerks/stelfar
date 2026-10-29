#include "core/Pacing.h"

#include "core/EngineConfig.h"

#include <algorithm>
#include <cmath>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace SFE {
	void DeadlineWaiter::waitUntil(const Clock::time_point deadline) {
		using Seconds = std::chrono::duration<double>;
		while (true) {
			const auto now = Clock::now();
			if (Seconds(deadline - now).count() <= mNapEstimate) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			observe(Seconds(Clock::now() - now).count());
		}
		while (Clock::now() < deadline) {
			std::this_thread::yield();
		}
	}

	void DeadlineWaiter::observe(const double seconds) {
		constexpr double kWeight = 0.05;
		const double delta = seconds - mMean;
		mMean += kWeight * delta;
		mVariance = (1.0 - kWeight) * (mVariance + kWeight * delta * delta);
		mNapEstimate = mMean + 2.0 * std::sqrt(mVariance);
	}

	FrameSchedule::FrameSchedule(const int fps, const Clock::time_point start)
		: mPeriod(fps > 0 ? std::chrono::round<Clock::duration>(std::chrono::duration<double>(1.0 / fps)) : Clock::duration::zero()),
		  mNext(start + mPeriod) {}

	FrameSchedule::Clock::time_point FrameSchedule::waitUntil(const Clock::time_point now) {
		if (!limited()) {
			return now;
		}
		if (now < mNext) {
			const auto until = mNext;
			mNext += mPeriod;
			return until;
		}
		mNext = now + mPeriod;
		return now;
	}

	bool FrameSchedule::limited() const {
		return mPeriod > Clock::duration::zero();
	}

	FrameSchedule::Clock::duration FrameSchedule::period() const {
		return mPeriod;
	}

	void FramePacer::start(const int fps) {
		mSchedule = FrameSchedule(fps, FrameSchedule::Clock::now());
	}

	bool FramePacer::paced() const {
		return mSchedule.limited();
	}

	void FramePacer::endFrame() {
		const auto now = FrameSchedule::Clock::now();
		const auto until = mSchedule.waitUntil(now);
		if (until > now) {
			mWaiter.waitUntil(until);
		}
	}

	bool realTime(const EngineConfig& config) {
		return !(config.fixedDeltaTime > 0.f || config.capture.enabled);
	}

	int pacedFps(const EngineConfig& config) {
		return realTime(config) ? (std::max)(0, config.maxFps) : 0;
	}

	void askFineNaps() {
#if defined(_WIN32)
		struct FineNaps {
			FineNaps() { timeBeginPeriod(1); }
			~FineNaps() { timeEndPeriod(1); }
		};
		static FineNaps naps;
#endif
	}
}
