#include "SystemManager.h"
#include "core/Engine.h"

#include "systemsModule/SystemBase.h"
#include "debugModule/Benchmark.h"
#include "debugModule/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace ecss {
	namespace {
		using Clock = std::chrono::steady_clock;

		// Waits until a deadline as closely as the platform allows, without asking anything of the platform: it sleeps
		// in short naps while more time is left than a nap has been seen to overrun, and yields for the rest. The
		// overrun is measured as it goes, so where the OS wakes a thread on time almost all of the wait is sleep, and
		// where it wakes it in coarse slices (Windows, ~15.6 ms by default) the last slice is spent yielding.
		class DeadlineWaiter {
		public:
			void waitUntil(Clock::time_point deadline) {
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

		private:
			// A running mean and variance of how long a 1 ms nap really takes; the estimate keeps two deviations of
			// margin, so a nap rarely carries the thread past its deadline.
			void observe(double seconds) {
				constexpr double kWeight = 0.05;
				const double delta = seconds - mMean;
				mMean += kWeight * delta;
				mVariance = (1.0 - kWeight) * (mVariance + kWeight * delta * delta);
				mNapEstimate = mMean + 2.0 * std::sqrt(mVariance);
			}

			double mMean = 0.002;
			double mVariance = 0.0;
			double mNapEstimate = 0.002;
		};
	}

	// One thread keeps the time for every tick system; each system runs on a thread of its own that sleeps on a
	// condition variable until its tick is due. Waiting precisely costs the waiting thread (see DeadlineWaiter), and
	// there is one of those however many systems tick; a woken worker costs nothing until it works.
	//
	// The schedule is absolute: a tick is due one period after the one before, whenever that one ran, so the rate holds
	// on average whatever the waits overrun, and every update gets exactly one period as its step. A system that falls
	// behind runs the ticks it owes back to back, up to a few, and lets the rest go rather than spiral.
	class SystemManager::TickScheduler {
	public:
		~TickScheduler() {
			mAlive.store(false);
			if (mThread.joinable()) {
				mThread.join();
			}
			for (auto& worker : mWorkers) {
				{
					std::lock_guard lock(worker->mutex);
					worker->alive = false;
				}
				worker->wake.notify_one();
				worker->thread.join();
			}
		}

		// Main thread, while systems are being set up.
		void add(System* system) {
			auto worker = std::make_unique<Worker>();
			worker->system = system;
			worker->next = Clock::now();
			auto* raw = worker.get();
			size_t number = 0;
			{
				std::lock_guard lock(mWorkersMutex);
				mWorkers.push_back(std::move(worker));
				number = mWorkers.size();
			}
			raw->thread = std::thread([raw, number] {
				SFE::Debug::Profiler::nameThread("Tick", static_cast<int>(number));
				work(*raw);
			});
			if (!mThread.joinable()) {
				mThread = std::thread([this] { schedule(); });
			}
		}

	private:
		static constexpr int kMaxCatchUp = 4;

		struct Worker {
			System* system = nullptr;
			Clock::time_point next; // scheduler thread only
			std::mutex mutex;
			std::condition_variable wake;
			int owed = 0;
			bool alive = true;
			std::thread thread;
		};

		static void work(Worker& worker) {
			while (true) {
				int owed = 0;
				{
					std::unique_lock lock(worker.mutex);
					worker.wake.wait(lock, [&] { return worker.owed > 0 || !worker.alive; });
					if (!worker.alive) {
						return;
					}
					owed = std::exchange(worker.owed, 0);
				}
				const float step = 1.f / worker.system->getTicks();
				for (int i = 0; i < owed; ++i) {
					SFE_PROFILE_ZONE(typeid(*worker.system).name());
					worker.system->update(step);
				}
			}
		}

		void schedule() {
			DeadlineWaiter waiter;
			while (mAlive.load()) {
				auto earliest = Clock::time_point::max();
				{
					std::lock_guard lock(mWorkersMutex);
					for (const auto& worker : mWorkers) {
						earliest = (std::min)(earliest, worker->next);
					}
				}
				waiter.waitUntil(earliest);

				const auto now = Clock::now();
				std::lock_guard lock(mWorkersMutex);
				for (const auto& worker : mWorkers) {
					const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / worker->system->getTicks()));
					int due = 0;
					while (now >= worker->next && due < kMaxCatchUp) {
						worker->next += period;
						++due;
					}
					if (now >= worker->next) {
						worker->next = now + period;
					}
					if (due > 0) {
						{
							std::lock_guard workerLock(worker->mutex);
							worker->owed = (std::min)(worker->owed + due, kMaxCatchUp);
						}
						worker->wake.notify_one();
					}
				}
			}
		}

		std::atomic<bool> mAlive{ true };
		std::mutex mWorkersMutex;
		std::vector<std::unique_ptr<Worker>> mWorkers;
		std::thread mThread;
	};

	SystemManager::SystemManager() = default;

	SystemManager::~SystemManager() {
		mTickScheduler.reset();

		// Every system's async work must be over before the first one is deleted: a pool task still running
		// updateAsync on a deleted system crashed on exit, and a destructor may still notify the other systems.
		for (const auto system : mSystemsMap) {
			if (system) {
				system->stopAsync();
			}
		}

		for (const auto system : mSystemsMap) {
			delete system;
		}
	}

	void SystemManager::addTickSystem(System* system, float ticks) {
		system->setTick(ticks);
		if (!mStepTickSystems) {
			if (!mTickScheduler) {
				mTickScheduler = std::make_unique<TickScheduler>();
			}
			mTickScheduler->add(system);
			return;
		}
		mSteppedTickSystems.push_back({ system });
	}

	void SystemManager::update(float_t dt) {
		FUNCTION_BENCHMARK;

		for (auto& ticked : mSteppedTickSystems) {
			const float step = 1.f / ticked.system->getTicks();
			ticked.accumulated += dt;
			while (ticked.accumulated >= step) {
				SFE_PROFILE_ZONE(typeid(*ticked.system).name());
				ticked.system->update(step);
				ticked.accumulated -= step;
			}
		}

		for (const auto system : mMainThreadSystems) {
			SFE_PROFILE_ZONE(typeid(*system).name());
			system->update(dt);
		}

		SFE_PROFILE_ZONE("Debug updates");
		for (auto system : mSystemsMap) {
			if (system) { // slots are type-indexed, so gaps are normal
				system->debugUpdate(dt);
			}
		}
	}
}